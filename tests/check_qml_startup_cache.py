#!/usr/bin/env python3
"""Open the production viewer with a fresh cache without compiling shared JavaScript."""

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile


def check(executable):
    report = {"success": False}
    with tempfile.TemporaryDirectory(prefix="licasa-qml-startup-cache-") as scratch:
        base = Path(scratch)
        env = dict(os.environ)
        # Exercise normal compiled-unit lookup, independently of developer overrides.
        for key in ("QML_DISABLE_DISK_CACHE", "QML_FORCE_DISK_CACHE", "QML_DISK_CACHE",
                    "QML_DISK_CACHE_PATH"):
            env.pop(key, None)
        env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   QSG_RENDER_LOOP="basic")
        for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                          ("XDG_RUNTIME_DIR", "runtime"), ("XDG_STATE_HOME", "state"),
                          ("TMPDIR", "tmp")):
            folder = base / name
            folder.mkdir(mode=0o700)
            env[key] = str(folder)
        image = base / "photo.ppm"
        image.write_bytes(b"P6\n32 24\n255\n" + bytes([53, 117, 201]) * (32 * 24))
        command = [str(executable.resolve()), "--viewer", "--megapixels", "25", str(image)]
        report["command"] = command
        try:
            child = subprocess.run(command, env=env, capture_output=True, text=True, timeout=40)
            report.update(returncode=child.returncode, stdout=child.stdout, stderr=child.stderr)
            if child.returncode:
                raise RuntimeError("Viewer failed before the startup-cache check")
            metrics = json.loads(child.stdout)
            report["metrics"] = metrics
            first_image = metrics.get("first_preview_presented_ms")
            if (metrics.get("error") or metrics.get("image_failed")
                    or metrics.get("image_status_at_settle") != 1
                    or [metrics.get("natural_image_width"), metrics.get("natural_image_height")]
                    != [32, 24]
                    or not isinstance(first_image, (int, float))
                    or not math.isfinite(first_image) or first_image <= 0):
                raise RuntimeError("Viewer did not present the requested image")
            if "Binding loop detected" in child.stderr:
                raise RuntimeError("Viewer emitted a binding loop")
            report["runtime_javascript_cache_files"] = sorted(
                str(p.relative_to(base / "cache"))
                for p in (base / "cache").rglob("*.jsc"))
            if report["runtime_javascript_cache_files"]:
                raise RuntimeError("Shared JavaScript compiled into disk cache during opening")
            report["success"] = True
        except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
            report["error"] = str(error)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = check(args.executable)
    encoded = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")
    raise SystemExit(not report["success"])


if __name__ == "__main__":
    main()
