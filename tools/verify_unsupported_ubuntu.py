#!/usr/bin/env python3
"""Check deliberate package rejection on Ubuntu versions outside the native baseline."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from verify_appimage_container import HOST_RUNTIME_PACKAGES


ROOT = Path(__file__).resolve().parents[1]
# Reach the intended ABI rejection after satisfying the installer preflight.
# These provide tools only; no Qt or application compatibility libraries.
INSTALLER_PACKAGES = ("bash", "coreutils", "findutils", "sed", "util-linux", "libc-bin", "python3")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--ubuntu-version", choices=("22.04", "26.04"), required=True)
    parser.add_argument("--appimage", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        parser.error("Run this container check only on an ephemeral GitHub-hosted VM")
    if (args.ubuntu_version == "22.04") != bool(args.appimage):
        parser.error("Supply the AppImage only for Ubuntu 22.04 rejection checks")
    package = args.package.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="licasa-ubuntu-rejection-") as temporary:
        work = Path(temporary)
        shutil.copy2(package, work / "licasa.deb")
        recipe = (
            f"FROM ubuntu:{args.ubuntu_version}\n"
            "COPY licasa.deb /tmp/licasa.deb\n"
            "RUN apt-get update\n"
        )
        if args.appimage:
            appimage = args.appimage.resolve(strict=True)
            shutil.copy2(appimage, work / "Licasa.AppImage")
            shutil.copy2(ROOT / "packaging/install-licasa-appimage.sh", work / "installer.sh")
            recipe += (
                "RUN apt-get install -y --no-install-recommends "
                + " ".join((*HOST_RUNTIME_PACKAGES, *INSTALLER_PACKAGES)) + "\n"
                "COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage\n"
                "COPY installer.sh /opt/installer.sh\n"
            )
        (work / "Dockerfile").write_text(recipe)
        image = f"licasa-ubuntu-{args.ubuntu_version}-rejection:local"
        subprocess.run(["docker", "build", "--tag", image, str(work)], check=True)
        apt = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", image,
             "apt-get", "install", "--simulate", "--no-install-recommends", "/tmp/licasa.deb"],
            capture_output=True, text=True, timeout=60,
        )
        apt_log = apt.stdout + apt.stderr
        if (apt.returncode == 0 or "qt6-base-abi" not in apt_log
                or "6.4.2" not in apt_log or "not installable" not in apt_log):
            raise RuntimeError(f"Expected the native Qt ABI dependency to reject the .deb:\n{apt_log}")
        print(apt_log)
        report = {
            "ubuntu_version": args.ubuntu_version,
            "native_deb": "unsupported_dependency_rejection_confirmed",
            "package_sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
        }
        if args.appimage:
            installed = subprocess.run(
                ["docker", "run", "--rm", "--network", "none", "--read-only",
                 "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
                 "-e", "HOME=/tmp/home", image,
                 "bash", "/opt/installer.sh", "/opt/Licasa.AppImage"],
                capture_output=True, text=True, timeout=120,
            )
            output = installed.stdout + installed.stderr
            if (installed.returncode == 0 or not re.search(r"GLIBC_[0-9.]+.*not found", output)
                    or "before replacing" not in output):
                raise RuntimeError(f"Expected an explicit glibc-baseline rejection:\n{output}")
            print(output)
            report["appimage"] = "unsupported_glibc_rejection_confirmed"
            report["appimage_sha256"] = hashlib.sha256(appimage.read_bytes()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
