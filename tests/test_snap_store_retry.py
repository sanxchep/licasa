"""Exercise Store retries without installing or publishing any real Snap."""

from functools import partial
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_snap_ci import build
from retry_snap_store import Attempt, DEFAULT_BUDGET, execute, retry, transient_error


class Clock:
    def __init__(self):
        self.now = 0
        self.pauses = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.pauses.append(seconds)
        self.now += seconds


class SnapRetryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clock = Clock()
        self.report = self.root / "report.json"
        self.budget = self.root / "budget.json"
        self.limits = []

    def run_attempts(self, results, **options):
        attempts = iter(results)

        def run(command, limit):
            self.assertEqual(command, ["snap-command", "file with spaces.snap"])
            self.limits.append(limit)
            result = next(attempts)
            if isinstance(result, BaseException):
                raise result
            duration, result = result if isinstance(result, tuple) else (0, result)
            self.clock.now += duration
            return result

        return retry(["snap-command", "file with spaces.snap"], runner=run,
                     clock=self.clock, sleep=self.clock.sleep, report=self.report,
                     budget_file=self.budget, **options)

    def test_actual_setup_failure_and_transient_http_errors_are_retryable(self):
        for message in ("error: unable to contact snap store",
                        "Post https://api.snapcraft.io: Client.Timeout exceeded",
                        "net/http: timeout awaiting response headers",
                        "cannot communicate with server: timeout exceeded while waiting for response",
                        "TLS handshake timeout", "connection reset by peer",
                        "temporary failure in name resolution", "HTTP Error 503",
                        "status code 429", "502 Bad Gateway"):
            with self.subTest(message=message):
                self.assertTrue(transient_error(message))

    def test_credentials_duplicate_review_and_build_failures_stop(self):
        for message in ("HTTP 401 unauthorized", "HTTP 403 forbidden",
                        "invalid credentials", "not logged in", "permission denied",
                        "A file with this exact same content has already been uploaded",
                        "manual review required", "CMake Error: bad configuration",
                        "ninja: build stopped", "make[1]: *** failed", "unknown failure"):
            with self.subTest(message=message):
                self.assertFalse(transient_error("unable to contact snap store\n" + message)
                                 if message != "unknown failure" else transient_error(message))

    def test_success_does_not_consume_the_retry_budget(self):
        self.assertEqual(self.run_attempts([(42, Attempt(0, "built"))]), 0)
        self.assertEqual(self.limits, [DEFAULT_BUDGET])
        self.assertEqual(json.loads(self.budget.read_text())["remaining_seconds"], 1800)
        self.assertEqual(self.clock.pauses, [])

    def test_transient_failure_then_success_preserves_exit_and_diagnostics(self):
        self.assertEqual(self.run_attempts([Attempt(1, "unable to contact snap store"),
                                            (4, Attempt(0, "uploaded"))]), 0)
        report = json.loads(self.report.read_text())
        self.assertEqual([x["exit_code"] for x in report["attempts"]], [1, 0])
        self.assertEqual(report["remaining_seconds"], 1766)
        self.assertEqual(self.limits, [1800, 1770])

    def test_persistent_outage_stops_at_thirty_minutes_including_retry_work(self):
        failures = [Attempt(1, "unable to contact snap store")] * 100
        self.assertEqual(self.run_attempts(failures), 124)
        self.assertEqual(self.clock.now, 1800)
        self.assertEqual(len(self.limits), 60)
        self.assertEqual(json.loads(self.budget.read_text())["remaining_seconds"], 0)

    def test_sleep_is_clipped_and_commands_share_the_remaining_window(self):
        self.assertEqual(self.run_attempts([Attempt(1, "HTTP 503"),
                                            (4, Attempt(0, "ok"))], seconds=50), 0)
        self.clock.now += 100  # Useful work between commands is not retry time.
        self.limits.clear()
        self.assertEqual(self.run_attempts([Attempt(1, "HTTP 503")], seconds=50), 124)
        self.assertEqual(self.clock.pauses, [30, 16])
        self.assertEqual(self.limits, [50])

    def test_build_keeps_its_normal_deadline_but_retries_get_only_remaining_time(self):
        self.assertEqual(self.run_attempts([(5000, Attempt(1, "HTTP 503")),
                                            Attempt(0, "built")], initial_timeout=10800), 0)
        self.assertEqual(self.limits, [10800, 1770])

    def test_acknowledged_duplicate_is_not_uploaded_again(self):
        self.assertEqual(self.run_attempts([Attempt(1, "HTTP 503"),
            Attempt(7, "A file with this exact same content has already been uploaded")]), 7)
        self.assertEqual(len(self.limits), 2)
        self.assertIn("non-transient", json.loads(self.report.read_text())["reason"])

    def test_unknown_failure_and_a_hung_command_are_not_blindly_retried(self):
        for failure in (Attempt(9, "bad recipe"), Attempt(124, "HTTP 503", True)):
            with self.subTest(failure=failure):
                self.assertEqual(self.run_attempts([failure]), failure.code)
                self.assertEqual(self.clock.pauses, [])

    def test_retry_deadline_is_passed_to_and_enforced_by_the_command_runner(self):
        self.assertEqual(self.run_attempts([Attempt(1, "HTTP 503"),
                                            (20, Attempt(124, "", True))], seconds=50), 124)
        self.assertEqual(self.limits, [50, 20])
        self.assertEqual(self.clock.now, 50)

    def test_late_success_cannot_turn_an_exhausted_deadline_into_a_pass(self):
        self.assertEqual(self.run_attempts([Attempt(1, "HTTP 503"),
                                            (21, Attempt(0, "too late"))], seconds=50), 124)

    def test_cancellation_preserves_the_consumed_budget_and_failure_report(self):
        with self.assertRaises(KeyboardInterrupt):
            self.run_attempts([Attempt(1, "HTTP 503"), KeyboardInterrupt()])
        report = json.loads(self.report.read_text())
        self.assertEqual(report["exit_code"], 130)
        self.assertEqual(report["reason"], "interrupted")
        self.assertEqual(report["remaining_seconds"], 1770)

    def test_corrupt_or_incompatible_budget_cannot_silently_restart_retries(self):
        for remaining in (-1, 1801, True, "1800", float("nan")):
            self.budget.write_text(json.dumps({"schema_version": 1,
                "budget_seconds": 1800, "remaining_seconds": remaining}))
            with self.subTest(remaining=remaining), self.assertRaises(ValueError):
                self.run_attempts([])
        self.budget.write_text('{"schema_version": 2, "budget_seconds": 1800, "remaining_seconds": 1}')
        with self.assertRaises(ValueError):
            self.run_attempts([])


class SnapRetryProcessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def cli(self, *command):
        return [sys.executable, str(ROOT / "tools/retry_snap_store.py"),
                "--seconds", "2", "--delay", "0.05", "--report",
                str(self.root / "report.json"), "--", *command]

    def assert_not_running(self, pid):
        status = Path(f"/proc/{pid}/stat")
        # A reparented zombie is waiting for init, and cannot continue working.
        self.assertTrue(not status.exists() or status.read_text().split()[2] == "Z")

    def test_real_cli_retries_then_succeeds_without_losing_argument_boundaries(self):
        state = self.root / "attempts"
        script = """import pathlib,sys
p=pathlib.Path(sys.argv[1]); count=int(p.read_text()) if p.exists() else 0
p.write_text(str(count+1)); assert sys.argv[2] == 'file with spaces.snap'
print('unable to contact snap store' if count == 0 else 'success')
sys.exit(1 if count == 0 else 0)
"""
        result = subprocess.run(self.cli(sys.executable, "-c", script, str(state),
                               "file with spaces.snap"), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(state.read_text(), "2")

    def test_real_cli_exhaustion_is_a_failure_with_a_report(self):
        result = subprocess.run(self.cli(sys.executable, "-c",
            "import sys; print('unable to contact snap store'); sys.exit(1)"),
            capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 124)
        self.assertGreater(len(json.loads((self.root / "report.json").read_text())["attempts"]), 1)

    def test_deadline_kills_a_descendant_even_after_its_parent_has_exited(self):
        pid_file = self.root / "child"
        script = """import pathlib,subprocess,sys
p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(100)'])
pathlib.Path(sys.argv[1]).write_text(str(p.pid))
"""
        result = subprocess.run(self.cli(sys.executable, "-c", script, str(pid_file)),
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 124)
        self.assert_not_running(int(pid_file.read_text()))

    def test_sigterm_stops_the_command_and_records_cancellation(self):
        pid_file = self.root / "child"
        process = subprocess.Popen(self.cli(sys.executable, "-c",
            "import os,pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(100)",
            str(pid_file)), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 3
            while not pid_file.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(pid_file.exists())
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            self.assertEqual(process.returncode, 130)
            self.assert_not_running(int(pid_file.read_text()))
            self.assertEqual(json.loads((self.root / "report.json").read_text())["reason"], "interrupted")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()


class SnapBuildTests(unittest.TestCase):
    def test_local_machine_is_rejected_before_any_setup(self):
        with patch.dict(os.environ, {"RUNNER_ENVIRONMENT": "self-hosted"}), \
                self.assertRaisesRegex(ValueError, "disposable"):
            build(ROOT)

    def test_existing_tools_are_refreshed_and_permanent_setup_failure_stops_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            calls = []

            def store(command, **options):
                calls.append(command)
                return 13 if command[-1] == "snapcraft" else 0

            with patch.dict(os.environ, {"RUNNER_ENVIRONMENT": "github-hosted",
                    "RUNNER_TEMP": str(root)}), self.assertRaises(subprocess.CalledProcessError) as failure:
                build(root, run=lambda *a, **k: None, retry_command=store, executable=lambda _: "/snap/bin/tool")
            self.assertEqual(failure.exception.returncode, 13)
            self.assertEqual(calls, [["sudo", "snap", "refresh", "lxd"],
                ["sudo", "snap", "refresh", "--channel=stable", "--classic", "snapcraft"]])
            self.assertFalse((root / "output").exists())

    def test_hosted_setup_build_output_and_retries_execute_with_fake_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "bin"
            binary.mkdir()
            log = root / "commands.jsonl"
            sudo = binary / "sudo"
            sudo.write_text("#!" + sys.executable + "\n" + '''import json,os,pathlib,sys
root=pathlib.Path(os.environ['FAKE_ROOT']); args=sys.argv[1:]
with (root/'commands.jsonl').open('a') as stream:
    stream.write(json.dumps({'args':args,'cwd':os.getcwd(),'image':os.environ.get('SNAPCRAFT_IMAGE_INFO')})+'\\n')
if args[:3] == ['snap','install','lxd'] and not (root/'failed').exists():
    (root/'failed').touch(); print('error: unable to contact snap store'); sys.exit(1)
if args[-1:] == ['snapcraft'] and args[0] == '-u':
    (root/'licasa_test_amd64.snap').write_bytes(b'fake snap')
''')
            sudo.chmod(0o755)
            env = {"PATH": str(binary) + ":" + os.environ["PATH"], "FAKE_ROOT": str(root),
                   "RUNNER_ENVIRONMENT": "github-hosted", "RUNNER_TEMP": str(root / "temp"),
                   "GITHUB_REPOSITORY": "example/licasa", "GITHUB_RUN_ID": "123",
                   "GITHUB_OUTPUT": str(root / "output")}
            clock = Clock()
            bounded = partial(retry, clock=clock, sleep=clock.sleep)
            with patch.dict(os.environ, env):
                build(root, retry_command=bounded, executable=lambda _: None)
            commands = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertTrue(all(item["cwd"] == str(root) for item in commands))
            self.assertEqual(sum(item["args"] == ["snap", "install", "lxd"] for item in commands), 2)
            self.assertEqual(clock.pauses, [30])
            self.assertEqual((root / "output").read_text(), f"snap={root}/licasa_test_amd64.snap\n")
            reports = root / "build/snap-store-retries"
            self.assertEqual(len(list(reports.glob("*.json"))), 3)
            self.assertEqual(json.loads((reports / "build.json").read_text())["remaining_seconds"], 1770)
            self.assertIn("example/licasa/actions/runs/123", commands[-1]["image"])


class SnapWorkflowTests(unittest.TestCase):
    def test_every_store_operation_uses_one_job_budget_and_retains_diagnostics(self):
        operations = 0
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            workflow = yaml.safe_load(path.read_text())
            for name, job in workflow["jobs"].items():
                scripts = "\n".join(step.get("run", "") for step in job.get("steps", []))
                store_lines = [line for line in scripts.splitlines() if any(text in line for text in
                               ("sudo snap install", "sudo snap refresh", "snapcraft upload", "snapcraft release"))]
                for line in store_lines:
                    with self.subTest(workflow=path.name, job=name, line=line):
                        self.assertIn("--report build/snap-store-retries/", line)
                        self.assertIn(" -- ", line)
                operations += len(store_lines)
                if store_lines or "tools/build_snap_ci.py" in scripts:
                    self.assertIn('--budget-file "$RUNNER_TEMP/snap-store-retry-budget.json"', scripts)
                    self.assertGreaterEqual(job["timeout-minutes"], 45)
                    self.assertTrue(any(step.get("if") == "always()" and
                        step.get("with", {}).get("path") == "build/snap-store-retries/"
                        for step in job["steps"]))
                self.assertNotIn("snapcore/action-build", str(job))
        self.assertGreaterEqual(operations, 15)


if __name__ == "__main__":
    unittest.main()
