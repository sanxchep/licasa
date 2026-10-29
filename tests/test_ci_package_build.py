#!/usr/bin/env python3
"""Keep PR package builds on canonical recipes with isolated, profile-exact caches."""

from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_ci_packages import command_for
from check_package_report import check_report
from qualification_evidence import digest


class CiPackageBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="licasa PR package ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def command(self, target, reuse=False):
        return command_for(target, image="candidate-builder", output=self.root / "packages",
            dependencies=self.root / target, jobs=3, reuse=reuse, source=self.root / "source")

    def test_ubuntu_uses_canonical_recipe_and_private_runtime_locations(self):
        command = self.command("ubuntu-24.04", reuse=True)
        self.assertEqual(command[-3:], ["candidate-builder", "bash", "/source/packaging/verify-fresh-build.sh"])
        self.assertIn("LICASA_REUSE_DEPENDENCIES=true", command)
        self.assertTrue(any(value.endswith(":/source:ro") for value in command))
        self.assertTrue(any(value.endswith(":/work/licasa/build/deps/qt-ffmpeg-runtime-release") for value in command))
        self.assertNotIn("--privileged", command)

    def test_debian_profile_cache_requires_its_receipt_and_never_reuses_ubuntu_paths(self):
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.command("debian-13", reuse=True)
        command = self.command("debian-13")
        script = command[command.index("-ec") + 1]
        self.assertIn("tools/build_debian_deb.py", script)
        self.assertIn("--exclude='./build*'", script)
        self.assertNotIn("cp -a", script)
        self.assertTrue(any("debian-6.8.2/qt-ffmpeg-runtime-release" in value for value in command))
        self.assertIn("--skip-dependencies", self.command("debian-13", reuse=True))

    def test_pr_report_cannot_qualify_a_different_artifact(self):
        package = self.root / "candidate.deb"
        package.write_bytes(b"candidate")
        report = {"package_sha256": "a" * 64}
        with self.assertRaisesRegex(ValueError, "different candidate"):
            check_report(report, {}, {}, package)
        report["package_sha256"] = digest(package)
        with patch("check_package_report.validate_report") as strict:
            check_report(report, {"id": "candidate"}, {}, package)
            strict.assert_called_once_with(report, {"id": "candidate"}, {})

    def test_desktop_test_runner_uses_real_user_credentials_and_preserves_arguments(self):
        probe = '''import os, pathlib, sys
assert os.getuid() == os.geteuid() != 0
assert 0 not in os.getgroups()
assert sys.argv[1] == "argument with spaces"
if sys.argv[2] == "root-parent":
    home = pathlib.Path(os.environ["HOME"])
    assert home == pathlib.Path.cwd()
    assert home.stat().st_uid == os.geteuid()
    assert home.stat().st_mode & 0o777 == 0o700
    assert os.getgroups() == []
    assert pathlib.Path(os.environ["XDG_RUNTIME_DIR"]).stat().st_uid == os.geteuid()
print("credentials verified")
'''
        completed = subprocess.run([sys.executable, str(ROOT / "tests/run_unprivileged.py"),
                                    "-c", probe, "argument with spaces",
                                    "root-parent" if os.geteuid() == 0 else "user-parent"],
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("credentials verified", completed.stdout)

    def test_desktop_test_runner_propagates_failure(self):
        completed = subprocess.run([sys.executable, str(ROOT / "tests/run_unprivileged.py"),
                                    "-c", "raise SystemExit(23)"], timeout=10)
        self.assertEqual(completed.returncode, 23)

    def test_native_desktop_runner_enforces_permissions_and_preserves_environment(self):
        probe = '''import os, pathlib, sys, tempfile
assert os.getuid() == os.geteuid() != 0
assert 0 not in os.getgroups()
assert sys.argv[1:4] == ["argument with spaces", "-platform", "offscreen"]
assert os.environ["QT_QPA_PLATFORM"] == "offscreen"
assert os.environ["LD_LIBRARY_PATH"] == "/test/private runtime/lib"
if sys.argv[4] == "root-parent":
    assert "DBUS_SESSION_BUS_ADDRESS" not in os.environ
with tempfile.TemporaryDirectory() as temporary:
    directory = pathlib.Path(temporary)
    directory.chmod(0o500)
    try:
        try:
            (directory / "must-not-exist").write_text("root would bypass this")
        except PermissionError:
            pass
        else:
            raise AssertionError("permission-denial injection was ineffective")
    finally:
        directory.chmod(0o700)
print("native credentials and denial verified")
'''
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                           LD_LIBRARY_PATH="/test/private runtime/lib",
                           DBUS_SESSION_BUS_ADDRESS="deliberate-test-session")
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tests/run_unprivileged.py"), "--exec",
             "/bin/sh", "-c", 'exec "$@"', "native test", sys.executable, "-c", probe,
             "argument with spaces", "-platform", "offscreen",
             "root-parent" if os.geteuid() == 0 else "user-parent"],
            env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("native credentials and denial verified", completed.stdout)

    def test_native_desktop_runner_preserves_failures_and_signals(self):
        for script, expected in [("exit 23", 23),
                                 ("kill -TERM $$", 128 + signal.SIGTERM
                                  if os.geteuid() == 0 else -signal.SIGTERM)]:
            with self.subTest(script=script):
                completed = subprocess.run(
                    [sys.executable, str(ROOT / "tests/run_unprivileged.py"),
                     "--exec", "/bin/sh", "-c", script], timeout=10)
                self.assertEqual(completed.returncode, expected)

    def test_native_desktop_runner_rejects_missing_command(self):
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tests/run_unprivileged.py"), "--exec"],
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("Expected", completed.stderr)


if __name__ == "__main__":
    unittest.main()
