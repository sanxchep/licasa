#!/usr/bin/env python3
"""Execute the confined-smoke shell probes against a deterministic GUI fixture."""

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import verify_snap_install as smoke


class SnapInstallProbeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="snap smoke with spaces ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        binaries = self.root / "bin"
        binaries.mkdir()
        snap = binaries / "snap"
        snap.write_text('#!/bin/sh\nset -eu\ntest "$1 $2 $3" = "run --shell licasa"\n'
                        'shift 3\nexec /bin/sh "$@"\n')
        snap.chmod(0o755)
        self.payload = self.root / "payload"
        app = self.payload / "usr/bin/licasa"
        app.parent.mkdir(parents=True)
        app.write_text(f'#!{sys.executable}\n' + '''import json, os, sys, time
from pathlib import Path
with open(os.environ['CALLS'], 'a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\\n')
if '--quit' in sys.argv:
    print('{"result":"broken"}')
    raise SystemExit(9)  # Headless control does not initialize the GUI logger.
if os.environ.get('BEHAVIOR') == 'early-exit':
    print('intentional viewer failure', file=sys.stderr, flush=True)
    raise SystemExit(23)
if os.environ.get('XDG_RUNTIME_DIR') == 'relative' and os.environ.get('BEHAVIOR') != 'no-warning':
    target = Path(os.environ['XDG_STATE_HOME']) / 'licasa/errors.log'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('Single-instance IPC disabled: XDG_RUNTIME_DIR is unavailable or relative.\\n')
time.sleep(60)
''')
        app.chmod(0o755)
        self.data = self.root / "snap data"
        previous = self.data / ".local/state/licasa/errors.log"
        previous.parent.mkdir(parents=True)
        self.previous = previous
        previous.write_text("Single-instance IPC disabled: previous unrelated launch\n")
        self.output = self.root / "evidence/report.json"
        self.photo = self.root / 'photo with spaces "quotes" and $literal.jpg'
        self.photo.touch()
        self.environment = dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ["PATH"],
                                SNAP=str(self.payload), SNAP_USER_DATA=str(self.data),
                                CALLS=str(self.root / "calls.jsonl"))

    def probe(self, *, error_log=False):
        with patch.dict(os.environ, self.environment, clear=True):
            smoke.run_viewer_probe(smoke.viewer_probe_script(error_log=error_log, startup_seconds=0.3),
                                   phase="runtime-error-log" if error_log else "cold-start",
                                   sample=self.photo, output=self.output)

    def test_gui_start_and_fresh_logging_pass_without_using_headless_quit(self):
        self.probe()
        self.probe(error_log=True)
        calls = [json.loads(line) for line in (self.root / "calls.jsonl").read_text().splitlines()]
        self.assertEqual(calls, [[str(self.photo)], [str(self.photo)]])
        self.assertIn("Single-instance IPC disabled", self.output.with_suffix(".runtime-error-log.log").read_text())
        self.assertEqual(self.previous.read_text(), "Single-instance IPC disabled: previous unrelated launch\n")
        self.assertFalse(list(self.data.glob(".local/state/licasa-smoke.*")))

    def test_cold_start_failure_preserves_the_real_exit_and_output(self):
        self.environment["BEHAVIOR"] = "early-exit"
        with self.assertRaisesRegex(RuntimeError, r"cold-start probe failed \(exit 23\)"):
            self.probe()
        log = self.output.with_suffix(".cold-start.log").read_text()
        self.assertIn("intentional viewer failure", log)
        self.assertIn("Viewer exited before the probe completed", log)

    def test_logging_failure_is_reported_as_logging_with_viewer_output(self):
        self.environment["BEHAVIOR"] = "early-exit"
        with self.assertRaisesRegex(RuntimeError, r"runtime-error-log probe failed \(exit 23\)"):
            self.probe(error_log=True)
        self.assertIn("intentional viewer failure", self.output.with_suffix(".runtime-error-log.log").read_text())

    def test_previous_warning_cannot_satisfy_a_missing_fresh_warning(self):
        self.environment["BEHAVIOR"] = "no-warning"
        with self.assertRaisesRegex(RuntimeError, "fresh confined error log"):
            self.probe(error_log=True)
        self.assertTrue(self.previous.is_file())
        self.assertFalse(list(self.data.glob(".local/state/licasa-smoke.*")))


if __name__ == "__main__":
    unittest.main()
