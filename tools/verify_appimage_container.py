#!/usr/bin/env python3
"""Run an AppImage without network or preinstalled Qt on a selected clean Ubuntu."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from display_session_results import parse_display_graphics
from check_package_report import write_checked_report
from verify_installed_release import SAMPLES
from package_dialog_checks import run_dialog_checks
from package_motion_checks import run_motion_checks


ROOT = Path(__file__).resolve().parents[1]
EXTRA_SAMPLES = {"jp2": "formats/additional-probes/jpeg-2000.jp2"}
# Host graphics, font and session libraries; Qt and codecs must be bundled.
HOST_RUNTIME_PACKAGES = (
    "libx11-6", "libxext6", "libxcb1", "libgl1", "libegl1", "libopengl0",
    "libfontconfig1", "libharfbuzz0b", "libdbus-1-3",
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--ubuntu-version", choices=("22.04", "24.04", "26.04"))
    parser.add_argument("--target", choices=("ubuntu-22.04", "ubuntu-24.04", "ubuntu-26.04", "debian-13"))
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/qualification/appimage/install-smoke.json")
    args = parser.parse_args()
    if args.target and args.ubuntu_version and args.target != f"ubuntu-{args.ubuntu_version}":
        parser.error("--target and --ubuntu-version select different userspaces")
    args.target = args.target or f"ubuntu-{args.ubuntu_version or '24.04'}"
    args.ubuntu_version = args.target.removeprefix("ubuntu-") if args.target.startswith("ubuntu-") else None
    appimage = args.appimage.resolve(strict=True)
    sample_paths = {**SAMPLES, **EXTRA_SAMPLES}
    samples = [ROOT / "tests/test-assets" / name for name in sample_paths.values()]
    if any(not sample.is_file() for sample in samples):
        parser.error("Repository release samples are missing")

    with tempfile.TemporaryDirectory(prefix="licasa-appimage-verify-") as temporary:
        work = Path(temporary)
        shutil.copy2(appimage, work / "Licasa.AppImage")
        shutil.copy2(ROOT / "packaging/install-licasa-appimage.sh",
                     work / "install-licasa-appimage.sh")
        (work / "Dockerfile").write_text(
            f"FROM {'debian:trixie-slim' if args.target == 'debian-13' else 'ubuntu:' + args.ubuntu_version}\n"
            "RUN apt-get update && apt-get install -y --no-install-recommends "
            + " ".join(HOST_RUNTIME_PACKAGES) + " "
            "&& rm -rf /var/lib/apt/lists/*\n"
            "COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage\n"
            "COPY --chmod=755 install-licasa-appimage.sh /opt/install-licasa-appimage.sh\n"
            # Scan the whole bundle before display tools can supply missing libraries.
            "RUN cd /tmp && /opt/Licasa.AppImage --appimage-extract >/dev/null "
            "&& bash -ec '. /opt/install-licasa-appimage.sh; "
            "licasa_check_runtime_dependencies "
            "/tmp/squashfs-root/usr/lib/licasa/lib:/tmp/squashfs-root/usr/lib "
            "/tmp/squashfs-root/usr' && rm -rf /tmp/squashfs-root\n"
            "RUN apt-get update && apt-get install -y --no-install-recommends "
            "desktop-file-utils xdg-utils xvfb x11-utils openbox weston libgl1-mesa-dri python3 "
            "&& rm -rf /var/lib/apt/lists/*\n")
        tag = f"licasa-appimage-{args.target}-verify:local"
        subprocess.run(["docker", "build", "--tag", tag, str(work)], check=True)
        command = (
            "cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null; "
            "appdir=/tmp/squashfs-root; "
            "export LD_LIBRARY_PATH=\"$appdir/usr/lib/licasa/lib:$appdir/usr/lib\"; "
            "export QT_PLUGIN_PATH=\"$appdir/usr/plugins:$appdir/usr/lib/qt6/plugins:"
            "$appdir/usr/lib/licasa/plugins\"; "
            "export QML2_IMPORT_PATH=\"$appdir/usr/qml:$appdir/usr/lib/qt6/qml\"; "
            "export QML_IMPORT_PATH=\"$QML2_IMPORT_PATH\"; "
            "exec \"$appdir/usr/bin/licasa_diagnostics\" --formats --megapixels 25 \"$@\""
        )
        result = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "--read-only",
             "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
             "-e", "QT_QPA_PLATFORM=offscreen", "-e", "QT_QUICK_BACKEND=software",
             "-e", "XDG_RUNTIME_DIR=/tmp/runtime", "-e", "XDG_CACHE_HOME=/tmp/cache",
             "-v", f"{ROOT / 'tests/test-assets'}:/samples:ro", tag,
             "sh", "-ec", command, "_",
             *(f"/samples/{name}" for name in sample_paths.values())],
            capture_output=True, text=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(f"Clean AppImage decode failed:\n{result.stderr}\n{result.stdout}")
        report = json.loads(result.stdout)
        missing = set(sample_paths) - {name.lower() for name in report["read_formats"]}
        failed = [sample["path"] for sample in report["samples"] if not sample["decoded"]]
        if missing or failed:
            raise RuntimeError(f"AppImage missing readers {sorted(missing)} or failed files {failed}")
        quick = [path for path in report["before_decode"]["mapped_qt_libraries"]
                 if "libQt6Quick.so." in path]
        if len(quick) != 1 or "/tmp/squashfs-root/" not in quick[0]:
            raise RuntimeError(f"AppImage loaded the wrong Qt Quick runtime: {quick}")
        for sample in report["samples"]:
            for library in sample["process"]["mapped_optional_libraries"]:
                if not library.startswith("/tmp/squashfs-root/"):
                    raise RuntimeError(f"AppImage codec escaped its bundle: {library}")
        viewer = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "--read-only",
             "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
             "-e", "APPIMAGE_EXTRACT_AND_RUN=1", "-e", "QT_QPA_PLATFORM=offscreen",
             "-e", "QT_QUICK_BACKEND=software", "-e", "XDG_RUNTIME_DIR=/tmp/runtime",
             "-e", "XDG_CACHE_HOME=/tmp/cache", "-e", "XDG_CONFIG_HOME=/tmp/config",
             "-v", f"{ROOT / 'tests/test-assets'}:/samples:ro", tag,
             "sh", "-ec", "status=0; timeout 15 /opt/Licasa.AppImage \"$1\" "
             ">/tmp/viewer.log 2>&1 || status=$?; "
             "if [ \"$status\" -ne 124 ]; then cat /tmp/viewer.log; exit 1; fi",
             "_", f"/samples/{next(iter(SAMPLES.values()))}"],
            capture_output=True, text=True, timeout=40,
        )
        if viewer.returncode:
            raise RuntimeError(f"AppImage viewer did not stay open:\n{viewer.stdout}\n{viewer.stderr}")
        installed = subprocess.run(
            ["docker", "run", "--rm", "--network", "none", "--read-only",
             "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
             "-e", "HOME=/tmp/home", "-e", "XDG_DATA_HOME=/tmp/home/.local/share",
             "-e", "XDG_CONFIG_HOME=/tmp/home/.config",
             "-e", "XDG_RUNTIME_DIR=relative", "-e", "XDG_STATE_HOME=/tmp/state",
             "-e", "QT_QPA_PLATFORM=offscreen",
             "-e", "QT_QUICK_BACKEND=software",
             "-v", f"{ROOT / 'tests/test-assets'}:/samples:ro", tag,
             "sh", "-ec", "mkdir -p \"$HOME\"; "
             "/opt/install-licasa-appimage.sh /opt/Licasa.AppImage; "
             "/opt/install-licasa-appimage.sh /opt/Licasa.AppImage; "
             "desktop-file-validate \"$XDG_DATA_HOME/applications/licasa-appimage.desktop\"; "
             "xdg-mime default licasa-appimage.desktop image/jpeg; "
             "test \"$(xdg-mime query default image/jpeg)\" = licasa-appimage.desktop; "
             "status=0; timeout 15 \"$HOME/.local/opt/licasa-appimage/AppRun\" \"$1\" "
             ">/tmp/installed-viewer.log 2>&1 || status=$?; "
             "if [ \"$status\" -ne 124 ]; then cat /tmp/installed-viewer.log; exit 1; fi; "
             "grep -Fq 'Single-instance IPC disabled' /tmp/state/licasa/errors.log",
             "_", f"/samples/{next(iter(SAMPLES.values()))}"],
            capture_output=True, text=True, timeout=90,
        )
        if installed.returncode:
            raise RuntimeError("FUSE-free AppImage desktop installation failed:\n"
                               f"{installed.stdout}\n{installed.stderr}")
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
                     "-v", f"{samples[0]}:/samples/photo.jpg:ro", tag,
                     "sh", "-ec", display_script], capture_output=True, text=True, timeout=100,
                )
                if display.returncode:
                    raise RuntimeError(f"Ubuntu AppImage {name} failed:\n"
                                       f"{display.stdout}\n{display.stderr}")
                displays[name] = parse_display_graphics(display.stdout, context=f"Ubuntu AppImage {name}")
        report["display_sessions"] = displays
        report["dialog_interactions"] = run_dialog_checks(
            tag, output_dir=args.output.parent / "dialog-artifacts", app="/tmp/squashfs-root/AppRun",
            diagnostic="/tmp/squashfs-root/usr/bin/licasa_diagnostics", sample=samples[0],
            setup="cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null;")
        report["target"] = args.target
        report["motion_photo"] = run_motion_checks(
            tag, diagnostic="/tmp/squashfs-root/usr/bin/licasa_diagnostics",
            runtime_root="/tmp/squashfs-root/usr", output_dir=args.output.parent / "motion-artifacts",
            setup="cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null;")
        report["ubuntu_version"] = args.ubuntu_version
        report["runtime_dependencies_checked_before_desktop_tools"] = True
        report["host_runtime_packages"] = HOST_RUNTIME_PACKAGES
        report["appimage_sha256"] = hashlib.sha256(appimage.read_bytes()).hexdigest()
    target_id = {"debian-13": "debian13-appimage", "ubuntu-22.04": "ubuntu22-appimage", "ubuntu-24.04": "ubuntu24-appimage",
                 "ubuntu-26.04": "ubuntu26-appimage"}[args.target]
    write_checked_report(report, target_id=target_id, package=appimage, output=args.output)
    print(f"Clean AppImage launched the viewer and decoded {len(samples)} samples: {args.output}")


if __name__ == "__main__":
    main()
