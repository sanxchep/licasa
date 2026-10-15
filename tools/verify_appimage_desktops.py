#!/usr/bin/env python3
"""Smoke the release AppImage on Arch with virtual X11 and Wayland displays."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from verify_installed_release import SAMPLES


ROOT = Path(__file__).resolve().parents[1]
IMAGE = "licasa-appimage-arch-verify:local"
SAMPLE = "formats/core-matrix/jpeg/micro-1x1.jpg"
DECODE_SAMPLES = {**SAMPLES, "jp2": "formats/additional-probes/jpeg-2000.jp2"}
SETUP = """
mkdir -m 700 /tmp/runtime
export XDG_RUNTIME_DIR=/tmp/runtime
export XDG_CACHE_HOME=/tmp/cache
export XDG_CONFIG_HOME=/tmp/config
export XDG_DATA_HOME=/tmp/home/.local/share
export HOME=/tmp/home
export LIBGL_ALWAYS_SOFTWARE=1
cp "$1" '/tmp/photo with spaces.jpg'
mkdir -p "$HOME"
/opt/install-licasa-appimage.sh /opt/Licasa.AppImage
desktop-file-validate "$XDG_DATA_HOME/applications/licasa-appimage.desktop"
xdg-mime default licasa-appimage.desktop image/jpeg
test "$(xdg-mime query default image/jpeg)" = licasa-appimage.desktop
"""
X11 = SETUP + """
export DISPLAY=:90
export QT_QPA_PLATFORM=xcb
Xvfb :90 -ac -screen 0 1280x800x24 -nolisten tcp >/tmp/display.log 2>&1 & display_pid=$!
viewer_pid=
trap 'kill "$viewer_pid" "$display_pid" 2>/dev/null || true' EXIT
i=0
while [ ! -S /tmp/.X11-unix/X90 ] && [ "$i" -lt 40 ]; do
    kill -0 "$display_pid" 2>/dev/null || { cat /tmp/display.log; exit 1; }
    sleep 0.25
    i=$((i + 1))
done
test -S /tmp/.X11-unix/X90 || { cat /tmp/display.log; exit 1; }
"$HOME/.local/opt/licasa-appimage/AppRun" '/tmp/photo with spaces.jpg' >/tmp/viewer.log 2>&1 & viewer_pid=$!
i=0
while [ "$i" -lt 80 ]; do
    kill -0 "$viewer_pid" 2>/dev/null || { cat /tmp/viewer.log; exit 1; }
    if xwininfo -root -tree 2>/dev/null | grep -q '"Licasa"'; then
        echo 'Arch X11: Licasa window opened'
        exit 0
    fi
    sleep 0.25
    i=$((i + 1))
done
cat /tmp/viewer.log
echo 'Licasa did not create an X11 window' >&2
exit 1
"""
WAYLAND = SETUP + """
export DISPLAY=:91
export WAYLAND_DISPLAY=wayland-licasa
export QT_QPA_PLATFORM=wayland
export QT_QUICK_BACKEND=software
Xvfb :91 -ac -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 & x11_pid=$!
i=0
while [ ! -S /tmp/.X11-unix/X91 ] && [ "$i" -lt 40 ]; do
    kill -0 "$x11_pid" 2>/dev/null || { cat /tmp/xvfb.log; exit 1; }
    sleep 0.25
    i=$((i + 1))
done
test -S /tmp/.X11-unix/X91 || { cat /tmp/xvfb.log; exit 1; }
weston --backend=x11 --renderer=pixman --socket="$WAYLAND_DISPLAY" --idle-time=0 >/tmp/display.log 2>&1 & display_pid=$!
viewer_pid=
trap 'kill "$viewer_pid" "$display_pid" "$x11_pid" 2>/dev/null || true' EXIT
i=0
while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ "$i" -lt 40 ]; do
    kill -0 "$display_pid" 2>/dev/null || { cat /tmp/display.log; exit 1; }
    sleep 0.25
    i=$((i + 1))
done
test -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" || { cat /tmp/display.log; exit 1; }
grep -q "Compositor capabilities:" /tmp/display.log || { cat /tmp/display.log; exit 1; }
"$HOME/.local/opt/licasa-appimage/AppRun" '/tmp/photo with spaces.jpg' >/tmp/viewer.log 2>&1 & viewer_pid=$!
i=0
while [ "$i" -lt 40 ]; do
    kill -0 "$viewer_pid" 2>/dev/null || { cat /tmp/viewer.log; exit 1; }
    sleep 0.25
    i=$((i + 1))
done
grep -q 'Qt Quick backend (sceneGraphInitialized)' /tmp/viewer.log || {
    cat /tmp/viewer.log
    echo 'Wayland scene graph did not initialize' >&2
    exit 1
}
echo 'Arch Wayland: Licasa stayed open'
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/qualification/appimage/desktop-smoke.json")
    args = parser.parse_args()
    appimage = args.appimage.resolve(strict=True)
    sample = ROOT / "tests/test-assets" / SAMPLE
    assets = ROOT / "tests/test-assets"
    if any(not (assets / path).is_file() for path in DECODE_SAMPLES.values()):
        parser.error("Repository release samples are missing")

    subprocess.run(["docker", "build", "--file",
                    str(ROOT / "packaging/verify-appimage-arch.Dockerfile"),
                    "--tag", IMAGE, str(ROOT / "packaging")], check=True)
    checks = {}
    for name, script in (("arch_x11_window", X11), ("arch_wayland_launch", WAYLAND)):
        command = [
            "docker", "run", "--rm", "--network", "none", "--read-only",
            "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--shm-size", "256m",
            "--user", "65534:65534", "--cap-drop", "ALL",
            "-v", f"{appimage}:/opt/Licasa.AppImage:ro",
            "-v", f"{ROOT / 'packaging/install-licasa-appimage.sh'}:/opt/install-licasa-appimage.sh:ro",
            "-v", f"{sample}:/samples/photo.jpg:ro",
            IMAGE, "sh", "-ec", script, "_", "/samples/photo.jpg",
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(f"{name} failed:\n{result.stdout}\n{result.stderr}")
        checks[name] = True
        print(result.stdout.strip(), flush=True)

    decode_script = (
        "cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null; "
        "appdir=/tmp/squashfs-root; "
        "export LD_LIBRARY_PATH=\"$appdir/usr/lib/licasa/lib:$appdir/usr/lib\"; "
        "export QT_PLUGIN_PATH=\"$appdir/usr/plugins:$appdir/usr/lib/licasa/plugins\"; "
        "export QML2_IMPORT_PATH=\"$appdir/usr/qml\"; "
        "export QML_IMPORT_PATH=\"$QML2_IMPORT_PATH\"; "
        "exec \"$appdir/usr/bin/licasa_diagnostics\" --formats --megapixels 25 \"$@\""
    )
    result = subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "--read-only",
         "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
         "--cap-drop", "ALL", "-e", "QT_QPA_PLATFORM=offscreen",
         "-e", "XDG_RUNTIME_DIR=/tmp/runtime", "-e", "XDG_CACHE_HOME=/tmp/cache",
         "-v", f"{appimage}:/opt/Licasa.AppImage:ro",
         "-v", f"{assets}:/samples:ro", IMAGE, "sh", "-ec", decode_script, "_",
         *(f"/samples/{path}" for path in DECODE_SAMPLES.values())],
        capture_output=True, text=True, timeout=90,
    )
    if result.returncode:
        raise RuntimeError(f"Arch image decoders failed:\n{result.stdout}\n{result.stderr}")
    report = json.loads(result.stdout)
    missing = set(DECODE_SAMPLES) - {name.lower() for name in report["read_formats"]}
    failed = [item["path"] for item in report["samples"] if not item["decoded"]]
    if missing or failed:
        raise RuntimeError(f"Arch missing readers {sorted(missing)} or failed files {failed}")
    for item in report["samples"]:
        for library in item["process"]["mapped_optional_libraries"]:
            if not library.startswith("/tmp/squashfs-root/"):
                raise RuntimeError(f"Arch codec escaped the AppImage bundle: {library}")
    checks["arch_image_decoders"] = len(DECODE_SAMPLES)
    print(f"Arch: decoded {len(DECODE_SAMPLES)} image formats", flush=True)

    dependency_script = (
        "cd /tmp; /opt/Licasa.AppImage --appimage-extract >/dev/null; "
        "export LD_LIBRARY_PATH=/tmp/squashfs-root/usr/lib/licasa/lib:"
        "/tmp/squashfs-root/usr/lib; "
        "find squashfs-root/usr -type f "
        "\\( -name '*.so' -o -name '*.so.*' -o -name licasa "
        "-o -name licasa_diagnostics \\) | while IFS= read -r file; do "
        "missing=$(ldd \"$file\" 2>/dev/null | grep 'not found' || true); "
        "if [ -n \"$missing\" ]; then printf '%s: %s\\n' \"$file\" \"$missing\"; fi; "
        "done"
    )
    result = subprocess.run(
        ["docker", "run", "--rm", "--network", "none", "--read-only",
         "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--user", "65534:65534",
         "--cap-drop", "ALL", "-v", f"{appimage}:/opt/Licasa.AppImage:ro",
         IMAGE, "sh", "-ec", dependency_script],
        capture_output=True, text=True, timeout=90,
    )
    if result.returncode or result.stdout.strip():
        raise RuntimeError(f"Arch missing bundled dependencies:\n{result.stdout}\n{result.stderr}")
    checks["arch_elf_dependencies"] = True
    print("Arch: all packaged ELF dependencies resolved", flush=True)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({
        "appimage_sha256": hashlib.sha256(appimage.read_bytes()).hexdigest(),
        "test_image": IMAGE,
        "checks": checks,
    }, indent=2) + "\n")
    print(f"Arch desktop checks passed: {args.output}")


if __name__ == "__main__":
    main()
