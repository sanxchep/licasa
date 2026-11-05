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
from source_cache import add_source_arguments, fetch_archive
from runtime_profiles import PROFILES, add_runtime_profile_argument, configured_qt_version

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_source_arguments(parser)
    add_runtime_profile_argument(parser)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/deps")
    parser.add_argument("--qt-prefix", type=Path)
    parser.add_argument("--install-module", action="store_true",
                        help="Build and install all of patched Qt Declarative into --qt-prefix")
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
    if args.install_module and not args.qt_prefix:
        parser.error("--install-module requires --qt-prefix")
    work = args.work_dir.resolve()
    lock = PROFILES[args.runtime_profile]["quick_lock"]
    if lock is None:
        parser.error("This profile uses the audited same-version distro Qt Quick; no backport is required")
    spec_path = ROOT / lock
    spec = json.loads(spec_path.read_text())
    downloads = args.source_cache.resolve() / "archives" if args.source_cache else work / "downloads"
    archive = fetch_archive(downloads, spec["url"], spec["sha256"], offline=args.offline,
                            limit=128 * 1024 * 1024)
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"qtdeclarative-everywhere-src-{spec['version']}"
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter="data")
    if args.prepare_only:
        print(f"Verified Qt Quick sources: {work}")
        return
    patch = ROOT / spec["patch"]
    command = ["patch", "--batch", "-p1", "-i", str(patch)]
    if subprocess.run(command + ["--forward", "--dry-run"], cwd=source,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        subprocess.run(command + ["--forward"], cwd=source, check=True)
    else:
        subprocess.run(command + ["--reverse", "--dry-run"], cwd=source, check=True)
    build = work / "qt-backport-build"
    env = dict(os.environ)
    destination = args.qt_prefix.resolve() if args.install_module else work / "qt-backport-prefix"
    command = ["cmake", "-S", str(source), "-B", str(build),
               f"-DCMAKE_INSTALL_PREFIX={destination}",
               "-DCMAKE_BUILD_TYPE=Release", "-DQT_BUILD_EXAMPLES=OFF",
               "-DQT_BUILD_TESTS=OFF", "-DQT_BUILD_BENCHMARKS=OFF",
               "-DCMAKE_CXX_FLAGS=-fstack-protector-strong",
               "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,relro,-z,now,-z,noexecstack"]
    if args.qt_prefix:
        qt_prefix = args.qt_prefix.resolve(strict=True)
        command.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")
        # The unpacked qsb executable needs its matching ShaderTools library.
        library_dirs = [qt_prefix / "lib", *sorted((qt_prefix / "lib").glob("*-linux-gnu"))]
        env["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, library_dirs))
    subprocess.run(command, env=env, check=True)
    if configured_qt_version((build / "CMakeCache.txt").read_text()) != spec["version"]:
        raise RuntimeError(f"Private Qt Quick requires exact Qt {spec['version']}")
    target = [] if args.install_module else ["--target", "Quick"]
    subprocess.run(["cmake", "--build", str(build), *target, "--parallel", str(args.jobs)],
                   env=env, check=True)
    if args.install_module:
        subprocess.run(["cmake", "--install", str(build)], env=env, check=True)
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
    (runtime / "build.json").write_text(json.dumps({
        "runtime_profile": args.runtime_profile, "qt_version": spec["version"],
        "library_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "source_sha256": spec["sha256"],
        "patches": {spec["patch"]: hashlib.sha256(patch.read_bytes()).hexdigest()},
        "build_target": "Quick", "configure": command,
    }, indent=2) + "\n")
    print(f"Verified Qt Quick runtime: {output}")


if __name__ == "__main__":
    main()
