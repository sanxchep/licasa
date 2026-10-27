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

from display_session_results import parse_display_graphics
from verify_installed_release import SAMPLES
from package_dialog_checks import run_dialog_checks
from package_reader_checks import run_reader_checks
from package_motion_checks import run_motion_checks


ROOT = Path(__file__).resolve().parents[1]
HOST_RUNTIME_PACKAGES = (
    "libX11", "libXext", "libxcb", "libxkbcommon", "mesa-libGL", "mesa-libEGL",
    "libglvnd-opengl", "fontconfig", "harfbuzz", "dbus-libs",
)
# Managed installation needs these tools after the raw bundle's loader check.
INSTALLER_PACKAGES = (
    "bash", "coreutils", "findutils", "glibc-common", "sed", "util-linux-core", "python3",
)


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
        shutil.copy2(ROOT / "tools/check_appimage_install.sh", work / "check_appimage_install.sh")
        (work / "Dockerfile").write_text(
            "FROM fedora:44\n"
            "RUN dnf install -y --setopt=install_weak_deps=False "
            + " ".join(HOST_RUNTIME_PACKAGES) + " && dnf clean all\n"
            "COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage\n"
            "COPY --chmod=755 install-licasa-appimage.sh /opt/install-licasa-appimage.sh\n"
            "COPY check_appimage_install.sh /opt/check_appimage_install.sh\n"
            "RUN cd /tmp && /opt/Licasa.AppImage --appimage-extract >/dev/null "
            "&& bash -ec '. /opt/install-licasa-appimage.sh; "
            "licasa_check_runtime_dependencies "
            "/tmp/squashfs-root/usr/lib/licasa/lib:/tmp/squashfs-root/usr/lib "
            "/tmp/squashfs-root/usr' && rm -rf /tmp/squashfs-root\n"
            "RUN dnf install -y --setopt=install_weak_deps=False "
            + " ".join(INSTALLER_PACKAGES) + " && dnf clean all\n"
            "USER 65534:65534\n"
            "RUN HOME=/tmp/licasa-install-test bash /opt/check_appimage_install.sh "
            "/opt/install-licasa-appimage.sh /opt/Licasa.AppImage\n"
            "USER 0:0\n"
            "RUN dnf install -y --setopt=install_weak_deps=False "
            "desktop-file-utils xdg-utils xorg-x11-server-Xvfb xwininfo xprop openbox weston mesa-dri-drivers "
            "&& dnf clean all\n")
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
        displays = {}
        for protocol in ("x11", "wayland"):
            for mode in ("default", "software"):
                name = f"{protocol}_{mode}"
                display_script = (
                    "mkdir -p \"$HOME\" \"$XDG_CONFIG_HOME\"; "
                    "/opt/install-licasa-appimage.sh /opt/Licasa.AppImage; "
                    "desktop-file-validate \"$XDG_DATA_HOME/applications/licasa-appimage.desktop\"; "
                    "xdg-mime default licasa-appimage.desktop image/jpeg; "
                    "test \"$(xdg-mime query default image/jpeg)\" = licasa-appimage.desktop; "
                    "cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null; "
                    "sh /opt/check_display_session.sh "
                    '"$HOME/.local/opt/licasa-appimage/AppRun" '
                    "/tmp/squashfs-root/usr/bin/licasa_diagnostics /samples/photo.jpg "
                    f"{protocol} {mode}"
                )
                display = subprocess.run(
                    ["docker", "run", "--rm", "--network", "none", "--read-only",
                     "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--shm-size", "256m",
                     "--user", "65534:65534", "--cap-drop", "ALL",
                     "-e", "HOME=/tmp/home", "-e", "XDG_DATA_HOME=/tmp/home/.local/share",
                     "-e", "XDG_CONFIG_HOME=/tmp/home/.config",
                     "-e", "XDG_RUNTIME_DIR=/tmp/runtime",
                     "-v", f"{ROOT / 'tools/check_display_session.sh'}:/opt/check_display_session.sh:ro",
                     "-v", f"{sample}:/samples/photo.jpg:ro", tag,
                     "sh", "-ec", display_script], capture_output=True, text=True, timeout=100,
                )
                if display.returncode:
                    raise RuntimeError(f"Fedora AppImage {name} failed:\n"
                                       f"{display.stdout}\n{display.stderr}")
                displays[name] = parse_display_graphics(display.stdout, context=f"Fedora AppImage {name}")
        readers = run_reader_checks(
            tag, diagnostic="/tmp/squashfs-root/usr/bin/licasa_diagnostics",
            runtime_root="/tmp/squashfs-root/usr/lib", output_dir=args.output.parent / "reader-artifacts",
            setup="cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null;")
        motion = run_motion_checks(
            tag, diagnostic="/tmp/squashfs-root/usr/bin/licasa_diagnostics",
            runtime_root="/tmp/squashfs-root/usr", output_dir=args.output.parent / "motion-artifacts",
            setup="cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null;")
        dialogs = run_dialog_checks(
            tag, output_dir=args.output.parent / "dialog-artifacts", app="/tmp/squashfs-root/AppRun",
            diagnostic="/tmp/squashfs-root/usr/bin/licasa_diagnostics", sample=sample,
            setup="cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null;")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "fedora_version": 44,
        "appimage_sha256": hashlib.sha256(appimage.read_bytes()).hexdigest(),
        "fuse_free_install": True,
        "runtime_checked_before_desktop_tools": True,
        "appimage_lifecycle_checked_before_desktop_tools": True,
        "desktop_valid": True,
        "jpeg_default_selected": True,
        "viewer_launched": True,
        "display_sessions": displays,
        "dialog_interactions": dialogs,
        "reader_smoke": readers,
        "motion_photo": motion,
    }, indent=2) + "\n")
    print(f"Fedora 44 AppImage desktop installation passed: {args.output}")


if __name__ == "__main__":
    main()
