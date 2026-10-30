#!/usr/bin/env python3
"""Desktop retries must use fresh evidence and preserve failure reports."""
import contextlib
import io
import json
import os
from pathlib import Path
import re
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
                  save_dialog="Save as a copy", spoken_dialog=None, container_exit=0,
                  desktop=True, phase=None):
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
                if desktop:
                    (raw / "desktop.json").write_text(json.dumps({"status": "passed", "scenarios": [
                        {"name": "save_as_dialog_name_and_role", "status": "passed", "detail": save_dialog}]}))
                if phase:
                    (raw / "bootstrap-phase.txt").write_text(phase + "\n")
                if orca:
                    spoken_name = save_dialog if spoken_dialog is None else spoken_dialog
                    (raw / "orca-debug.log").write_text(
                        f"Open image Exposure {spoken_name} Image could not be opened\n" +
                        "\n".join(f"SPEECH OUTPUT: '{value}'" for value in utterances))
                return subprocess.CompletedProcess(command, container_exit, "", "")
            if cleanup_timeout:
                raise subprocess.TimeoutExpired(command, 15)
            return subprocess.CompletedProcess(command, 0, "", "")
        args = ["verify_mate_accessibility_container.py", str(artifact), "--output", str(output), "--skip-build"]
        with patch.object(sys, "argv", args), patch.object(mate.subprocess, "run", side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            result = mate.main()
        return result, json.loads(output.read_text()), calls

    def test_speech_startup_failure_retains_its_phase_and_exit_before_atspi(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, calls = self.run_check(Path(temporary), desktop=False,
                orca=False, container_exit=141, phase="speech-dispatcher-start")
            self.assertEqual(result, 1)
            self.assertEqual(report["exit_code"], 141)
            self.assertEqual(report["scenarios"], [])
            self.assertIn("speech-dispatcher-start", report["error"])
            self.assertIn("141", report["error"])
            self.assertEqual(calls[-1][:3], ["docker", "rm", "-f"])

    def test_early_failure_without_phase_retains_exit_and_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, report, _ = self.run_check(Path(temporary), desktop=False,
                orca=False, container_exit=23)
            self.assertEqual(result, 1)
            self.assertIn("23", report["error"])
            self.assertIn("unknown", report["error"])

    def test_minimal_image_explicitly_installs_speech_runtime_and_fallback(self):
        recipe = re.sub(r"\\\n\s*", " ",
                        (ROOT / "packaging/verify-mate-accessibility.Dockerfile").read_text())
        installs = [line[4:].split("&& rm -rf", 1)[0] for line in recipe.splitlines()
                    if line.startswith("RUN ") and "apt-get install" in line]
        self.assertEqual(len(installs), 1)
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            capture = work / "apt.jsonl"
            apt = work / "apt-get"
            apt.write_text(f"#!{sys.executable}\nimport json,os,sys\n"
                "with open(os.environ['APT_CAPTURE'],'a') as f:"
                " f.write(json.dumps(sys.argv[1:])+'\\n')\n")
            apt.chmod(0o755)
            subprocess.run(["sh", "-ec", installs[0]], check=True, capture_output=True,
                timeout=5, env=dict(os.environ, PATH=str(work)+os.pathsep+os.environ["PATH"],
                                    APT_CAPTURE=str(capture)))
            calls = [json.loads(line) for line in capture.read_text().splitlines()]
            installed = {arg for call in calls if call[0] == "install" for arg in call[1:]}
            required = {"orca", "speech-dispatcher", "speech-dispatcher-espeak-ng", "python3-speechd"}
            self.assertEqual(required - installed, set(),
                             "Declare speech prerequisites explicitly with --no-install-recommends")

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
            setup = setup.replace('/reports/', str(reports) + '/')
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


class SpeechBootstrapTests(unittest.TestCase):
    def run_setup(self, directory, *, unavailable=None, executable=True,
                  daemon_exit=0, active_modules="dummy espeak-ng-fallback", connection_failure=False,
                  default_module="dummy"):
        reports = directory / "reports"
        reports.mkdir()
        modules = directory / "modules"
        modules.mkdir()
        for name in ("sd_dummy", "sd_espeak-ng"):
            module = modules / name
            if name != unavailable or not executable:
                module.write_text("#!/bin/sh\nexit 0\n")
                module.chmod(0o644 if name == unavailable else 0o755)
        commands = directory / "commands"
        commands.mkdir()
        calls = directory / "calls.txt"
        daemon = commands / "speech-dispatcher"
        daemon.write_text('''#!/bin/sh
printf 'start\\n' >> "$SPEECH_CALLS"
for module in $SPEECH_ACTIVE_MODULES; do
    printf 'Module %s started successfully\\n' "$module"
done > "$SPEECH_SERVER_LOG"
exit "$SPEECH_EXIT"
''')
        daemon.chmod(0o755)
        (directory / "speechd.py").write_text('''import os
from pathlib import Path
def record(value):
    with Path(os.environ['SPEECH_CALLS']).open('a') as output:
        output.write(value + '\\n')
class SSIPClient:
    def __init__(self, name, *, autospawn):
        assert autospawn is False, 'Do not autospawn a replacement daemon'
        record('connect')
        if os.environ['SPEECH_CONNECT_FAILURE'] == '1':
            raise OSError('Private speech socket unavailable')
    def list_output_modules(self):
        raise AssertionError('Built-in modules are intentionally absent from this listing')
    def get_output_module(self):
        record('default-module')
        return os.environ['SPEECH_DEFAULT_MODULE']
    def close(self):
        record('close')
''')
        setup = mate.SPEECH_SETUP.replace('/usr/lib/speech-dispatcher-modules', str(modules))
        setup = setup.replace('/reports/', str(reports) + '/')
        setup = setup.replace(' /reports/speechd', ' ' + str(reports / 'speechd'))
        result = subprocess.run(["sh", "-ec", setup], capture_output=True, text=True, timeout=5,
            env=dict(os.environ, HOME=str(directory / 'home'), PYTHONPATH=str(directory),
                     PATH=str(commands)+os.pathsep+os.environ["PATH"],
                     SPEECH_CALLS=str(calls), SPEECH_EXIT=str(daemon_exit),
                     SPEECH_ACTIVE_MODULES=active_modules,
                     SPEECH_SERVER_LOG=str(reports / 'speechd/speech-dispatcher.log'),
                     SPEECH_DEFAULT_MODULE=default_module,
                     SPEECH_CONNECT_FAILURE='1' if connection_failure else '0'))
        invoked = calls.read_text().splitlines() if calls.exists() else []
        return result, (reports / 'bootstrap-phase.txt').read_text().strip(), invoked

    def test_each_missing_or_nonexecutable_module_fails_before_daemon_start(self):
        for module in ('sd_dummy', 'sd_espeak-ng'):
            for executable in (True, False):
                with self.subTest(module=module, executable=executable), tempfile.TemporaryDirectory() as temporary:
                    result, phase, calls = self.run_setup(Path(temporary), unavailable=module,
                                                          executable=executable)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(module, result.stderr)
                    self.assertEqual(phase, 'speech-module-preflight')
                    self.assertEqual(calls, [])

    def test_daemon_failure_stops_before_readiness_probe(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phase, calls = self.run_setup(Path(temporary), daemon_exit=141)
            self.assertEqual(result.returncode, 141)
            self.assertEqual(phase, 'speech-dispatcher-start')
            self.assertEqual(calls, ['start'])

    def test_successful_daemon_must_offer_both_initialized_modules(self):
        for modules in ('', 'dummy', 'espeak-ng-fallback'):
            with self.subTest(modules=modules), tempfile.TemporaryDirectory() as temporary:
                result, phase, calls = self.run_setup(Path(temporary), active_modules=modules)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('did not initialize required modules', result.stderr)
                self.assertEqual(phase, 'speech-dispatcher-ready')
                self.assertEqual(calls, ['start'])

    def test_private_connection_requires_dummy_without_autospawn(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phase, calls = self.run_setup(Path(temporary))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(phase, 'speech-dispatcher-ready')
            self.assertEqual(calls, ['start', 'connect', 'default-module', 'close'])
            config = Path(temporary) / 'home/speechd/speechd.conf'
            self.assertIn('DefaultModule "dummy"', config.read_text())

    def test_foreign_default_configuration_is_not_overridden_to_hide_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phase, calls = self.run_setup(Path(temporary), default_module='espeak-ng-fallback')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('not using the private dummy configuration', result.stderr)
            self.assertEqual(phase, 'speech-dispatcher-ready')
            self.assertEqual(calls, ['start', 'connect', 'default-module', 'close'])

    def test_connection_failure_is_not_autospawned_or_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, phase, calls = self.run_setup(Path(temporary), connection_failure=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Private speech socket unavailable', result.stderr)
            self.assertEqual(phase, 'speech-dispatcher-ready')
            self.assertEqual(calls, ['start', 'connect'])


if __name__ == "__main__":
    unittest.main()
