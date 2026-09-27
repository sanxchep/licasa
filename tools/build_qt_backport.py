#!/usr/bin/env python3
"""Build only the pinned Qt 6.4.2 Quick runtime with its texture cleanup fix.

Ubuntu 24.04 needs qt6-base-private-dev, qt6-declarative-private-dev and
qt6-shadertools-dev in addition to the normal application build dependencies.
--qt-prefix also accepts an unpacked development prefix; no system install is made.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/deps")
    parser.add_argument("--qt-prefix", type=Path)
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
    spec_path = ROOT / "packaging/qt-backport.lock.json"
    spec = json.loads(spec_path.read_text())
    archive = work / "downloads" / spec["url"].rsplit("/", 1)[1]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        temporary = archive.with_suffix(".partial")
        try:
            with urlopen(spec["url"], timeout=60) as response, temporary.open("wb") as output:
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > 128 * 1024 * 1024:
                        raise RuntimeError("Qt source archive exceeded download bound")
                    output.write(block)
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    with archive.open("rb") as data:
        if hashlib.file_digest(data, "sha256").hexdigest() != spec["sha256"]:
            raise RuntimeError("Qt source checksum mismatch")
    source = work / f"qtdeclarative-everywhere-src-{spec['version']}"
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter="data")
    patch = ROOT / spec["patch"]
    command = ["patch", "--batch", "-p1", "-i", str(patch)]
    if subprocess.run(command + ["--forward", "--dry-run"], cwd=source,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        subprocess.run(command + ["--forward"], cwd=source, check=True)
    else:
        subprocess.run(command + ["--reverse", "--dry-run"], cwd=source, check=True)
    build = work / "qt-backport-build"
    env = dict(os.environ)
    command = ["cmake", "-S", str(source), "-B", str(build),
               f"-DCMAKE_INSTALL_PREFIX={work / 'qt-backport-prefix'}",
               "-DCMAKE_BUILD_TYPE=Release", "-DQT_BUILD_EXAMPLES=OFF",
               "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_BENCHMARKS=OFF",
               "-DFEATURE_quick3d=OFF", "-DCMAKE_CXX_FLAGS=-fstack-protector-strong",
               "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,relro,-z,now,-z,noexecstack"]
    if args.qt_prefix:
        qt_prefix = args.qt_prefix.resolve(strict=True)
        command.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")
        # The unpacked qsb executable needs its matching ShaderTools library.
        library_dirs = [qt_prefix / "lib", *sorted((qt_prefix / "lib").glob("*-linux-gnu"))]
        env["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, library_dirs))
    subprocess.run(command, env=env, check=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "Quick",
                    "--parallel", str(args.jobs)], env=env, check=True)
    name = f"libQt6Quick.so.{spec['version']}"
    libraries = list((build / "lib").rglob(name))
    if len(libraries) != 1:
        raise RuntimeError(f"Expected one built {name}, found {libraries}")
    runtime = work / "qt-backport-runtime"
    runtime.mkdir(exist_ok=True)
    output = runtime / name
    shutil.copy2(libraries[0], output)
    # Do not let this library pick up Qml or other runtimes from the build tree.
    # CMake bracket arguments keep paths literal, including '$' and semicolons.
    script = runtime / "remove-build-rpath.cmake"
    if "]=]" in str(output):
        raise RuntimeError("Unsupported path delimiter in build directory")
    script.write_text(f"file(RPATH_REMOVE FILE [=[{output}]=])\n")
    try:
        subprocess.run(["cmake", "-P", str(script)], check=True)
    finally:
        script.unlink(missing_ok=True)
    link = runtime / "libQt6Quick.so.6"
    link.unlink(missing_ok=True)
    link.symlink_to(name)
    licenses = runtime / "licenses"
    shutil.copytree(source / "LICENSES", licenses, dirs_exist_ok=True)
    shutil.copy2(spec_path, licenses / "sources.json")
    shutil.copy2(patch, licenses / patch.name)
    print(f"Verified Qt Quick runtime: {output}")


if __name__ == "__main__":
    main()
