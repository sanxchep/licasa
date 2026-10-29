#!/usr/bin/env python3
"""Desktop retries must use fresh evidence and preserve failure reports."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import verify_mate_accessibility_container as mate
import verify_appimage_picker_container as picker


class MateWrapperTests(unittest.TestCase):
    def run_check(self, directory, *, orca=True, utterances=("10", "-10", "0"), cleanup_timeout=False,
                  save_dialog="Save as a copy", spoken_dialog=None):
        artifact = directory / "Licasa.AppImage"
        artifact.write_bytes(b"fixture package")
        output = directory / "report.json"
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            if command[1:3] == ["image", "inspect"]:
                return subprocess.CompletedProcess(command, 0, "sha256:installed-image\n", "")
            if command[1] == "run":
                mount = next(value for value in command if value.endswith(":/reports:rw"))
                raw = Path(mount.removesuffix(":/reports:rw"))
                (raw / "runtime-formats.json").write_text(json.dumps({"qt_version": "6.4.2", "before_decode": {}}))
                (raw / "desktop.json").write_text(json.dumps({"status": "passed", "scenarios": [
                    {"name": "save_as_dialog_name_and_role", "status": "passed", "detail": save_dialog}]}))
                if orca:
                    spoken_name = save_dialog if spoken_dialog is None else spoken_dialog
                    (raw / "orca-debug.log").write_text(
                        f"Open image Exposure {spoken_name} Image could not be opened\n" +
                        "\n".join(f"SPEECH OUTPUT: '{value}'" for value in utterances))
                return subprocess.CompletedProcess(command, 0, "", "")
            if cleanup_timeout:
                raise subprocess.TimeoutExpired(command, 15)
            return subprocess.CompletedProcess(command, 0, "", "")
        args = ["verify_mate_accessibility_container.py", str(artifact), "--output", str(output), "--skip-build"]
        with patch.object(sys, "argv", args), patch.object(mate.subprocess, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            result = mate.main()
        return result, json.loads(output.read_text()), calls

    def test_retry_cannot_borrow_orca_events_from_previous_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stale = directory / "report-desktop"
            stale.mkdir()
            (stale / "orca-debug.log").write_text("Open image Exposure Save edited image as Image could not be opened")
            result, report, calls = self.run_check(directory, orca=False)
            self.assertEqual(result, 1)
            self.assertEqual(report["status"], "failed")
            self.assertIn("Orca did not produce", report["error"])
            self.assertNotEqual(Path(report["evidence_directory"]), stale)
            self.assertEqual(calls[-1][:3], ["docker", "rm", "-f"])

    def test_cleanup_timeout_does_not_discard_desktop_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, calls = self.run_check(Path(temporary), cleanup_timeout=True)
            self.assertEqual(result, 0)
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["artifact_sha256"], mate.digest(Path(temporary) / "Licasa.AppImage"))
            cleanup = Path(report["evidence_directory"]) / "container-cleanup.log"
            self.assertIn("timed out", cleanup.read_text())
            self.assertEqual(calls[-1][-1], calls[-2][calls[-2].index("--name") + 1])

    def test_repeated_nine_speech_fails_even_with_correct_accessible_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary),
                utterances=("10", "-10", "0", "0.09999999999999999"))
            self.assertEqual(result, 1)
            self.assertIn("numeric speech tails", report["error"])

    def test_event_processing_without_generated_speech_is_insufficient(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary), utterances=())
            self.assertEqual(result, 1)
            self.assertIn("did not generate any speech", report["error"])

    def test_required_signed_values_must_reach_orca_speech(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary), utterances=("10", "0"))
            self.assertEqual(result, 1)
            self.assertIn("required adjustment values", report["error"])

    def test_copy_dialog_event_uses_its_observed_accessible_name(self):
        for name in ("Save as a copy", "Save a separate copy"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                result, report, _ = self.run_check(Path(temporary), save_dialog=name)
                self.assertEqual(result, 0, report.get("error"))
                self.assertTrue(report["orca_observed_controls"][name])

    def test_obsolete_dialog_speech_cannot_qualify_the_current_dialog(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary), spoken_dialog="Save edited image as")
            self.assertEqual(result, 1)
            self.assertIn("Save as a copy", report["error"])
            self.assertFalse(report["orca_observed_controls"]["Save as a copy"])

    def test_copy_dialog_requires_a_nonempty_observed_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary), save_dialog=" ")
            self.assertEqual(result, 1)
            self.assertIn("named successful save dialog", report["error"])

    def test_downloaded_nonexecutable_candidate_is_copied_without_changing_original(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "package/Licasa.AppImage"
            package.parent.mkdir()
            package.write_text('#!/bin/sh\ntest "$1" = --appimage-extract\nprintf extracted\n')
            package.chmod(0o644)
            reports = root / "reports"
            reports.mkdir()
            work = root / "work"
            work.mkdir()
            before = mate.digest(package)
            setup = mate.SCRIPT.split("Xvfb :93", 1)[0]
            setup = setup.replace('/package/Licasa.AppImage', str(package))
            setup = setup.replace('/tmp/Licasa.AppImage', str(work / 'Licasa.AppImage'))
            setup = setup.replace('/reports/extraction.log', str(reports / 'extraction.log'))
            setup = setup.replace('cd /tmp', 'cd "' + str(work) + '"')
            import os
            subprocess.run(["sh", "-ec", setup], check=True,
                env=dict(os.environ, HOME=str(root / "home"), XDG_RUNTIME_DIR=str(root / "runtime"),
                         XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(root / "state"),
                         XDG_CACHE_HOME=str(root / "cache")))
            self.assertEqual((reports / "extraction.log").read_text(), "extracted")
            self.assertEqual(mate.digest(package), before)
            self.assertEqual(package.stat().st_mode & 0o777, 0o644)

    def test_picker_stages_readonly_nonexecutable_candidate_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.AppImage"
            candidate.write_text('#!/bin/sh\nset -eu\ntest -x "$0"\ntest "$1" = --appimage-extract\nprintf extracted\n')
            candidate.chmod(0o444)
            reports = root / "reports"
            reports.mkdir()
            work = root / "work"
            work.mkdir()
            before = mate.digest(candidate)
            setup = picker.SESSION.split('if [ "$LICASA_PICKER_REMOVE_NATIVE"', 1)[0]
            setup = setup.replace('/candidate.AppImage', str(candidate))
            setup = setup.replace('/tmp/Licasa.AppImage', str(work / 'Licasa.AppImage'))
            setup = setup.replace('/reports/extract.log', str(reports / 'extract.log'))
            setup = setup.replace('cd /tmp', 'cd "' + str(work) + '"')
            import os
            subprocess.run(["sh", "-ec", setup], check=True,
                env=dict(os.environ, HOME=str(root / "home"), XDG_RUNTIME_DIR=str(root / "runtime"),
                         XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(root / "state"),
                         XDG_CACHE_HOME=str(root / "cache")))
            self.assertEqual((reports / "extract.log").read_text(), "extracted")
            self.assertEqual((work / "Licasa.AppImage").stat().st_mode & 0o777, 0o755)
            self.assertEqual(mate.digest(candidate), before)
            self.assertEqual(candidate.stat().st_mode & 0o777, 0o444)


if __name__ == "__main__":
    unittest.main()
