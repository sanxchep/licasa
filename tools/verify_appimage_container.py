#!/usr/bin/env python3
"""Run an AppImage without network or preinstalled Qt on clean Ubuntu 24.04."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from verify_installed_release import SAMPLES


ROOT = Path(__file__).resolve().parents[1]
EXTRA_SAMPLES = {"jp2": "formats/additional-probes/jpeg-2000.jp2"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/qualification/appimage/install-smoke.json")
    args = parser.parse_args()
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
            "FROM ubuntu:24.04\n"
            "RUN apt-get update && apt-get install -y --no-install-recommends "
            "libx11-6 libxext6 libxcb1 libgl1 libegl1 libopengl0 "
            "libfontconfig1 libharfbuzz0b libdbus-1-3 "
            "desktop-file-utils xdg-utils "
            "&& rm -rf /var/lib/apt/lists/*\n"
            "COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage\n"
            "COPY --chmod=755 install-licasa-appimage.sh /opt/install-licasa-appimage.sh\n")
        tag = "licasa-appimage-verify:local"
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
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Clean AppImage launched the viewer and decoded {len(samples)} samples: {args.output}")


if __name__ == "__main__":
    main()
