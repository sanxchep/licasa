#!/usr/bin/env python3
"""Install and launch the release AppImage without FUSE on clean Fedora 44."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from verify_installed_release import SAMPLES


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/qualification/appimage/fedora-smoke.json")
    args = parser.parse_args()
    appimage = args.appimage.resolve(strict=True)
    sample = ROOT / "tests/test-assets" / next(iter(SAMPLES.values()))
    if not sample.is_file():
        parser.error(f"Missing sample: {sample}")
    with tempfile.TemporaryDirectory(prefix="licasa-fedora-appimage-") as temporary:
        work = Path(temporary)
        shutil.copy2(appimage, work / "Licasa.AppImage")
        shutil.copy2(ROOT / "packaging/install-licasa-appimage.sh",
                     work / "install-licasa-appimage.sh")
        (work / "Dockerfile").write_text(
            "FROM fedora:44\n"
            "RUN dnf install -y --setopt=install_weak_deps=False "
            "libX11 libXext libxcb mesa-libGL mesa-libEGL libglvnd-opengl "
            "fontconfig harfbuzz dbus-libs desktop-file-utils xdg-utils "
            "&& dnf clean all\n"
            "COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage\n"
            "COPY --chmod=755 install-licasa-appimage.sh /opt/install-licasa-appimage.sh\n")
        tag = "licasa-appimage-fedora-verify:local"
        subprocess.run(["docker", "build", "--tag", tag, str(work)], check=True)
        command = (
            "mkdir -p \"$HOME\"; "
            "/opt/install-licasa-appimage.sh /opt/Licasa.AppImage; "
            "desktop-file-validate \"$XDG_DATA_HOME/applications/licasa-appimage.desktop\"; "
            "xdg-mime default licasa-appimage.desktop image/jpeg; "
            "test \"$(xdg-mime query default image/jpeg)\" = licasa-appimage.desktop; "
            "status=0; timeout 15 \"$HOME/.local/opt/licasa-appimage/AppRun\" \"$1\" "
            ">/tmp/viewer.log 2>&1 || status=$?; "
            "if [ \"$status\" -ne 124 ]; then cat /tmp/viewer.log; exit 1; fi"
        )
        result = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "--read-only",
             "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
             "-e", "HOME=/tmp/home", "-e", "XDG_DATA_HOME=/tmp/home/.local/share",
             "-e", "XDG_CONFIG_HOME=/tmp/home/.config",
             "-e", "XDG_RUNTIME_DIR=/tmp/runtime", "-e", "QT_QPA_PLATFORM=offscreen",
             "-e", "QT_QUICK_BACKEND=software",
             "-v", f"{ROOT / 'tests/test-assets'}:/samples:ro", tag,
             "sh", "-ec", command, "_", "/samples/" + str(sample.relative_to(ROOT / "tests/test-assets"))],
            capture_output=True, text=True, timeout=90,
        )
        if result.returncode:
            raise RuntimeError(f"Fedora AppImage installation failed:\n{result.stdout}\n{result.stderr}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "fedora_version": 44,
        "appimage_sha256": hashlib.sha256(appimage.read_bytes()).hexdigest(),
        "fuse_free_install": True,
        "desktop_valid": True,
        "jpeg_default_selected": True,
        "viewer_launched": True,
    }, indent=2) + "\n")
    print(f"Fedora 44 AppImage desktop installation passed: {args.output}")


if __name__ == "__main__":
    main()
