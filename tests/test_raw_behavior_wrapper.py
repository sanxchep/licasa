"""Keep strict heartbeat assertions and evidence from a failed RAW probe."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("raw_behavior_wrapper", ROOT / "tools/check_raw_behavior.py")
wrapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wrapper)


class RawBehaviorWrapperTests(unittest.TestCase):
    def probe(self, *, no_preview=False, preview_gap=5, full_gap=5):
        return SimpleNamespace(returncode=0, stdout=json.dumps({
            "initial_preview_available": not no_preview, "raw_fast_intermediate_ready": False,
            "first_raster_full_detail": no_preview, "preview_max_heartbeat_gap_ms": preview_gap,
            "automatic_full_detail_ready": True, "automatic_full_detail_ms": 10,
            "explicit_full_detail_ready": True, "full_edit_kept_detail": True,
            "full_first_edit_ready_ms": 10, "full_repeat_edit_ready_ms": 10,
            "full_max_heartbeat_gap_ms": full_gap,
        }), stderr="Decoded base cached: (fixture, full)\n"
                    "Decoded base cache hit: (fixture, full)\n"
                    "Decoded base cache hit: (fixture, full)\n")

    def test_failed_preview_heartbeat_stays_fatal_and_retains_the_failing_case(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "reports/raw.json"
            result = self.probe(preview_gap=139.456344)
            with patch.object(sys, "argv", ["check", "/diagnostic", "--output", str(output)]), \
                    patch.object(wrapper.subprocess, "run", return_value=result):
                with self.assertRaises(AssertionError):
                    wrapper.main()
            self.assertEqual(json.loads(output.read_text()), {"small.dng": json.loads(result.stdout)})

    def test_failed_full_heartbeat_retains_completed_and_failing_cases(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "reports/raw.json"
            results = [self.probe(), self.probe(no_preview=True, full_gap=250)]
            with patch.object(sys, "argv", ["check", "/diagnostic", "--output", str(output)]), \
                    patch.object(wrapper.subprocess, "run", side_effect=results):
                with self.assertRaises(AssertionError):
                    wrapper.main()
            report = json.loads(output.read_text())
            self.assertEqual(set(report), {"small.dng", "no-preview.dng"})
            self.assertEqual(report["no-preview.dng"]["full_max_heartbeat_gap_ms"], 250)

    def test_failed_process_cannot_leave_a_previous_successful_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "raw.json"
            output.write_text('{"old-success": true}\n')
            result = SimpleNamespace(returncode=2, stdout="diagnostic failure", stderr="test failure")
            with patch.object(sys, "argv", ["check", "/diagnostic", "--output", str(output)]), \
                    patch.object(wrapper.subprocess, "run", return_value=result):
                with self.assertRaisesRegex(RuntimeError, "exit code 2"):
                    wrapper.main()
            self.assertEqual(json.loads(output.read_text()), {})


if __name__ == "__main__":
    unittest.main()
