#!/usr/bin/env python3
"""Verify the pinned AVIF dependency foundation in a private codec prefix."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

EXPECTED = {
    "libavif": "1.4.2",
    "dav1d": "1.5.4",
    "libyuv": "1924",
}
EXPECTED_LIBYUV_COMMIT = "644251f252a84bf8ce91ff0aca86a9b16b069ab8"
EXPECTED_LIBYUV_TREE = "ba22b636166dc16532fcc9d5a9524117432d1782"
FORBIDDEN_AV1 = ("libaom", "libgav1", "rav1e", "SvtAv1", "libSvt", "libavm")
FORBIDDEN_TOOLS = ("avifenc", "avifdec", "avifgainmaputil", "dav1d", "yuvconvert", "yuvconstants", "cpuid")


def command_output(command, env=None):
    return subprocess.run(command, check=True, text=True, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def shared_library(prefix, stem):
    candidates = sorted((prefix / "lib").glob(stem + ".so*"))
    if not candidates:
        raise RuntimeError(f"missing {stem}.so under {prefix / 'lib'}")
    real = [candidate for candidate in candidates if not candidate.is_symlink()]
    return (real or candidates)[-1].resolve()


def hardening_checks(path):
    program = command_output(["readelf", "-W", "-l", str(path)])
    dynamic = command_output(["readelf", "-W", "-d", str(path)])
    stack = next((line for line in program.splitlines() if "GNU_STACK" in line), "")
    executable_stack = bool(re.search(r"\b[A-Z]*E[A-Z]*\b", stack))
    return {
        "non_executable_stack": bool(stack) and not executable_stack,
        "relro": "GNU_RELRO" in program,
        "immediate_binding": "BIND_NOW" in dynamic or bool(re.search(r"FLAGS(?:_1)?.*NOW", dynamic)),
    }


def pkg_version(prefix, package):
    env = dict(os.environ)
    env["PKG_CONFIG_PATH"] = str(prefix / "lib/pkgconfig")
    env.pop("PKG_CONFIG_SYSROOT_DIR", None)
    return command_output(["pkg-config", "--modversion", package], env=env).strip()


def license_present(prefix, name):
    directory = prefix / "share/licenses/licasa-codecs" / name
    return directory.is_dir() and any(
        p.is_file() and p.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT"))
        for p in directory.iterdir()
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)

    checks = {}
    details = {"prefix": str(prefix), "expected_versions": EXPECTED}

    versions = {}
    for package, expected in EXPECTED.items():
        try:
            versions[package] = pkg_version(prefix, package)
            checks[f"version_{package}"] = versions[package] == expected
        except (subprocess.CalledProcessError, FileNotFoundError):
            versions[package] = None
            checks[f"version_{package}"] = False
    details["versions"] = versions

    libraries = {}
    for key, stem in (("libavif", "libavif"), ("dav1d", "libdav1d"), ("libyuv", "libyuv")):
        try:
            libraries[key] = shared_library(prefix, stem)
            checks[f"shared_{key}"] = True
        except RuntimeError:
            libraries[key] = None
            checks[f"shared_{key}"] = False
    details["libraries"] = {key: str(value) if value else None for key, value in libraries.items()}

    avif = libraries["libavif"]
    if avif:
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = str(prefix / "lib")
        try:
            ldd = command_output(["ldd", str(avif)], env=env)
        except subprocess.CalledProcessError as error:
            ldd = error.stdout or ""
        details["libavif_ldd"] = ldd
        prefix_text = str(prefix / "lib")
        checks["libavif_uses_private_dav1d"] = any(
            "libdav1d" in line and prefix_text in line for line in ldd.splitlines())
        checks["libavif_uses_private_libyuv"] = any(
            "libyuv" in line and prefix_text in line for line in ldd.splitlines())
        checks["no_unplanned_av1_backend"] = not any(token in ldd for token in FORBIDDEN_AV1)
    else:
        checks["libavif_uses_private_dav1d"] = False
        checks["libavif_uses_private_libyuv"] = False
        checks["no_unplanned_av1_backend"] = False

    for name, path in libraries.items():
        if path:
            for key, passed in hardening_checks(path).items():
                checks[f"{name}_{key}"] = passed
        else:
            for key in ("non_executable_stack", "relro", "immediate_binding"):
                checks[f"{name}_{key}"] = False

    checks["no_avif_dependency_tools"] = not any((prefix / "bin" / tool).exists() for tool in FORBIDDEN_TOOLS)
    checks["no_static_avif_stack"] = not any(
        (prefix / "lib" / archive).exists() for archive in ("libavif.a", "libdav1d.a", "libyuv.a")
    )
    for name in EXPECTED:
        checks[f"license_{name}"] = license_present(prefix, name)

    provenance_path = prefix / "share/licenses/licasa-codecs/sources.json"
    provenance = None
    if provenance_path.exists():
        try:
            provenance = json.loads(provenance_path.read_text())
        except json.JSONDecodeError:
            pass
    checks["provenance_json"] = provenance is not None
    if provenance:
        yuv = provenance.get("libyuv", {})
        checks["libyuv_commit_identity"] = yuv.get("commit") == EXPECTED_LIBYUV_COMMIT
        checks["libyuv_tree_identity"] = yuv.get("tree") == EXPECTED_LIBYUV_TREE
        checks["libavif_archive_identity"] = provenance.get("libavif", {}).get("sha256") == \
            "2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d"
        checks["dav1d_archive_identity"] = provenance.get("dav1d", {}).get("sha256") == \
            "686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5"
    else:
        checks["libyuv_commit_identity"] = False
        checks["libyuv_tree_identity"] = False
        checks["libavif_archive_identity"] = False
        checks["dav1d_archive_identity"] = False

    report = {
        "checks": checks,
        "passed": all(checks.values()),
        "details": details,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    print(encoded, end="")
    if not report["passed"]:
        raise SystemExit("AVIF dependency foundation checks failed")


if __name__ == "__main__":
    main()
