#!/usr/bin/env python3
"""Build a Fedora 44 x86_64 RPM from the verified FUSE-free AppImage payload."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import subprocess

from native_lifecycle import embedded_script


ROOT = Path(__file__).resolve().parents[1]
IMAGE = "licasa-rpm-builder:fedora44"


def render_spec() -> str:
    """Use the same incoming lifecycle scripts for production and manager fixtures."""
    spec = (ROOT / "packaging/licasa-appimage.spec").read_text()
    operations = {"@NATIVE_PREINSTALL@": ("prepare", "upgrade"),
                  "@NATIVE_COMPLETE@": ("complete", "upgrade"),
                  "@NATIVE_PREREMOVE@": ("prepare", "remove"),
                  "@NATIVE_REMOVED@": ("removed", "remove")}
    for marker, (operation, action) in operations.items():
        content = embedded_script("appimage-native", operation, action)
        if action == "remove":
            content = content.replace("set -eu\n", 'set -eu\n[ "${1:-0}" -eq 0 ] || exit 0\n', 1)
        # RPM expands percent macros while parsing scriptlets.
        spec = spec.replace(marker, content.removeprefix("#!/bin/sh\n").replace("%", "%%"))
    return spec


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "build/qualification/rpm")
    args = parser.parse_args()
    appimage = args.appimage.resolve(strict=True)
    match = re.fullmatch(r"Licasa-(\d+\.\d+\.\d+)-x86_64\.AppImage", appimage.name)
    if not match:
        parser.error("Expected Licasa-<version>-x86_64.AppImage")
    version = match.group(1)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    rendered_spec = output / "licasa-appimage.spec"
    rendered_spec.write_text(render_spec())
    subprocess.run(["docker", "build", "--file", str(ROOT / "packaging/build-rpm.Dockerfile"),
                    "--tag", IMAGE, str(ROOT / "packaging")], check=True)
    script = (
        "mkdir -p /tmp/rpmbuild/SOURCES /tmp/rpmbuild/RPMS; "
        "cp /input/AppImage /tmp/rpmbuild/SOURCES/Licasa-$1-x86_64.AppImage; "
        "cp /source/tools/stage_portable_package.py /tmp/rpmbuild/SOURCES/; "
        "cp /source/tools/native_lifecycle.py /tmp/rpmbuild/SOURCES/; "
        "rpmbuild -bb --define '_topdir /tmp/rpmbuild' "
        "--define \"licasa_version $1\" /input/licasa-appimage.spec; "
        "cp /tmp/rpmbuild/RPMS/x86_64/*.rpm /output/"
    )
    subprocess.run(["docker", "run", "--rm", "--network", "none",
                    "-v", f"{ROOT}:/source:ro", "-v", f"{appimage}:/input/AppImage:ro",
                    "-v", f"{rendered_spec}:/input/licasa-appimage.spec:ro",
                    "-v", f"{output}:/output", IMAGE, "sh", "-ec", script, "_", version],
                   check=True)
    packages = list(output.glob(f"licasa-appimage-{version}-1.*.x86_64.rpm"))
    if len(packages) != 1:
        raise RuntimeError(f"Expected one Fedora RPM, found {packages}")
    package = packages[0]
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    package.with_name(package.name + ".sha256").write_text(
        f"{digest}  {package.name}\n")
    print(f"Fedora RPM: {package}\nSHA-256: {digest}")


if __name__ == "__main__":
    main()
