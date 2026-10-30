"""Exercise real test subprocesses, concurrency, selection and failed runs."""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/run_python_tests.py"
spec = importlib.util.spec_from_file_location("parallel_runner", RUNNER)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ParallelPythonRunnerTests(unittest.TestCase):
    def run_suite(self, directory, *arguments):
        return subprocess.run([sys.executable, "-B", str(RUNNER), "--tests", str(directory),
                               *arguments], capture_output=True, text=True, timeout=20)

    def test_workers_overlap_in_separate_processes_without_interleaving_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, other in (("a", "b"), ("b", "a")):
                (root / f"test_{name}.py").write_text(f'''import os, time, unittest
from pathlib import Path
class ParallelTest(unittest.TestCase):
    def test_overlap(self):
        root = Path({str(root)!r})
        (root / {name!r}).write_text(str(os.getpid()))
        deadline = time.monotonic() + 5
        while not (root / {other!r}).exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue((root / {other!r}).exists(), "other worker never started")
        self.assertNotEqual((root / {name!r}).read_text(), (root / {other!r}).read_text())
''')
            result = self.run_suite(root, "--jobs", "2")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("2 worker processes", result.stdout)
            self.assertEqual(result.stdout.count("Ran 1 test"), 2)
            self.assertIn("2 modules, 0 failed", result.stdout)

    def test_failures_and_import_errors_do_not_hide_other_modules(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "test_failed.py").write_text("import unittest\nclass Failed(unittest.TestCase):\n def test_no(self): self.fail('expected assertion')\n")
            (root / "test_import.py").write_text("raise RuntimeError('expected import failure')\n")
            (root / "test_ok.py").write_text("import unittest\nclass Good(unittest.TestCase):\n def test_yes(self): pass\n")
            result = self.run_suite(root, "--jobs", "2")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("expected assertion", result.stdout)
            self.assertIn("expected import failure", result.stdout)
            self.assertIn("PASS: test_ok.py", result.stdout)
            self.assertIn("3 modules, 2 failed", result.stdout)

    def test_patterns_are_deduplicated_and_package_modules_keep_import_context(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "package"
            package.mkdir()
            (package / "__init__.py").write_text("")
            (package / "helper.py").write_text("VALUE = 42\n")
            (package / "test_nested.py").write_text("from .helper import VALUE\nimport unittest\nclass Nested(unittest.TestCase):\n def test_value(self): self.assertEqual(VALUE, 42)\n")
            (root / "test_excluded.py").write_text("raise RuntimeError('must not run')\n")
            result = self.run_suite(root, "--jobs", "1", "--pattern", "test_nested.py",
                                    "--pattern", "test_nested.py")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 modules, 0 failed", result.stdout)
            self.assertEqual(result.stdout.count("Ran 1 test"), 1)

    def test_empty_selection_and_invalid_limits_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for arguments in ((), ("--jobs", "0"), ("--jobs", "-1"), ("--timeout", "0")):
                with self.subTest(arguments=arguments):
                    result = self.run_suite(root, *arguments)
                    self.assertNotEqual(result.returncode, 0)

    def test_timeout_fails_and_remaining_modules_still_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "test_hang.py").write_text("import time\ntime.sleep(60)\n")
            (root / "test_ok.py").write_text("import unittest\nclass Good(unittest.TestCase):\n def test_yes(self): pass\n")
            result = self.run_suite(root, "--jobs", "2", "--timeout", "1")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("Module timed out after 1s", result.stdout)
            self.assertIn("PASS: test_ok.py", result.stdout)

    def test_default_workers_respect_process_cpu_affinity(self):
        with patch.object(runner.os, "sched_getaffinity", return_value={1, 3, 5}):
            self.assertEqual(runner.available_jobs(), 3)
        with patch.object(runner.os, "sched_getaffinity", side_effect=OSError), \
                patch.object(runner.os, "cpu_count", return_value=None):
            self.assertEqual(runner.available_jobs(), 1)

    def test_workflow_uses_parallel_python_and_ctest_entry_points(self):
        import yaml
        ci = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())
        script = "\n".join(step.get("run", "") for step in ci["jobs"]["package_contract"]["steps"])
        self.assertIn("/usr/bin/python3 -B tools/run_python_tests.py", script)
        source = (ROOT / ".github/workflows/verify-source.yml").read_text()
        self.assertIn('ctest --test-dir build/core --parallel "$(nproc)"', source)
        self.assertIn('ctest --preset ${{ matrix.preset }} --parallel "$(nproc)"', source)


if __name__ == "__main__":
    unittest.main()
