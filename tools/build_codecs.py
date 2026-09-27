#!/usr/bin/env python3
"""Build pinned, minimal image codecs in a user-owned prefix (no sudo)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.parse import urlparse
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
MAX_SOURCE_BYTES = 256 * 1024 * 1024


def directory_bytes(path):
    total = 0
    for entry in path.rglob("*"):
        if entry.is_file() and not entry.is_symlink():
            total += entry.stat().st_size
            if total > MAX_SOURCE_BYTES:
                break
    return total


def archive_name(name, spec):
    suffixes = "".join(Path(urlparse(spec["url"]).path).suffixes[-2:])
    if not suffixes.startswith(".tar"):
        suffixes = ".tar.gz"
    return f"{name}-{spec['version']}{suffixes}"


def prepare_archive_source(name, spec, downloads, source):
    archive = downloads / archive_name(name, spec)
    if not archive.exists():
        print(f"Downloading {name} {spec['version']}", flush=True)
        temporary = archive.with_name(archive.name + ".partial")
        try:
            with urlopen(spec["url"], timeout=60) as response, temporary.open("wb") as output:
                total = 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > MAX_SOURCE_BYTES:
                        raise RuntimeError("codec archive exceeded download bound")
                    output.write(block)
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    with archive.open("rb") as content:
        actual = hashlib.file_digest(content, "sha256").hexdigest()
    if actual != spec["sha256"]:
        raise RuntimeError(
            f"Checksum mismatch: {archive} (expected {spec['sha256']}, got {actual})"
        )
    if not source.exists() or not any(source.iterdir()):
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=downloads) as unpack:
            with tarfile.open(archive, mode="r:*") as bundle:
                bundle.extractall(unpack, filter="data")
            extracted = Path(unpack)
            if spec.get("archive_root", True):
                roots = list(extracted.iterdir())
                if len(roots) != 1 or not roots[0].is_dir():
                    raise RuntimeError(f"Unexpected source archive layout: {archive}")
                extracted = roots[0]
            shutil.copytree(extracted, source, dirs_exist_ok=True)


def git_output(source, *args):
    return subprocess.run(
        ["git", "-C", str(source), *args], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    ).stdout.strip()


def prepare_git_source(name, spec, source):
    if shutil.which("git") is None:
        raise RuntimeError(f"git is required to prepare pinned source {name}")
    commit = spec["commit"].lower()
    tree = spec["tree"].lower()
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        source.mkdir()
    if not (source / ".git").exists():
        if any(source.iterdir()):
            raise RuntimeError(f"Refusing to replace non-git source directory: {source}")
        subprocess.run(["git", "-C", str(source), "init", "--quiet"], check=True)
        subprocess.run(
            ["git", "-C", str(source), "remote", "add", "origin", spec["url"]], check=True
        )
    origin = git_output(source, "remote", "get-url", "origin").rstrip("/")
    if origin != spec["url"].rstrip("/"):
        raise RuntimeError(f"Unexpected git origin for {name}: {origin}")
    dirty = git_output(source, "status", "--porcelain", "--untracked-files=all")
    if dirty:
        raise RuntimeError(f"Refusing to overwrite dirty pinned source tree: {source}")
    have_commit = subprocess.run(
        ["git", "-C", str(source), "cat-file", "-e", f"{commit}^{{commit}}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    ).returncode == 0
    if not have_commit:
        print(f"Fetching {name} pinned commit {commit}", flush=True)
        subprocess.run(
            ["git", "-c", "fetch.fsckObjects=true", "-C", str(source), "fetch",
             "--depth=1", "--no-tags", "origin", commit],
            check=True
        )
    subprocess.run(
        ["git", "-C", str(source), "checkout", "--quiet", "--detach", "--force", commit],
        check=True
    )
    actual_commit = git_output(source, "rev-parse", "HEAD").lower()
    actual_tree = git_output(source, "rev-parse", "HEAD^{tree}").lower()
    if actual_commit != commit:
        raise RuntimeError(f"Commit mismatch for {name}: expected {commit}, got {actual_commit}")
    if actual_tree != tree:
        raise RuntimeError(f"Tree mismatch for {name}: expected {tree}, got {actual_tree}")
    if git_output(source, "status", "--porcelain", "--untracked-files=all"):
        raise RuntimeError(f"Pinned git source is unexpectedly dirty: {source}")
    if directory_bytes(source) > MAX_SOURCE_BYTES:
        raise RuntimeError(f"Pinned git source exceeded size bound: {source}")


def prepare_source(name, spec, downloads, source):
    source_type = spec.get("source_type", "archive")
    if source_type == "archive":
        prepare_archive_source(name, spec, downloads, source)
    elif source_type == "git":
        prepare_git_source(name, spec, source)
    else:
        raise RuntimeError(f"Unsupported source_type for {name}: {source_type}")


def install_jxl_decoder(build, prefix, version):
    # Upstream builds a public decoder-only target but deliberately does not
    # install it alongside libjxl (both export decoder symbols). This private
    # prefix contains only that target and its CMS, never both variants.
    library_dir = prefix / "lib"
    library_dir.mkdir(parents=True, exist_ok=True)
    for stem in ("libjxl_dec.so", "libjxl_cms.so", "libjxl_threads.so"):
        for artifact in (build / "lib").glob(stem + "*"):
            target = library_dir / artifact.name
            target.unlink(missing_ok=True)
            if artifact.is_symlink():
                target.symlink_to(artifact.readlink())
            else:
                shutil.copy2(artifact, target)
                # These targets need only system C/C++ runtimes; their
                # sibling relationship is resolved by the Qt backend's RPATH.
                # CMake's uninstalled padding can contain empty path elements,
                # which would otherwise search the current working directory.
                subprocess.run(["cmake", f"-DLICASA_CODEC_LIBRARY={target}", "-P",
                                str(ROOT / "cmake/strip_codec_rpath.cmake")], check=True)
    shutil.copytree(build / "lib/include/jxl", prefix / "include/jxl", dirs_exist_ok=True)
    pkg = library_dir / "pkgconfig"
    pkg.mkdir(exist_ok=True)
    (pkg / "libjxl_decoder.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
        "Name: libjxl_decoder\nDescription: Private JPEG XL decoder and CMS\n"
        f"Version: {version}\n"
        "Libs: -L${libdir} -ljxl_dec -ljxl_cms\nCflags: -I${includedir}\n")
    (pkg / "libjxl_threads.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
        "Name: libjxl_threads\nDescription: Private JPEG XL parallel runner\n"
        f"Version: {version}\n"
        "Libs: -L${libdir} -ljxl_threads\nCflags: -I${includedir}\n")


def install_libyuv(build, source, prefix, version):
    """Install only libyuv's shared library and headers, not its tools/static archive."""
    library_dir = prefix / "lib"
    library_dir.mkdir(parents=True, exist_ok=True)
    artifacts = list(build.glob("libyuv.so*"))
    if not artifacts:
        artifacts = [p for p in build.rglob("libyuv.so*") if "CMakeFiles" not in p.parts]
    if not artifacts:
        raise RuntimeError(f"libyuv shared library was not produced under {build}")
    for artifact in artifacts:
        target = library_dir / artifact.name
        target.unlink(missing_ok=True)
        if artifact.is_symlink():
            target.symlink_to(artifact.readlink())
        elif artifact.is_file():
            shutil.copy2(artifact, target)
            subprocess.run(["cmake", f"-DLICASA_CODEC_LIBRARY={target}", "-P",
                            str(ROOT / "cmake/strip_codec_rpath.cmake")], check=True)
    shutil.copytree(source / "include", prefix / "include", dirs_exist_ok=True)
    package_dir = library_dir / "pkgconfig"
    package_dir.mkdir(exist_ok=True)
    (package_dir / "libyuv.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n\n"
        "Name: libyuv\nDescription: Pinned private libyuv conversion runtime\n"
        f"Version: {version}\n"
        "Libs: -L${libdir} -lyuv\nCflags: -I${includedir}\n")


def build_rawspeed3(source, build, prefix, spec, jobs):
    """Build LibRaw's pinned RawSpeed3 wrapper in this codec prefix only."""
    pinned = build.parent / "rawspeed3-pinned"
    prepare_git_source("rawspeed3", spec["rawspeed3"], pinned)
    rawspeed_source = build / "rawspeed3-source"
    if not rawspeed_source.exists():
        shutil.copytree(pinned, rawspeed_source, ignore=shutil.ignore_patterns(".git"))
    patch_dir = source / "RawSpeed3/patches"
    camera_header = rawspeed_source / "src/librawspeed/metadata/CameraMetaData.h"
    camera_patch = patch_dir / "01.CameraMeta-extensibility.patch"
    # LibRaw 0.22.2 shipped this patch backwards relative to its README and
    # pinned RawSpeed commit: the wrapper derives from CameraMetaData.
    if "class CameraMetaData final {" in camera_header.read_text():
        subprocess.run(["patch", "--batch", "-p1", "--reverse", "-i", str(camera_patch)],
                       cwd=rawspeed_source, check=True)
    if "class CameraMetaData {" not in camera_header.read_text():
        raise RuntimeError("RawSpeed3 CameraMetaData must be extensible")
    for patch in sorted(patch_dir.glob("0[2-5].*.patch")):
        command = ["patch", "--batch", "-p1", "-i", str(patch)]
        forward = subprocess.run(command + ["--forward", "--dry-run"], cwd=rawspeed_source,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if forward.returncode == 0:
            subprocess.run(command + ["--forward"], cwd=rawspeed_source, check=True)
        else:
            subprocess.run(command + ["--reverse", "--dry-run"], cwd=rawspeed_source,
                           check=True, stdout=subprocess.DEVNULL)

    wrapper = build / "rawspeed3-capi"
    wrapper.mkdir(parents=True, exist_ok=True)
    for name in ("rawspeed3_capi.cpp", "rawspeed3_capi.h"):
        shutil.copy2(source / "RawSpeed3/rawspeed3_c_api" / name, wrapper / name)
    capi = wrapper / "rawspeed3_capi.cpp"
    capi_text = capi.read_text()
    old_include = "#include <../pugixml/pugixml.hpp>"
    if old_include not in capi_text:
        raise RuntimeError("Unexpected RawSpeed3 pugixml include")
    capi.write_text(capi_text.replace(old_include, "#include <pugixml.hpp>"))
    with (rawspeed_source / "data/cameras.xml").open("rb") as cameras_xml, \
            (wrapper / "cameras.cpp").open("wb") as cameras_cpp:
        subprocess.run(["sh", str(source / "RawSpeed3/rawspeed3_c_api/rsxml2c.sh")],
                       stdin=cameras_xml, stdout=cameras_cpp, check=True)

    project = build / "rawspeed3-superproject"
    project.mkdir(parents=True, exist_ok=True)
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.13)\n"
        "project(licasa_rawspeed3 LANGUAGES CXX)\n"
        "set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n"
        "add_subdirectory(\"${RAWSPEED_SOURCE}\" rawspeed-src)\n"
        "add_library(rawspeed3 SHARED\n"
        "    \"${RAWSPEED_CAPI_DIR}/rawspeed3_capi.cpp\"\n"
        "    \"${RAWSPEED_CAPI_DIR}/cameras.cpp\")\n"
        "target_include_directories(rawspeed3 PRIVATE \"${RAWSPEED_CAPI_DIR}\")\n"
        "target_link_libraries(rawspeed3 PRIVATE rawspeed "
        "rawspeed_get_number_of_processor_cores pugixml)\n"
        "set_target_properties(rawspeed3 PROPERTIES OUTPUT_NAME rawspeed3 SOVERSION 0)\n"
        "install(TARGETS rawspeed3 LIBRARY DESTINATION lib)\n"
        "install(FILES \"${RAWSPEED_CAPI_DIR}/rawspeed3_capi.h\" "
        "DESTINATION include/rawspeed3)\n")
    rawspeed_build = build / "rawspeed3-build"
    subprocess.run([
        "cmake", "-S", str(project), "-B", str(rawspeed_build),
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}",
        f"-DRAWSPEED_SOURCE={rawspeed_source}", f"-DRAWSPEED_CAPI_DIR={wrapper}",
        "-DBINARY_PACKAGE_BUILD=ON", "-DWITH_OPENMP=OFF", "-DWITH_JPEG=OFF",
        "-DWITH_ZLIB=OFF", "-DWITH_PUGIXML=ON", "-DUSE_BUNDLED_PUGIXML=ON",
        "-DALLOW_DOWNLOADING_PUGIXML=ON", "-DRAWSPEED_ENABLE_WERROR=OFF",
        "-DBUILD_TESTING=OFF", "-DBUILD_TESTS=OFF", "-DBUILD_FUZZERS=OFF",
        "-DRAWSPEED_ENABLE_DEBUG_INFO=OFF", "-DBUILD_SHARED_LIBS=OFF",
        "-DCMAKE_CXX_FLAGS=-fstack-protector-strong",
    ], check=True)
    subprocess.run(["cmake", "--build", str(rawspeed_build), "--target", "rawspeed3",
                    "--parallel", str(jobs)], check=True)
    library_dir = prefix / "lib"
    library_dir.mkdir(parents=True, exist_ok=True)
    for artifact in rawspeed_build.glob("librawspeed3.so*"):
        target = library_dir / artifact.name
        target.unlink(missing_ok=True)
        if artifact.is_symlink():
            target.symlink_to(artifact.readlink())
        else:
            shutil.copy2(artifact, target)
    if not (library_dir / "librawspeed3.so.0").is_file():
        raise RuntimeError("RawSpeed3 library was not installed")
    headers = prefix / "include/rawspeed3"
    headers.mkdir(parents=True, exist_ok=True)
    shutil.copy2(wrapper / "rawspeed3_capi.h", headers)


def build_libraw(source, build, prefix, spec, flags, env, jobs, sanitized,
                 rawspeed3=False):
    # Use the maintained release's configure/Makefile, not the separately
    # contributed and explicitly unmaintained LibRaw-cmake project. Only the
    # thread-safe library is needed; example tools and the duplicate non-TLS
    # runtime are not built or installed.
    build.mkdir(parents=True, exist_ok=True)
    native_env = dict(env)
    native_env["CFLAGS"] = native_env["CXXFLAGS"] = (
        ("-O2 -g " if sanitized else "-O3 ") + flags)
    native_env["CPPFLAGS"] = "-DLIBRAW_MAX_PROFILE_SIZE_MB=4"
    native_env["LDFLAGS"] = "-Wl,-z,relro,-z,now,-z,noexecstack"
    if rawspeed3:
        build_rawspeed3(source, build, prefix, spec, jobs)
        native_env["CPPFLAGS"] += (
            f" -DUSE_RAWSPEED3 -DUSE_RAWSPEED_BITS"
            f" -I{source / 'RawSpeed3/rawspeed3_c_api'}")
        native_env["LIBS"] = f"-L{prefix / 'lib'} -lrawspeed3"
        # The package keeps libraw_r and librawspeed3 side by side. Escape '$'
        # through make and the shell so the ELF records a literal $ORIGIN.
        native_env["LDFLAGS"] += " -Wl,-rpath,\\$$ORIGIN"
    subprocess.run([str(source / "configure"), f"--prefix={prefix}",
                    f"--libdir={prefix / 'lib'}", *spec["configure_options"]],
                   cwd=build, env=native_env, check=True)
    subprocess.run(["make", f"-j{jobs}", "lib/libraw_r.la"],
                   cwd=build, env=native_env, check=True)
    library_dir = prefix / "lib"
    library_dir.mkdir(parents=True, exist_ok=True)
    for artifact in (build / "lib/.libs").glob("libraw_r.so*"):
        target = library_dir / artifact.name
        target.unlink(missing_ok=True)
        if artifact.is_symlink():
            target.symlink_to(artifact.readlink())
        else:
            shutil.copy2(artifact, target)
            if not rawspeed3:
                subprocess.run(["cmake", f"-DLICASA_CODEC_LIBRARY={target}", "-P",
                                str(ROOT / "cmake/strip_codec_rpath.cmake")], check=True)
    headers = prefix / "include/libraw"
    headers.mkdir(parents=True, exist_ok=True)
    for header in (source / "libraw").glob("*.h"):
        shutil.copy2(header, headers)
    package_dir = library_dir / "pkgconfig"
    package_dir.mkdir(exist_ok=True)
    shutil.copy2(build / "libraw_r.pc", package_dir)


def build_meson(source, build, prefix, spec, flags, env, jobs, sanitizer):
    if shutil.which("meson") is None:
        raise RuntimeError("meson is required for dav1d; install the Ubuntu 'meson' package")
    native_env = dict(env)
    if sanitizer:
        native_env["CFLAGS"] = f"-O1 -g {flags}"
        native_env["CXXFLAGS"] = f"-O1 -g {flags}"
        native_env["LDFLAGS"] = (
            f"-fsanitize={sanitizer} -Wl,-z,relro,-z,now,-z,noexecstack")
        buildtype = "plain"
    else:
        native_env["CFLAGS"] = flags
        native_env["CXXFLAGS"] = flags
        native_env["LDFLAGS"] = "-Wl,-z,relro,-z,now,-z,noexecstack"
        buildtype = "release"
    setup = [
        "meson", "setup", str(build), str(source), f"--prefix={prefix}",
        "--libdir=lib", f"--buildtype={buildtype}", "--default-library=shared",
    ]
    setup.extend(f"-D{key}={value}" for key, value in spec["options"].items())
    if (build / "meson-private/coredata.dat").exists():
        setup.insert(2, "--reconfigure")
    subprocess.run(setup, env=native_env, check=True)
    subprocess.run(["meson", "compile", "-C", str(build), "-j", str(jobs)],
                   env=native_env, check=True)
    subprocess.run(["meson", "install", "-C", str(build)], env=native_env, check=True)


def selected_names(lock, requested):
    if not requested:
        return set(lock)
    unknown = set(requested) - lock.keys()
    if unknown:
        raise ValueError("unknown codec name: " + ", ".join(sorted(unknown)))
    selected = set(requested)
    pending = list(requested)
    while pending:
        name = pending.pop()
        for dependency in lock[name].get("requires", []):
            if dependency not in lock:
                raise RuntimeError(f"{name} requires missing lock entry {dependency}")
            if dependency not in selected:
                selected.add(dependency)
                pending.append(dependency)
    return selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "build/codecs")
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--sanitizer", choices=("address", "undefined", "address,undefined"))
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
    parser.add_argument("--only", nargs="+", help="Build only named entries plus their locked requirements")
    parser.add_argument("--libraw-rawspeed3", action="store_true",
                        help="Build pinned RawSpeed3 into an isolated LibRaw codec prefix")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be at least 1")
    work = args.work_dir.resolve()
    variant = args.sanitizer or "release"
    prefix = (args.prefix or work / (variant + "-prefix")).resolve()
    downloads = work / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / "packaging/codecs.lock.json").read_text())
    try:
        selected = selected_names(lock, args.only)
    except ValueError as error:
        parser.error(str(error))
    if args.libraw_rawspeed3 and ("libraw" not in selected or args.sanitizer):
        parser.error("--libraw-rawspeed3 requires libraw and a release build")
    if args.libraw_rawspeed3 and prefix == (ROOT / "build/codecs/release-prefix").resolve():
        parser.error("--libraw-rawspeed3 requires an isolated --work-dir or --prefix")
    if args.libraw_rawspeed3 and work == (ROOT / "build/codecs").resolve():
        parser.error("--libraw-rawspeed3 requires an isolated --work-dir")
    flags = "-fstack-protector-strong"
    if args.sanitizer:
        flags += f" -fsanitize={args.sanitizer} -fno-omit-frame-pointer -fno-sanitize-recover=all"
    env = dict(os.environ)
    # Do not accidentally cross-wire a developer's unrelated sysroot.
    env.pop("PKG_CONFIG_SYSROOT_DIR", None)
    env["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig")
    for name, spec in lock.items():
        if name not in selected:
            continue
        source = work / f"{name}-{spec['version']}"
        prepare_source(name, spec, downloads, source)
        for dependency, pinned in spec.get("dependencies", {}).items():
            prepare_source(dependency, pinned, downloads, source / pinned["path"])
        for patch in spec.get("patches", []):
            command = ["patch", "--batch", "-p1", "-i", str(ROOT / patch)]
            check_patch = subprocess.run(command + ["--forward", "--dry-run"], cwd=source,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if check_patch.returncode == 0:
                subprocess.run(command + ["--forward"], cwd=source, check=True)
            else:
                # Repeated builds must validate the exact already-applied
                # patch, not silently accept an unrelated dirty source tree.
                subprocess.run(command + ["--reverse", "--dry-run"], cwd=source, check=True)
        build = work / f"{name}-{variant}"
        if spec.get("build_system") == "autotools":
            if name != "libraw":
                raise RuntimeError(f"No pinned autotools recipe for {name}")
            build_libraw(source, build, prefix, spec, flags, env, args.jobs,
                         args.sanitizer, args.libraw_rawspeed3)
        elif spec.get("build_system") == "meson":
            build_meson(source, build, prefix, spec, flags, env, args.jobs, args.sanitizer)
        else:
            command = ["cmake", "-S", str(source), "-B", str(build),
                   "-DCMAKE_BUILD_TYPE=" + ("RelWithDebInfo" if args.sanitizer else "Release"),
                   f"-DCMAKE_INSTALL_PREFIX={prefix}", f"-DCMAKE_PREFIX_PATH={prefix}",
                   "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_INSTALL_RPATH=$ORIGIN",
                   "-DBUILD_SHARED_LIBS=ON", f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}",
                   "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,relro,-z,now,-z,noexecstack"]
            command.extend(f"-D{key}={value}" for key, value in spec["options"].items())
            subprocess.run(command, env=env, check=True)
            build_command = ["cmake", "--build", str(build), "--parallel", str(args.jobs)]
            if name == "libjxl":
                build_command += ["--target", "jxl_dec", "jxl_cms", "jxl_threads"]
            elif name == "libyuv":
                build_command += ["--target", "yuv_shared"]
            subprocess.run(build_command, env=env, check=True)
            if name == "libjxl":
                install_jxl_decoder(build, prefix, spec["version"])
            elif name == "libyuv":
                install_libyuv(build, source, prefix, spec["version"])
            else:
                subprocess.run(["cmake", "--install", str(build)], env=env, check=True)
        license_dir = prefix / "share/licenses/licasa-codecs" / name
        # A codec prefix is intentionally reused across pinned upgrades. Replace
        # this codec's provenance subtree after a successful build so versioned
        # patch/license files from an older pin cannot leak into packages.
        if license_dir.is_symlink():
            license_dir.unlink()
        elif license_dir.exists():
            shutil.rmtree(license_dir)
        license_dir.mkdir(parents=True, exist_ok=True)
        if spec.get("patches"):
            patch_dir = license_dir / "patches"
            patch_dir.mkdir(exist_ok=True)
            for patch in spec["patches"]:
                shutil.copy2(ROOT / patch, patch_dir / Path(patch).name)
        for pattern in ("COPYING*", "LICENSE*", "COPYRIGHT*"):
            for license_file in source.glob(pattern):
                if license_file.is_file():
                    shutil.copy2(license_file, license_dir)
        for dependency, pinned in spec.get("dependencies", {}).items():
            dependency_licenses = license_dir / dependency
            dependency_licenses.mkdir(exist_ok=True)
            for license_file in (source / pinned["path"]).glob("LICENSE*"):
                if license_file.is_file():
                    shutil.copy2(license_file, dependency_licenses)
        if name == "libraw" and args.libraw_rawspeed3:
            rawspeed_receipt = license_dir / "rawspeed3"
            rawspeed_receipt.mkdir(exist_ok=True)
            shutil.copy2(work / "rawspeed3-pinned/LICENSE", rawspeed_receipt / "LICENSE")
            for patch in (source / "RawSpeed3/patches").glob("*.patch"):
                shutil.copy2(patch, rawspeed_receipt / patch.name)
            pugixml_readme = (build / "rawspeed3-build/rawspeed-src/src/external/"
                              "pugixml/src/readme.txt")
            shutil.copy2(pugixml_readme, rawspeed_receipt / "pugixml-readme.txt")
    provenance = prefix / "share/licenses/licasa-codecs/sources.json"
    provenance.parent.mkdir(parents=True, exist_ok=True)
    provenance.write_text(json.dumps(lock, indent=2) + "\n")
    print(f"Codec prefix: {prefix}")


if __name__ == "__main__":
    main()
