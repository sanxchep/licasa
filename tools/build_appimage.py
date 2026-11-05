#!/usr/bin/env python3
"""Build the x86-64 portable package in an Ubuntu 22.04 userspace."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile

from package_deb import REQUIRED_FEATURES, cache_value
from source_cache import add_source_arguments, fetch_archive

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "packaging/appimage-runtime.lock.json"
HARDENING = "-Wl,-z,relro,-z,now,-z,noexecstack"


def run(*command, cwd=ROOT, env=None):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def unpack(archive, directory):
    name = archive.name.removesuffix(".tar.xz")
    source = directory / name
    if not source.is_dir():
        with tempfile.TemporaryDirectory(dir=directory) as temporary:
            with tarfile.open(archive) as bundle:
                bundle.extractall(temporary, filter="data")
            (Path(temporary) / name).rename(source)
    return source


def build_qt(sources, work, prefix, python, dep_args, jobs, environment):
    options = {
        "qtbase": ["-DFEATURE_xcb=ON", "-DFEATURE_gtk3=ON", "-DFEATURE_glib=ON",
                   "-DFEATURE_system_jpeg=ON", "-DFEATURE_system_png=ON",
                   "-DFEATURE_system_zlib=ON", "-DFEATURE_openssl_linked=ON"],
        "qttools": ["-DFEATURE_designer=OFF", "-DFEATURE_assistant=OFF"],
    }
    for name, source in sources.items():
        if name == "qtdeclarative":
            run(python, "tools/build_qt_backport.py", "--work-dir", work,
                "--qt-prefix", prefix, "--install-module", "--jobs", jobs, *dep_args,
                env=dict(environment, CMAKE_GENERATOR="Ninja"))
            continue
        build = work / f"build-{name}"
        run("cmake", "-S", source, "-B", build, "-G", "Ninja",
            f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_PREFIX_PATH={prefix}",
            "-DCMAKE_BUILD_TYPE=Release", "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_EXAMPLES=OFF",
            "-DCMAKE_CXX_FLAGS=-fstack-protector-strong",
            f"-DCMAKE_SHARED_LINKER_FLAGS={HARDENING}", f"-DCMAKE_MODULE_LINKER_FLAGS={HARDENING}",
            *options.get(name, ()), env=environment)
        run("cmake", "--build", build, "--parallel", jobs, env=environment)
        run("cmake", "--install", build, env=environment)


def runtime_environment(work):
    qt, ffmpeg, codecs = work / "qt-sdk", work / "ffmpeg", work / "codecs/release-prefix"
    return dict(os.environ, PATH=f"{qt / 'bin'}:{os.environ['PATH']}",
                PKG_CONFIG_PATH=os.pathsep.join(str(prefix / "lib/pkgconfig") for prefix in (codecs, ffmpeg)),
                LD_LIBRARY_PATH=os.pathsep.join(str(prefix / "lib") for prefix in (qt, ffmpeg, codecs)))


def build_application(work, output, tools_dir, jobs, sources, ffmpeg_source, *, offline):
    build = work / "build-licasa"
    qt_prefix = work / "qt-sdk"
    codec_prefix = work / "codecs/release-prefix"
    ffmpeg_prefix = work / "ffmpeg"
    quick = work / "qt/qt-backport-runtime"
    ffmpeg_plugin = work / "qt/qt-ffmpeg-runtime-release/libffmpegmediaplugin.so"
    environment = runtime_environment(work)
    environment["LD_LIBRARY_PATH"] = f"{quick}:{environment['LD_LIBRARY_PATH']}"
    run("cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=/usr", "-DCMAKE_INSTALL_LIBDIR=lib",
        "-DBUILD_TESTING=ON", "-DLICASA_ENABLE_IPO=OFF",
        *(f"-D{name}=ON" for name in REQUIRED_FEATURES),
        f"-DCMAKE_PREFIX_PATH={qt_prefix};{codec_prefix};{ffmpeg_prefix}",
        f"-DLICASA_QT_QUICK_BACKPORT_LIBRARY={quick / 'libQt6Quick.so.6.4.2'}",
        f"-DLICASA_QT_FFMPEG_PLUGIN={ffmpeg_plugin}",
        f"-DLICASA_QT_EXTRA_IMAGEFORMATS_DIR={qt_prefix / 'plugins/imageformats'}", env=environment)
    run("cmake", "--build", build, "--parallel", jobs, env=environment)
    run("ctest", "--test-dir", build, "--output-on-failure", env=environment)

    output.mkdir(parents=True, exist_ok=True)
    version = cache_value((build / "CMakeCache.txt").read_text(), "CMAKE_PROJECT_VERSION")
    with tempfile.TemporaryDirectory(prefix="appimage-payload-", dir=work) as temporary:
        payload = Path(temporary)
        run("cmake", "--install", build, env=dict(environment, DESTDIR=temporary))
        licenses = payload / "usr/share/licenses/licasa/portable-runtime"
        licenses.mkdir(parents=True)
        shutil.copy2(LOCK, licenses / LOCK.name)
        for module, source in sources.items():
            shutil.copytree(source / "LICENSES", licenses / module)
        shutil.copy2(ffmpeg_source / "COPYING.LGPLv2.1", licenses / "FFmpeg-LGPL-2.1")
        run(sys.executable, "tools/package_appimage.py", "--payload", payload, "--version", version,
            "--output-dir", output, "--tools-dir", tools_dir,
            *(["--offline"] if offline else []), env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_source_arguments(parser)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tools-dir", type=Path, default=ROOT / "build/packaging-tools")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    if not args.prepare_only:
        system = platform.freedesktop_os_release()
        if ((system["ID"], system["VERSION_ID"], platform.machine()) != ("ubuntu", "22.04", "x86_64")
                or os.confstr("CS_GNU_LIBC_VERSION") != "glibc 2.35"):
            parser.error("Build the AppImage inside the Ubuntu 22.04 amd64 recipe")
    work, output = args.work_dir.resolve(), args.output_dir.resolve()
    cache = args.source_cache.resolve() if args.source_cache else work / "source-cache"
    downloads = cache / "archives"
    dep_args = ["--source-cache", cache, *(["--offline"] if args.offline else [])]
    if args.prepare_only:
        dep_args.append("--prepare-only")
    jobs = str(args.jobs)
    python = sys.executable
    lock = json.loads(LOCK.read_text())
    archives = {}
    for module, checksum in lock["qt_modules"].items():
        name = f"{module}-everywhere-src-{lock['qt_version']}.tar.xz"
        archives[module] = fetch_archive(downloads, f"{lock['qt_url']}/{name}", checksum, offline=args.offline)
    ffmpeg = lock["ffmpeg"]
    ffmpeg_archive = fetch_archive(downloads, ffmpeg["url"], ffmpeg["sha256"], offline=args.offline)
    codec_work = work / "codecs"
    codec_prefix = codec_work / "release-prefix"
    qt_work = work / "qt"
    qt_prefix = work / "qt-sdk"
    ffmpeg_prefix = work / "ffmpeg"
    run(python, "tools/build_codecs.py", "--work-dir", codec_work, "--jobs", jobs, *dep_args)
    if args.prepare_only:
        run(python, "tools/build_qt_ffmpeg.py", "--work-dir", qt_work, *dep_args)
        return

    qt_work.mkdir(parents=True, exist_ok=True)
    sources = {module: unpack(archive, qt_work) for module, archive in archives.items()}
    ffmpeg_source = unpack(ffmpeg_archive, qt_work)
    ffmpeg_build = work / "build-ffmpeg"
    ffmpeg_build.mkdir(exist_ok=True)
    ffmpeg_environment = dict(os.environ, PKG_CONFIG_PATH=str(codec_prefix / "lib/pkgconfig"))
    run(ffmpeg_source / "configure", f"--prefix={ffmpeg_prefix}", "--enable-shared",
        "--disable-static", "--disable-programs", "--disable-doc", "--disable-debug",
        "--disable-autodetect", "--enable-libdav1d", "--enable-pic",
        "--extra-cflags=-fstack-protector-strong", f"--extra-ldflags={HARDENING}",
        cwd=ffmpeg_build, env=ffmpeg_environment)
    run("make", f"-j{jobs}", cwd=ffmpeg_build)
    run("make", "install", cwd=ffmpeg_build)

    environment = runtime_environment(work)
    build_qt(sources, qt_work, qt_prefix, python, dep_args, jobs, environment)
    ffmpeg_qt_build = qt_work / "qt-ffmpeg-build-release"
    run(python, "tools/build_qt_ffmpeg.py", "--work-dir", qt_work, "--qt-prefix", qt_prefix,
        "--jobs", jobs, *dep_args, env=environment)
    run("cmake", "--build", ffmpeg_qt_build, "--parallel", jobs, env=environment)
    run("cmake", "--install", ffmpeg_qt_build, "--prefix", qt_prefix, env=environment)

    build_application(work, output, args.tools_dir.resolve(), jobs, sources, ffmpeg_source,
                      offline=args.offline)


if __name__ == "__main__":
    main()
