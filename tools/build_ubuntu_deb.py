#!/usr/bin/env python3
"""Build the qualified full-featured Ubuntu 24.04 amd64 Debian package."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def run(*command: str) -> None:
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/deb-release")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/qualification/deb")
    parser.add_argument("--qt-imageformats-dir", type=Path,
                        help="Matching Ubuntu Qt TIFF/WebP plugins, if unpacked locally")
    parser.add_argument("--qt-imageformats-license", type=Path,
                        help="Copyright file for those Qt plugins, if unpacked locally")
    parser.add_argument("--skip-dependencies", action="store_true",
                        help="Reuse already verified local private dependency builds")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    system = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines()
                  if "=" in line)
    if (system.get("ID", "").strip('"') != "ubuntu"
            or system.get("VERSION_ID", "").strip('"') != "24.04"
            or platform.machine() != "x86_64"):
        parser.error("The qualified target is Ubuntu 24.04 amd64")
    required = ("cmake", "ninja", "meson", "nasm", "patch", "git", "pkg-config",
                "dpkg-architecture", "dpkg-shlibdeps", "dpkg-deb", "ctest")
    missing = [command for command in required if shutil.which(command) is None]
    if missing:
        parser.error("Missing build tools: " + ", ".join(missing)
                     + "; install the packages in packaging/build-deb.Dockerfile")

    python = sys.executable
    jobs = str(args.jobs)
    if not args.skip_dependencies:
        run(python, "tools/build_codecs.py", "--jobs", jobs)
        run(python, "tools/build_qt_backport.py", "--jobs", jobs)
        run(python, "tools/build_qt_ffmpeg.py", "--jobs", jobs)
    codec_prefix = ROOT / "build/codecs/release-prefix"
    quick = ROOT / "build/deps/qt-backport-runtime/libQt6Quick.so.6.4.2"
    ffmpeg = ROOT / "build/deps/qt-ffmpeg-runtime-release/libffmpegmediaplugin.so"
    for path in (codec_prefix, quick, ffmpeg):
        if not path.exists():
            parser.error(f"Missing private build output: {path}")
    multiarch = subprocess.check_output(["dpkg-architecture", "-qDEB_HOST_MULTIARCH"],
                                        text=True).strip()
    image_plugins = args.qt_imageformats_dir or Path("/usr/lib") / multiarch / "qt6/plugins/imageformats"
    image_license = args.qt_imageformats_license or Path("/usr/share/doc/qt6-image-formats-plugins/copyright")
    for path in (image_plugins / "libqtiff.so", image_plugins / "libqwebp.so", image_license):
        if not path.is_file():
            parser.error(f"Missing system Qt image plugin or license: {path}")

    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    run("cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=/usr",
        "-DCMAKE_INSTALL_LIBDIR=lib", "-DBUILD_TESTING=ON",
        "-DLICASA_BUILD_DIAGNOSTICS=ON", "-DLICASA_ENABLE_HEIF=ON",
        "-DLICASA_ENABLE_JXL=ON", "-DLICASA_ENABLE_RAW=ON",
        "-DLICASA_ENABLE_AVIF=ON", "-DLICASA_ENABLE_APNG=ON",
        "-DLICASA_ENABLE_JP2=ON", "-DLICASA_ENABLE_MOTION_PLAYBACK=ON",
        f"-DCMAKE_PREFIX_PATH={codec_prefix}",
        f"-DLICASA_QT_QUICK_BACKPORT_LIBRARY={quick}",
        f"-DLICASA_QT_FFMPEG_PLUGIN={ffmpeg}",
        f"-DLICASA_QT_EXTRA_IMAGEFORMATS_DIR={image_plugins}",
        f"-DLICASA_QT_EXTRA_IMAGEFORMATS_LICENSE={image_license}")
    run("cmake", "--build", str(build), "--parallel", jobs)
    # Snapcraft's SDK can put its own (older) libheif ahead of the private
    # codec build. Run the build tests with the same private codecs that are
    # installed into the package, without changing package dependency scans.
    test_environment = os.environ.copy()
    test_environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
        str(quick.parent), str(codec_prefix / "lib"),
        test_environment.get("LD_LIBRARY_PATH"),
    )))
    print("+ ctest --test-dir " + str(build) + " --output-on-failure", flush=True)
    subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"],
                   cwd=ROOT, env=test_environment, check=True)
    run(python, "tools/package_deb.py", "--build-dir", str(build),
        "--output-dir", str(output))


if __name__ == "__main__":
    main()
