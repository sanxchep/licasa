"""Operational acceptance checks must survive every Python optimization mode."""

import ast
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODES = ((), ("-O",), ("-OO",), ("env",))
RAW_DIAGNOSTIC = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
sample = Path(sys.argv[-1]).name
result = {
    "initial_preview_available": sample != "no-preview.dng",
    "raw_fast_intermediate_ready": False,
    "first_raster_full_detail": sample == "no-preview.dng",
    "preview_max_heartbeat_gap_ms": 5,
    "automatic_full_detail_ready": True, "automatic_full_detail_ms": 10,
    "explicit_full_detail_ready": True, "full_edit_kept_detail": True,
    "full_first_edit_ready_ms": 10, "full_repeat_edit_ready_ms": 10,
    "full_max_heartbeat_gap_ms": 5, "full_detail_blocked_by_policy": True,
}
result.update(json.loads(os.environ.get("RAW_INVALID_RESULT", "{}")))
print(json.dumps(result))
print("Decoded base cached: (fixture, full)", file=sys.stderr)
print("Decoded base cache hit: (fixture, full)", file=sys.stderr)
print("Decoded base cache hit: (fixture, full)", file=sys.stderr)
'''
PACKAGE_DIAGNOSTIC = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
prefix = Path(__file__).resolve().parents[1]
if "--viewer" in sys.argv:
    result = {"image_failed": True, "first_error_presented_ms": 1}
elif sys.argv[sys.argv.index("--megapixels") + 1] == "100":
    result = {"qt_allocation_mib": 763, "before_decode": {"storage_write_bytes": 0},
              "samples": [{"decoded": True, "largest_native_raster_pixels": 48000000,
                           "decoded_width": 1200, "decoded_height": 900,
                           "process": {"storage_write_bytes": 0, "mapped_optional_libraries": []}}]}
else:
    result = {"qt_allocation_mib": int(os.environ.get("PACKAGE_ALLOCATION_MIB", "191")),
              "before_decode": {"mapped_optional_libraries": [], "storage_write_bytes": 0,
                                "mapped_qt_libraries": [str(prefix / "lib/libQt6Quick.so.6")]},
              "read_mime_types": ["image/png"], "samples": []}
print(json.dumps(result))
'''


class QualificationChecksTests(unittest.TestCase):
    def run_python(self, mode, arguments, *, environment=None):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONPATH=str(ROOT / "tools"))
        env.pop("PYTHONOPTIMIZE", None)
        if mode == ("env",):
            env["PYTHONOPTIMIZE"] = "2"
            mode = ()
        env.update(environment or {})
        return subprocess.run([sys.executable, *mode, *arguments], env=env,
                              capture_output=True, text=True, timeout=15)

    def test_raw_success_and_failed_evidence_are_consistent_in_all_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            diagnostic = root / "diagnostic fixture"
            diagnostic.write_text(RAW_DIAGNOSTIC)
            diagnostic.chmod(0o755)
            output = root / "reports/raw.json"
            arguments = [str(ROOT / "tools/check_raw_behavior.py"), str(diagnostic),
                         "--output", str(output)]
            for mode in MODES:
                with self.subTest(mode=mode, valid=True):
                    run = self.run_python(mode, arguments)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertEqual(set(json.loads(output.read_text())),
                                     {"small.dng", "no-preview.dng", "large-48mp.dng"})
                for invalid in ({"preview_max_heartbeat_gap_ms": 100},
                                {"full_max_heartbeat_gap_ms": 250},
                                {"automatic_full_detail_ready": False}):
                    with self.subTest(mode=mode, invalid=invalid):
                        run = self.run_python(mode, arguments, environment={
                            "RAW_INVALID_RESULT": json.dumps(invalid)})
                        self.assertNotEqual(run.returncode, 0, run.stdout)
                        self.assertNotIn("RAW viewer behavior verified:", run.stdout)
                        report = json.loads(output.read_text())
                        self.assertEqual(set(report), {"small.dng"})
                        for key, value in invalid.items():
                            self.assertEqual(report["small.dng"][key], value)

    def test_native_script_producer_rejects_wrong_installation_in_all_modes(self):
        for mode in MODES:
            for installation in ("native", "appimage-native"):
                with self.subTest(mode=mode, installation=installation), \
                        tempfile.TemporaryDirectory() as temporary:
                    script = ("from pathlib import Path; "
                              "from verify_native_lifecycle import install_scripts; "
                              f"install_scripts(Path({temporary!r}), {installation!r})")
                    run = self.run_python(mode, ["-c", script])
                    scripts = list((Path(temporary) / "DEBIAN").glob("*"))
                    if installation == "native":
                        self.assertEqual(run.returncode, 0, run.stderr)
                        self.assertTrue(scripts)
                    else:
                        self.assertNotEqual(run.returncode, 0, run.stdout)
                        self.assertEqual(scripts, [])

    def test_package_qualifier_checks_real_diagnostic_output_in_all_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            diagnostic = root / "bin/licasa_diagnostics"
            diagnostic.parent.mkdir()
            diagnostic.write_text(PACKAGE_DIAGNOSTIC)
            diagnostic.chmod(0o755)
            desktop = root / "share/applications/licasa.desktop"
            desktop.parent.mkdir(parents=True)
            desktop.write_text("[Desktop Entry]\nMimeType=image/png;\n")
            output = root / "package.json"
            script = ("from pathlib import Path; import smoke_package as package; "
                      "package.VENDOR_SAMPLES = {}; "
                      f"package.ROOT = Path({temporary!r}); package.main()")
            arguments = ["-c", script, temporary, "--vendor-cache", str(root / "cache"),
                         "--output", str(output)]
            for mode in MODES:
                with self.subTest(mode=mode, valid=True):
                    run = self.run_python(mode, arguments)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertIn("qualification", json.loads(output.read_text()))
                with self.subTest(mode=mode, valid=False):
                    run = self.run_python(mode, arguments, environment={
                        "PACKAGE_ALLOCATION_MIB": "999"})
                    self.assertNotEqual(run.returncode, 0, run.stdout)
                    self.assertEqual(json.loads(output.read_text()), {})
                    self.assertNotIn("Verified ", run.stdout)

    def test_generated_native_peer_rejects_leaked_installation_environment(self):
        script = "from verify_native_lifecycle import VIEWER; exec(VIEWER)"
        with tempfile.TemporaryDirectory() as temporary:
            for mode in MODES:
                for leaked in ({"SNAP": "/leaked-snap"}, {"APPDIR": "/wrong-install"}):
                    with self.subTest(mode=mode, leaked=leaked):
                        run = self.run_python(mode, ["-c", script], environment={
                            **leaked, "XDG_RUNTIME_DIR": str(Path(temporary) / "absent")})
                        self.assertNotEqual(run.returncode, 0)
                        self.assertIn("AssertionError", run.stderr)
                        self.assertFalse(list(Path(temporary).iterdir()))

    def test_operational_python_contains_no_removable_assertion_gates(self):
        for path in sorted((ROOT / "tools").glob("*.py")):
            tree = ast.parse(path.read_text())
            with self.subTest(path=path.name):
                self.assertEqual([node.lineno for node in ast.walk(tree)
                                  if isinstance(node, ast.Assert)], [])
                # Generated recovery/manager peers run in separate interpreters.
                self.assertEqual([node.lineno for node in ast.walk(tree)
                                  if isinstance(node, ast.Constant) and isinstance(node.value, str)
                                  and re.search(r"^\s*assert\b", node.value, re.MULTILINE)], [])


if __name__ == "__main__":
    unittest.main()
