#!/usr/bin/env python3
"""Run installed-package AT-SPI contracts in a disposable Debian 13 MATE session."""
from __future__ import annotations
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = r'''
set -eu
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME"
chmod 700 "$HOME" "$XDG_RUNTIME_DIR"
cd /tmp
# Artifact downloads preserve bytes, but not executable permissions. Keep the
# mounted candidate immutable and extract an executable copy in disposable /tmp.
cp /package/Licasa.AppImage /tmp/Licasa.AppImage
chmod 755 /tmp/Licasa.AppImage
/tmp/Licasa.AppImage --appimage-extract >/reports/extraction.log 2>&1
Xvfb :93 -ac -screen 0 1360x900x24 -nolisten tcp >/reports/xvfb.log 2>&1 &
xvfb_pid=$!
cleanup() {
    chmod -R a+rX /reports/speechd 2>/dev/null || true
    kill "$xvfb_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
attempt=0
while ! xdotool getdisplaygeometry >/dev/null 2>&1 && [ "$attempt" -lt 50 ]; do
    sleep 0.1
    attempt=$((attempt + 1))
done
xdotool getdisplaygeometry >/dev/null
/tmp/squashfs-root/usr/bin/licasa_diagnostics --formats /samples/photo.png > /reports/runtime-formats.json 2>/reports/runtime-formats.log
# Enable the desktop's actual accessibility infrastructure before launching Qt.
gsettings set org.mate.interface accessibility true
mate-session --failsafe --disable-acceleration-check >/reports/mate-session.log 2>&1 &
mate_pid=$!
attempt=0
while ! pgrep -x marco >/dev/null && [ "$attempt" -lt 80 ]; do
    sleep 0.1
    attempt=$((attempt + 1))
done
pgrep -x mate-session >/dev/null
pgrep -x marco >/dev/null
# A dummy Speech Dispatcher output accepts the real Orca speech requests. Its
# debug log retains the generated utterances without requiring audio hardware.
mkdir -p "$HOME/speechd/modules" /reports/speechd
: > "$HOME/speechd/modules/dummy.conf"
cat > "$HOME/speechd/speechd.conf" <<'EOF'
AddModule "dummy" "sd_dummy" "dummy.conf"
DefaultModule "dummy"
LogLevel 5
AudioOutputMethod "alsa"
AudioALSADevice "null"
EOF
speech-dispatcher -d -t 0 -C "$HOME/speechd" -L /reports/speechd >/reports/speech-dispatcher.log 2>&1
orca --replace --enable speech --disable braille --debug-file /reports/orca-debug.log >/reports/orca.log 2>&1 &
orca_pid=$!
python3 /qualification/tools/verify_desktop_accessibility.py --app /tmp/squashfs-root/AppRun --sample /samples/photo.png --output /reports/desktop.json
status=$?
kill "$orca_pid" "$mate_pid" 2>/dev/null || true
exit "$status"
'''


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--image", default="licasa-mate-accessibility:debian13")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    artifact = args.appimage.resolve(strict=True)
    output = args.output.absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    raw = Path(tempfile.mkdtemp(prefix=output.stem + "-desktop-", dir=output.parent))
    raw.chmod(0o777) # The disposable container uses the existing nobody identity.
    container_name = "licasa-mate-" + uuid.uuid4().hex[:12]
    report = {"schema_version": 1, "kind": "accessibility", "target_id": "debian13-mate-accessibility",
              "status": "failed", "artifact_sha256": digest(artifact), "scenarios": [],
              "desktop": "MATE", "session": "virtual MATE X11", "protocol": "x11", "image": args.image,
              "coverage_limits": ["Orca speech is enabled with dummy output: generated speech is checked, but listening and real audio hardware acceptance remain required.",
                                  "This is an actual MATE session on a virtual X11 display, without physical GPU or display-manager login coverage."]}
    try:
        if not args.skip_build:
            build = subprocess.run(["docker", "build", "-f", str(ROOT / "packaging/verify-mate-accessibility.Dockerfile"),
                                    "-t", args.image, str(ROOT)], capture_output=True, text=True, timeout=600)
            (raw / "image-build.log").write_text(build.stdout + "\n" + build.stderr)
            if build.returncode: raise RuntimeError("MATE test image build failed; see image-build.log")
        identity = subprocess.run(["docker", "image", "inspect", args.image, "--format", "{{.Id}}"],
                                  capture_output=True, text=True, check=True, timeout=15)
        report["image_id"] = identity.stdout.strip()
        sample = ROOT / "tests/test-assets/dimensions/odd-photo-997x613.png"
        command = ["docker", "run", "--name", container_name, "--rm", "--network", "none", "--read-only", "--cap-drop", "ALL",
                   "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--shm-size", "256m", "--pids-limit", "256",
                   "--user", "65534:65534",
                   "-e", "HOME=/tmp/home", "-e", "XDG_CONFIG_HOME=/tmp/config", "-e", "XDG_STATE_HOME=/tmp/state",
                   "-e", "XDG_CACHE_HOME=/tmp/cache", "-e", "XDG_RUNTIME_DIR=/tmp/runtime", "-e", "DISPLAY=:93",
                   "-e", "XDG_CURRENT_DESKTOP=MATE", "-e", "DESKTOP_SESSION=mate", "-e", "QT_QPA_PLATFORM=xcb",
                   "-e", "LICASA_VIRTUAL_DESKTOP=1", "-e", "QT_QUICK_BACKEND=software",
                   "-v", f"{artifact}:/package/Licasa.AppImage:ro",
                   "-v", f"{sample}:/samples/photo.png:ro", "-v", f"{ROOT / 'tools'}:/qualification/tools:ro",
                   "-v", f"{raw}:/reports:rw", args.image, "sh", "-ec",
                   'mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"; exec dbus-run-session -- sh -ec "$1"',
                   "_", SCRIPT]
        result = subprocess.run(command, capture_output=True, text=True, timeout=150)
        (raw / "container.log").write_text(result.stdout + "\n" + result.stderr)
        report["exit_code"] = result.returncode
        if (raw / "runtime-formats.json").exists():
            formats = json.loads((raw / "runtime-formats.json").read_text())
            report["qt_version"] = formats.get("qt_version")
            report["runtime"] = {"qt_version": formats.get("qt_version"), "process": formats.get("before_decode", {})}
        if (raw / "desktop.json").exists():
            desktop = json.loads((raw / "desktop.json").read_text())
            report["desktop_report"] = desktop
            report["scenarios"] = desktop.get("scenarios", [])
            if desktop.get("process"):
                report.setdefault("runtime", {})["process"] = desktop["process"]
            report["coverage_limits"].extend(desktop.get("coverage_limits", []))
            if result.returncode or desktop.get("status") != "passed":
                raise RuntimeError(desktop.get("error", "Desktop accessibility check failed"))
        else:
            raise RuntimeError("Desktop check did not produce AT-SPI evidence")
        orca = raw / "orca-debug.log"
        if not orca.exists() or not orca.stat().st_size:
            raise RuntimeError("Orca did not produce its event/debug log")
        orca_text = orca.read_text(errors="replace")
        utterances = re.findall(r"SPEECH OUTPUT: '([^']*)'", orca_text)
        (raw / "orca-utterances.json").write_text(json.dumps(utterances, indent=2) + "\n")
        if not utterances:
            raise RuntimeError("Orca did not generate any speech output")
        numeric_tails = [value for value in utterances
                         if re.search(r"[.,]\d{6,}|(?:nine\s*){4,}|9{6,}", value, re.IGNORECASE)]
        if numeric_tails:
            raise RuntimeError("Orca generated numeric speech tails: " + repr(numeric_tails))
        missing_values = [value for value in ("10", "-10", "0") if value not in utterances]
        if missing_values:
            raise RuntimeError("Orca did not speak required adjustment values: " + repr(missing_values))
        report["orca_speech"] = {"output": "Speech Dispatcher dummy", "utterances": utterances,
                                 "numeric_tails": numeric_tails,
                                 "checked_adjustment_values": ["10", "-10", "0"]}
        report["scenarios"].append({"name": "orca_generated_adjustment_speech_is_readable", "status": "passed",
                                    "detail": "Orca spoke clean positive, negative and zero values, without long decimal tails."})
        # Match Orca against the dialog actually observed by AT-SPI in this run.
        # UI copy can change; an obsolete hard-coded title must not reject a
        # correctly named dialog or let unrelated old speech qualify it.
        save_dialog = next((row.get("detail") for row in report["scenarios"]
                            if row.get("name") == "save_as_dialog_name_and_role"
                            and row.get("status") == "passed"), None)
        if not isinstance(save_dialog, str) or not save_dialog.strip():
            raise RuntimeError("Desktop report lacks a named successful save dialog")
        observed = {name: name in orca_text for name in
                    ("Open image", "Exposure", save_dialog, "Image could not be opened")}
        report["orca_observed_controls"] = observed
        missing_controls = [name for name, present in observed.items() if not present]
        if missing_controls:
            raise RuntimeError("Orca did not process required production UI events: " + repr(missing_controls))
        report["scenarios"].append({"name": "orca_processes_production_events", "status": "passed",
                                    "detail": "Open, adjustment, save and error-alert events appear in Orca Qt toolkit log."})
        report["orca_log"] = str(orca)
        report["status"] = "passed"
        return 0
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        return 1
    finally:
        try:
            cleanup = subprocess.run(["docker", "rm", "-f", container_name], capture_output=True, text=True, timeout=15)
            (raw / "container-cleanup.log").write_text(cleanup.stdout + cleanup.stderr)
        except (OSError, subprocess.SubprocessError) as error:
            (raw / "container-cleanup.log").write_text(str(error) + "\n")
        report["evidence_directory"] = str(raw)
        output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({key: value for key, value in report.items() if key != "desktop_report"}))

if __name__ == "__main__":
    raise SystemExit(main())
