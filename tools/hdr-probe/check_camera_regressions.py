#!/usr/bin/env python3
"""Require the retained camera cases and record each executed QtTest row."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

FIXTURES = {
    "pixel6": ("LICASA_HDR_PIXEL6_FIXTURE", "9db9e5476c9075dfcdb2ba88843c3f81ceba1b515db2ad7cef415828026ba852"),
    "pixel9": ("LICASA_HDR_COMPOUND_CAMERA_FIXTURE", "ac0d8ac4560aa2137316678058c6723aa8a72d009475f0aa1fff87cb7dd8a4e7"),
}


def file_hash(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("pixel6", "pixel9", "decoder", "viewer", "save"):
        parser.add_argument(f"--{name}", required=True, type=Path)
    parser.add_argument("--viewer-qt-prefix", type=Path)
    parser.add_argument("--profile", required=True, choices=("release", "sanitized"))
    args = parser.parse_args()
    env = os.environ.copy()
    report = {"profile": args.profile, "fixtures": {}, "binaries": {}, "rows": {},
              "sanitizers": {key: env.get(key) for key in ("ASAN_OPTIONS", "UBSAN_OPTIONS")}}
    for name, (variable, expected) in FIXTURES.items():
        path = getattr(args, name).resolve()
        digest = file_hash(path) if path.is_file() else None
        report["fixtures"][name] = {"path": str(path), "sha256": digest, "valid": digest == expected}
        env[variable] = str(path)
    for name in ("decoder", "viewer", "save"):
        path = getattr(args, name).resolve()
        report["binaries"][name] = {"path": str(path), "sha256": file_hash(path) if path.is_file() else None}
    rows = (
        ("decoder", "cameraSizedDecode:pixel6", "AsyncTests::cameraSizedDecode(pixel6)", "pixel6", None),
        ("decoder", "cameraSizedDecode:pixel9", "AsyncTests::cameraSizedDecode(pixel9)", "pixel9", None),
        *(("decoder", f"cameraIndependentReconstruction:{camera}-{headroom}",
           f"AsyncTests::cameraIndependentReconstruction({camera}-{headroom})", camera, None)
          for camera in ("pixel6", "pixel9") for headroom in ("1", "1.5", "2", "4", "8")),
        ("viewer", "cameraSizedGainMap", "ViewerHdrTests::cameraSizedGainMap()", "pixel9", "basic"),
        ("viewer", "cameraSizedGainMap", "ViewerHdrTests::cameraSizedGainMap()", "pixel9", "threaded"),
        ("save", "hdrOriginalCopyAndSdrExport:compound-camera",
         "ImageSaveServiceTest::hdrOriginalCopyAndSdrExport(compound-camera)", "pixel9", None),
    )
    for binary, selector, expected, fixture, loop in rows:
        if args.profile == "sanitized" and binary == "save":
            continue  # The required release profile owns the save boundary.
        row = report["rows"][f"{binary}/{selector}/{loop or 'offscreen'}"] = {}
        if not report["fixtures"][fixture]["valid"] or not report["binaries"][binary]["sha256"]:
            row["status"] = "unavailable"
            continue
        child_env = dict(env, QT_QPA_PLATFORM="xcb" if loop else "offscreen",
                         QT_QPA_PLATFORMTHEME="generic")
        if loop:
            child_env["QSG_RENDER_LOOP"] = loop
            if args.viewer_qt_prefix:
                child_env["QT_PLUGIN_PATH"] = str(args.viewer_qt_prefix / "lib/plugins")
                child_env["QML_IMPORT_PATH"] = str(args.viewer_qt_prefix / "lib/qml")
        command = [report["binaries"][binary]["path"], selector]
        try:
            run = subprocess.run(command, env=child_env, capture_output=True, text=True, timeout=45)
            row.update(exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
            passes = re.findall(r"^PASS\s+: (.*)$", run.stdout, re.MULTILINE)
            row["status"] = "passed" if run.returncode == 0 and passes.count(expected) == 1 else "failed"
        except (OSError, subprocess.TimeoutExpired) as error:
            row.update(status="failed", error=str(error))
    print(json.dumps(report, indent=2))
    return int(any(row["status"] != "passed" for row in report["rows"].values()))


if __name__ == "__main__":
    sys.exit(main())
