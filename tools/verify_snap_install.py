#!/usr/bin/env python3
"""Decode release fixtures through the installed, strictly confined Snap."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from verify_installed_release import SAMPLES


ROOT = Path(__file__).resolve().parents[1]


def viewer_probe_script(*, error_log: bool = False, startup_seconds: float = 12) -> str:
    """Exercise GUI startup/logging separately from the headless control path."""
    script = r'''
set -eu
work=$(mktemp -d)
state=
viewer=
cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ -n "$viewer" ]; then
        kill "$viewer" 2>/dev/null || true
        wait "$viewer" 2>/dev/null || true
    fi
    cat "$work/viewer.log" >&2 2>/dev/null || true
    if [ -n "$state" ]; then
        cat "$state/licasa/errors.log" >&2 2>/dev/null || true
        rm -rf -- "$state"
    fi
    rm -rf -- "$work"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
mkdir -m 700 "$work/runtime"
export XDG_RUNTIME_DIR="$work/runtime"
'''
    if error_log:
        script += r'''
mkdir -p "$SNAP_USER_DATA/.local/state"
state=$(mktemp -d "$SNAP_USER_DATA/.local/state/licasa-smoke.XXXXXX")
export XDG_STATE_HOME="$state" XDG_RUNTIME_DIR=relative
'''
    script += r'''
"$SNAP/usr/bin/licasa" "$1" >"$work/viewer.log" 2>&1 & viewer=$!
viewer_is_alive() {
    if ! kill -0 "$viewer" 2>/dev/null; then
        status=0
        wait "$viewer" || status=$?
        viewer=
        echo "Viewer exited before the probe completed (exit $status)." >&2
        if [ "$status" -eq 0 ]; then status=1; fi
        exit "$status"
    fi
}
'''
    if error_log:
        script += r'''
attempt=0
until grep -Fq 'Single-instance IPC disabled' "$state/licasa/errors.log" 2>/dev/null; do
    viewer_is_alive
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 50 ]; then
        echo 'The GUI warning did not reach the fresh confined error log.' >&2
        exit 1
    fi
    sleep 0.1
done
viewer_is_alive
'''
    else:
        script += f"\nsleep {float(startup_seconds)}\nviewer_is_alive\n"
    return script


def run_viewer_probe(script: str, *, phase: str, sample: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        ["snap", "run", "--shell", "licasa", "-c", script, "_", str(sample)],
        cwd=ROOT, capture_output=True, text=True, timeout=30,
    )
    log = output.with_suffix(f".{phase}.log")
    log.write_text(f"phase={phase}\nexit={completed.returncode}\n"
                   f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}")
    if completed.returncode:
        raise RuntimeError(f"Confined Snap {phase} probe failed (exit {completed.returncode}); "
                           f"evidence: {log}\n{completed.stderr}\n{completed.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", help="Expected Git commit embedded by Snapcraft")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    listed = subprocess.run(["snap", "list", "licasa"], capture_output=True, text=True,
                            check=True)
    rows = listed.stdout.splitlines()
    if len(rows) != 2 or rows[1].split()[0:2] != ["licasa", args.version]:
        raise RuntimeError(f"Installed Snap version differs from {args.version}: {listed.stdout}")
    snap_root = Path("/snap/licasa/current").resolve(strict=True)
    commit_file = snap_root / "usr/share/licasa/source-commit"
    if not commit_file.is_file():
        raise RuntimeError("Installed Snap has no source commit; the checked build for this "
                           "release is not available on the selected Store channel")
    source_commit = commit_file.read_text().strip()
    if args.source_sha and source_commit != args.source_sha:
        raise RuntimeError(f"Installed Snap source commit {source_commit} differs from "
                           f"{args.source_sha}")
    diagnostic = snap_root / "usr/bin/licasa_diagnostics"
    if not diagnostic.is_file():
        raise RuntimeError(f"Installed Snap has no diagnostics binary: {diagnostic}")

    samples = [ROOT / "tests/test-assets" / sample for sample in SAMPLES.values()]
    for sample in samples:
        if not sample.is_file():
            raise RuntimeError(f"Missing release fixture: {sample}")
    pictures = Path.home() / "Pictures"
    pictures.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="licasa snap smoke ", dir=pictures) as temp:
        spaced_sample = Path(temp) / "image with spaces.jpg"
        shutil.copy2(samples[0], spaced_sample)
        command = [
            "snap", "run", "--shell", "licasa", "-c",
            'export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software; '
            'exec "$SNAP/usr/bin/licasa_diagnostics" --formats --megapixels 25 "$@"',
            "_", *map(str, samples), str(spaced_sample),
        ]
        completed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                   timeout=180)
        if completed.returncode:
            raise RuntimeError(f"Confined Snap decode failed:\n{completed.stderr}\n"
                               f"{completed.stdout}")
        report = json.loads(completed.stdout)

        # Start and stop the viewer inside confinement: the host cannot always
        # signal a process confined by the Snap's AppArmor profile.
        run_viewer_probe(viewer_probe_script(), phase="cold-start",
                         sample=spaced_sample, output=args.output)
        run_viewer_probe(viewer_probe_script(error_log=True), phase="runtime-error-log",
                         sample=spaced_sample, output=args.output)

    available = set(report["read_formats"])
    missing = set(SAMPLES) - available
    if missing:
        raise RuntimeError(f"Installed Snap readers missing: {sorted(missing)}")
    if report["before_decode"]["mapped_optional_libraries"]:
        raise RuntimeError("Optional codec libraries loaded before the first decode")
    quick = [Path(path) for path in report["before_decode"]["mapped_qt_libraries"]
             if "libQt6Quick.so." in path]
    if len(quick) != 1 or not quick[0].is_relative_to(snap_root):
        raise RuntimeError(f"Installed Qt Quick backport was not loaded from the Snap: {quick}")

    expected = [*SAMPLES, "jpeg"]
    for image_format, sample in zip(expected, report["samples"], strict=True):
        if not sample["decoded"] or sample["format"] != image_format:
            raise RuntimeError(f"Confined {image_format} decode failed: {sample}")
        if image_format == "dng" and sample["raw_development"]:
            raise RuntimeError("RAW fixture used full development instead of its preview")
        for library in sample["process"]["mapped_optional_libraries"]:
            if not Path(library).is_relative_to(snap_root):
                raise RuntimeError(f"Codec library escaped the Snap: {library}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    report["source_commit"] = source_commit
    report["viewer_cold_start"] = True
    report["runtime_error_log_probe"] = True
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Confined Snap launched the viewer and decoded {len(expected)} samples: {args.output}")


if __name__ == "__main__":
    main()
