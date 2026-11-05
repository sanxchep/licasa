#!/usr/bin/env python3
"""Build the pinned private FFmpeg plugin for an exact distro Qt runtime.

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
from source_cache import add_source_arguments, fetch_archive
from runtime_profiles import (PROFILES, add_runtime_profile_argument, profile_work_dir,
                              configured_qt_version, verify_ffmpeg_sonames)
from verify_qt_ffmpeg_wakeup import qualify as qualify_demuxer_wakeup

ROOT = Path(__file__).resolve().parents[1]


def apply(source, patch):
    command = ["patch", "--batch", "-p1", "-i", str(patch)]
    if subprocess.run(command + ["--forward", "--dry-run"], cwd=source,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
        subprocess.run(command + ["--forward"], cwd=source, check=True)
    else:
        subprocess.run(command + ["--reverse", "--dry-run"], cwd=source, check=True)


def audit_upstream_fixes(source, spec, downloads, *, offline=False):
    """Revalidate reviewed upstream fixes before using an unpatched subsystem."""
    for name, expected in spec.get("verified_upstream_files", {}).items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f"Reviewed upstream Qt fix changed: {name}")
    quick = spec.get("quick_source_audit")
    if quick:
        archive = fetch_archive(downloads, quick["url"], quick["sha256"], offline=offline,
                                limit=128 * 1024 * 1024)
        name = f"qtdeclarative-everywhere-src-{spec['version']}/{quick['file']}"
        with tarfile.open(archive) as bundle:
            with bundle.extractfile(name) as content:
                actual = hashlib.sha256(content.read()).hexdigest()
        if actual != quick["file_sha256"]:
            raise RuntimeError("Reviewed upstream Qt Quick texture cleanup changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_source_arguments(parser)
    add_runtime_profile_argument(parser)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/deps")
    parser.add_argument("--build-dir", type=Path,
                        help="Optional existing build tree for this exact pinned source")
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
    work = profile_work_dir(args.work_dir.resolve(), args.runtime_profile)
    lock = ROOT / PROFILES[args.runtime_profile]["ffmpeg_lock"]
    spec = json.loads(lock.read_text())
    downloads = args.source_cache.resolve() / "archives" if args.source_cache else work / "downloads"
    archive = fetch_archive(downloads, spec["url"], spec["sha256"], offline=args.offline,
                            limit=64 * 1024 * 1024)
    work.mkdir(parents=True, exist_ok=True)
    source = work / f"qtmultimedia-everywhere-src-{spec['version']}"
    if not source.exists():
        with tarfile.open(archive) as bundle:
            bundle.extractall(work, filter="data")
    patches = []
    distro = work / "qt-multimedia-debian"
    distro.mkdir(exist_ok=True)
    distro_url = spec.get("ubuntu_patch_url", spec.get("debian_patch_url"))
    distro_sha256 = spec.get("ubuntu_patch_sha256", spec.get("debian_patch_sha256"))
    distro_archive = fetch_archive(downloads, distro_url, distro_sha256, offline=args.offline,
                                   limit=64 * 1024 * 1024)
    with tarfile.open(distro_archive) as bundle:
        bundle.extractall(distro, filter="data")
    distro_patches = ([spec["ubuntu_abi_patch"]] if "ubuntu_abi_patch" in spec
                       else spec["debian_patches"])
    patches.extend(distro / patch for patch in distro_patches)
    patches.extend(ROOT / p for p in spec["patches"])
    audit_upstream_fixes(source, spec, downloads, offline=args.offline)
    if args.prepare_only:
        print(f"Verified Qt Multimedia sources: {work}")
        return
    for patch in patches:
        apply(source, patch)
    variant = args.sanitizer or "release"
    build = args.build_dir.resolve() if args.build_dir else work / f"qt-ffmpeg-build-{variant}"
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
    for feature in spec.get("required_features", []):
        command.append(f"-DFEATURE_{feature}=ON")
    for feature in spec.get("disabled_features", []):
        command.append(f"-DFEATURE_{feature}=OFF")
    if spec.get("required_features"):
        # Development packages can be added between local builds. Re-run the
        # dependency probes rather than retaining an earlier failed test.
        command.extend(["-UHAVE_EGL", "-UTEST_linux_dmabuf"])
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
    # Private Qt APIs and FFmpeg SONAMEs are part of this profile's contract.
    # Read the *configured* versions, not host tools from a different prefix.
    cache = (build / "CMakeCache.txt").read_text()
    if configured_qt_version(cache) != spec["version"]:
        raise RuntimeError(f"Runtime profile requires exact Qt {spec['version']}")
    wakeup_qualification = None
    if args.runtime_profile == "ubuntu-6.4.2":
        wakeup_qualification = qualify_demuxer_wakeup(
            source, work / "qt-ffmpeg-wakeup-qualification", jobs=args.jobs,
            environment=env, prefixes=prefixes)
    subprocess.run(["cmake", "--build", str(build), "--target", "QFFmpegMediaPlugin",
                    "--parallel", str(args.jobs)], env=env, check=True)
    plugins = [*(build / "lib").rglob("libffmpegmediaplugin.so"),
               *(build / "plugins").glob("multimedia/libffmpegmediaplugin.so")]
    if len(plugins) != 1:
        raise RuntimeError(f"Expected one FFmpeg plugin, found {plugins}")
    dynamic = subprocess.check_output(["readelf", "-d", str(plugins[0])], text=True)
    ffmpeg_sonames = verify_ffmpeg_sonames(dynamic, args.runtime_profile)
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
        "qt_version": spec["version"], "runtime_profile": args.runtime_profile,
        "distro_patch_sha256": distro_sha256, "distro_patches": distro_patches,
        "required_features": spec.get("required_features", []),
        "disabled_features": spec.get("disabled_features", []),
        "ffmpeg_sonames": ffmpeg_sonames,
        "source_sha256": spec["sha256"],
        "patches": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
                    for p in spec["patches"]},
        "bytes": output.stat().st_size, "configure": command,
        "wakeup_qualification": wakeup_qualification,
    }, indent=2) + "\n")
    print(f"Private FFmpeg runtime ({variant}): {output}")


if __name__ == "__main__":
    main()
