#!/usr/bin/env python3
"""Build a private Qt 6.4.2 FFmpeg plugin for Ubuntu 24.04; no system installation.

Needs Qt private headers, ShaderTools, PulseAudio and FFmpeg development packages.
Only the plugin is staged: the installed Qt Multimedia library stays in use.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]


def fetch(work, url, digest):
    archive = work / "downloads" / url.rsplit("/", 1)[1]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        try:
            with urlopen(url, timeout=60) as response, partial.open("wb") as output:
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > 64 * 1024 * 1024:
                        raise RuntimeError("Source archive exceeded the download bound")
                    output.write(block)
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    with archive.open("rb") as data:
        if hashlib.file_digest(data, "sha256").hexdigest() != digest:
            raise RuntimeError(f"Source checksum mismatch: {archive}")
    return archive


def apply(source, patch):
    command = ["patch", "--batch", "-p1", "-i", str(patch)]
    if subprocess.run(command + ["--forward", "--dry-run"], cwd=source,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        subprocess.run(command + ["--forward"], cwd=source, check=True)
    else:
        subprocess.run(command + ["--reverse", "--dry-run"], cwd=source, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/deps")
    parser.add_argument("--qt-prefix", type=Path)
    parser.add_argument("--development-prefix", type=Path,
                        help="Unpacked FFmpeg/PulseAudio headers and shared-library links")
    parser.add_argument("--sanitizer", choices=("address", "undefined"))
    parser.add_argument(
        "--jobs",
        type=int,
        default=int(
            os.environ.get(
                "LICASA_MAX_JOBS",
                os.cpu_count() or 1,
            )
        ),
        help=(
            "parallel build jobs "
            "(default: LICASA_MAX_JOBS or all logical CPUs)"
        ),
    )
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be at least 1")
    work = args.work_dir.resolve()
    lock = ROOT / "packaging/qt-ffmpeg.lock.json"
    spec = json.loads(lock.read_text())
    archive = fetch(work, spec["url"], spec["sha256"])
    source = work / f"qtmultimedia-everywhere-src-{spec['version']}"
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter="data")
    distro = work / "qt-multimedia-debian"
    distro.mkdir(exist_ok=True)
    distro_archive = fetch(work, spec["ubuntu_patch_url"], spec["ubuntu_patch_sha256"])
    with tarfile.open(distro_archive) as bundle:
        bundle.extractall(distro, filter="data")
    patches = [distro / spec["ubuntu_abi_patch"], *[ROOT / p for p in spec["patches"]]]
    for patch in patches:
        apply(source, patch)
    variant = args.sanitizer or "release"
    build = work / f"qt-ffmpeg-build-{variant}"
    runtime = work / f"qt-ffmpeg-runtime-{variant}"
    flags = "-fstack-protector-strong"
    linker = "-Wl,-z,relro,-z,now,-z,noexecstack"
    if args.sanitizer:
        flags += f" -fsanitize={args.sanitizer} -fno-omit-frame-pointer -fno-sanitize-recover=all"
        linker += f" -fsanitize={args.sanitizer} -fno-sanitize-recover=all"
    command = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
               "-DCMAKE_BUILD_TYPE=RelWithDebInfo" if args.sanitizer else "-DCMAKE_BUILD_TYPE=Release",
               "-DQT_BUILD_EXAMPLES=OFF", "-DQT_BUILD_TESTS=OFF",
               "-DQT_BUILD_BENCHMARKS=OFF", "-DFEATURE_spatialaudio=OFF",
               "-DFEATURE_gstreamer=OFF", "-DFEATURE_ffmpeg=ON", "-DFEATURE_pulseaudio=ON",
               f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_SHARED_LINKER_FLAGS={linker}",
               f"-DCMAKE_MODULE_LINKER_FLAGS={linker}"]
    prefixes = []
    env = dict(os.environ)
    if args.qt_prefix:
        prefix = args.qt_prefix.resolve(strict=True)
        prefixes.append(str(prefix))
        env["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, [prefix / "lib", *sorted((prefix / "lib").glob("*-linux-gnu"))]))
    if args.development_prefix:
        prefix = args.development_prefix.resolve(strict=True)
        prefixes.append(str(prefix))
        command += [f"-DFFMPEG_DIR={prefix}", f"-DPULSEAUDIO_INCLUDE_DIR={prefix / 'include'}"]
        pulse = list((prefix / "lib").rglob("libpulse.so"))
        if len(pulse) != 1:
            raise RuntimeError("Expected one PulseAudio shared-library development link")
        command += [f"-DPULSEAUDIO_LIBRARY={pulse[0]}"]
    if prefixes:
        command += ["-DCMAKE_PREFIX_PATH=" + ";".join(prefixes)]
    subprocess.run(command, env=env, check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "QFFmpegMediaPlugin",
                    "--parallel", str(args.jobs)], env=env, check=True)
    plugins = list((build / "lib").rglob("libffmpegmediaplugin.so"))
    if len(plugins) != 1:
        raise RuntimeError(f"Expected one FFmpeg plugin, found {plugins}")
    runtime.mkdir(exist_ok=True)
    output = runtime / "libffmpegmediaplugin.so"
    shutil.copy2(plugins[0], output)
    script = runtime / "remove-build-rpath.cmake"
    if "]=]" in str(output):
        raise RuntimeError("Unsupported CMake path delimiter")
    script.write_text(f"file(RPATH_REMOVE FILE [=[{output}]=])\n")
    try:
        subprocess.run(["cmake", "-P", str(script)], check=True)
    finally:
        script.unlink(missing_ok=True)
    licenses = runtime / "licenses"
    shutil.copytree(source / "LICENSES", licenses, dirs_exist_ok=True)
    shutil.copy2(lock, licenses / "sources.json")
    for patch in patches:
        shutil.copy2(patch, licenses / patch.name)
    (runtime / "build.json").write_text(json.dumps({
        "variant": variant, "plugin_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "bytes": output.stat().st_size, "configure": command,
    }, indent=2) + "\n")
    print(f"Private FFmpeg runtime ({variant}): {output}")


if __name__ == "__main__":
    main()
