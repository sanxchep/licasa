#!/usr/bin/env python3
"""Exercise installer failures with fake packages and loader tools, without a GUI."""

import os
import fcntl
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "packaging/install-licasa-appimage.sh"
BASH = shutil.which("bash")
sys.path.insert(0, str(ROOT / "tools"))
from lifecycle_update import InstallationGate, recover_appimage, start_identity
from process_fixture import start_fixture, stop_fixture, wait_for_message


class AppImageInstallerTests(unittest.TestCase):
    def setUp(self):
        old_umask = os.umask(0o022)
        self.addCleanup(os.umask, old_umask)
        self.temporary = tempfile.TemporaryDirectory(prefix="licasa-installer-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.user_home = self.work / "home with spaces"
        self.tools = self.work / "tools"
        self.tools.mkdir()
        self.environment = dict(os.environ)
        for name in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "BASH_ENV", "ENV", "LD_PRELOAD", "LD_AUDIT"):
            self.environment.pop(name, None)
        self.environment.update(
            HOME=str(self.user_home), PATH=f"{self.tools}:/usr/bin:/bin",
            TMPDIR=str(self.work), CHECKED_FILES=str(self.work / "checked-files"),
            APP_STARTED=str(self.work / "app-started"),
            LD_LIBRARY_PATH="/unrelated/developer/libraries",
        )
        self.write_tool("uname", """case "$1" in
  -s) echo "${TEST_OS:-Linux}" ;;
  -m) echo "${TEST_ARCH:-x86_64}" ;;
  *) exit 1 ;;
esac
""")
        self.write_tool("getconf", """case "$1" in
  LONG_BIT) echo "${TEST_BITS:-64}" ;;
  GNU_LIBC_VERSION) echo "${TEST_LIBC:-glibc 2.39}" ;;
  *) exit 1 ;;
esac
""")
        self.write_tool("ldd", """printf '%s|%s\\n' "$LD_LIBRARY_PATH" "$1" >> "$CHECKED_FILES"
if [[ "$1" == *"${FAILING_FILE:-no-such-file}" ]]; then
  case "${LOADER_FAILURE:-missing}" in
    missing) echo 'libmissing.so.1 => not found'; exit 0 ;;
    version) echo "version GLIBC_2.39 not found" >&2; exit 1 ;;
    version-status-zero) echo "libc.so.6: version GLIBC_2.39 not found" >&2; exit 0 ;;
    status) echo 'loader inspection failed' >&2; exit 2 ;;
  esac
fi
echo 'libc.so.6 => /lib/libc.so.6 (0x1234)'
""")
        self.write_tool("update-desktop-database", 'exit "${CACHE_STATUS:-0}"\n')
        self.appimage = self.write_tool("Licasa fixture.AppImage", """if [[ "$1" != --appimage-extract ]]; then exit 99; fi
if [[ "${EXTRACTION_FAILURE:-0}" == 1 ]]; then
  echo 'fixture extraction failed' >&2
  exit 1
fi
mkdir -p squashfs-root/usr/{bin,lib/licasa/lib,plugins/platforms,share/applications,share/icons/hicolor/512x512/apps}
printf '\\177ELFfixture' > squashfs-root/usr/bin/licasa
printf '\\177ELFfixture' > 'squashfs-root/usr/plugins/platforms/reader with spaces.so'
printf 'not a binary' > squashfs-root/usr/lib/licasa/lib/LICENSE
printf '#!/bin/sh\\necho started > "$APP_STARTED"\\n' > squashfs-root/AppRun
chmod +x squashfs-root/AppRun
printf '[Desktop Entry]\\nMimeType=image/jpeg;\\n' > squashfs-root/usr/share/applications/licasa.desktop
printf 'fixture icon' > squashfs-root/usr/share/icons/hicolor/512x512/apps/licasa.png
""")

    def write_tool(self, name, body):
        path = self.tools / name
        path.write_text(f"#!{BASH}\nset -eu\n{body}")
        path.chmod(0o755)
        return path

    def run_installer(self, uninstall=False, installed_script=False, **environment):
        script = (self.user_home / ".local/opt/licasa-appimage/install-licasa-appimage.sh"
                  if installed_script else INSTALLER)
        completed = subprocess.run(
            [BASH, str(script), "--uninstall" if uninstall else str(self.appimage)],
            env=dict(self.environment, **environment), capture_output=True, text=True,
            timeout=10,
        )
        self.assertFalse((self.work / "app-started").exists(), "Installer launched the app")
        return completed

    def existing_installation(self):
        target = self.user_home / ".local/opt/licasa-appimage"
        target.mkdir(parents=True)
        paths = {
            target / ".licasa-appimage-install": "Installed by packaging/install-licasa-appimage.sh\n",
            target / "AppRun": "existing application",
            self.user_home / ".local/share/applications/licasa-appimage.desktop": "existing launcher",
            self.user_home / ".local/share/icons/hicolor/512x512/apps/licasa-appimage.png": "existing icon",
        }
        for path, contents in paths.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        return paths

    def assert_preserved(self, paths):
        for path, contents in paths.items():
            self.assertEqual(path.read_text(), contents)
        self.assertFalse([path for path in (self.user_home / ".local/opt").glob(".licasa-*")
                          if path.name not in {".licasa-appimage.lock", ".licasa-appimage.licasa-lifecycle"}])

    def fail_rename(self, destination, after=False, signal=False):
        real_mv = shutil.which("mv")
        self.write_tool("mv", f'''if [[ "${{@: -1}}" == {destination} && ! -e "$TMPDIR/failure-fired" ]]; then
  touch "$TMPDIR/failure-fired"
  {f'"{real_mv}" "$@"' if after else ':'}
  {"kill -TERM $PPID" if signal else ':'}
  exit 73
fi
exec "{real_mv}" "$@"
''')

    def test_failed_update_restores_every_file_at_each_replacement_boundary(self):
        paths = self.existing_installation()
        for destination in ("*/opt/licasa-appimage", "*/apps/licasa-appimage.png",
                            "*/applications/licasa-appimage.desktop"):
            for after in (False, True):
                with self.subTest(destination=destination, after=after):
                    (self.work / "failure-fired").unlink(missing_ok=True)
                    self.fail_rename(destination, after=after)
                    result = self.run_installer()
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assert_preserved(paths)
                    self.assertFalse(list(self.user_home.rglob(".licasa-desktop.*")))
                    self.assertFalse(list(self.user_home.rglob(".licasa-icon.*")))
        (self.tools / "mv").unlink()
        result = self.run_installer()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_failed_fresh_install_removes_partial_application_and_integration(self):
        self.fail_rename("*/applications/licasa-appimage.desktop", after=True)
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        for name in (".local/opt/licasa-appimage", ".local/share/applications/licasa-appimage.desktop",
                     ".local/share/icons/hicolor/512x512/apps/licasa-appimage.png"):
            self.assertFalse((self.user_home / name).exists())

    def test_termination_after_icon_replacement_rolls_back(self):
        paths = self.existing_installation()
        self.fail_rename("*/apps/licasa-appimage.png", after=True, signal=True)
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assert_preserved(paths)

    def test_killed_update_recovers_runtime_icon_and_desktop_from_durable_journal(self):
        for boundary in ("*/opt/licasa-appimage", "*/apps/licasa-appimage.png"):
            with self.subTest(boundary=boundary):
                paths = self.existing_installation()
                real_mv = shutil.which("mv")
                self.write_tool("mv", f'''"{real_mv}" "$@"
if [[ "${{@: -1}}" == {boundary} && ! -e "$TMPDIR/kill-fired" ]]; then
  touch "$TMPDIR/kill-fired"
  kill -KILL "$PPID"
fi
''')
                result = self.run_installer()
                self.assertNotEqual(result.returncode, 0)
                target = self.user_home / ".local/opt/licasa-appimage"
                with InstallationGate(target) as gate:
                    self.assertEqual(gate.read()["state"], "replacing")
                    recover_appimage(gate)
                    self.assertEqual(gate.read()["state"], "verified")
                self.assert_preserved(paths)
                (self.tools / "mv").unlink()
                (self.work / "kill-fired").unlink()
                shutil.rmtree(self.user_home)

    def test_concurrent_install_is_rejected_without_mutating_existing_files(self):
        paths = self.existing_installation()
        lock = self.user_home / ".local/opt/.licasa-appimage.lock"
        with lock.open("w") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Another Licasa installation", result.stderr)
        self.assert_preserved(paths)

    def test_failed_rollback_preserves_recovery_files(self):
        paths = self.existing_installation()
        real_mv = shutil.which("mv")
        self.write_tool("mv", f'''if [[ "${{@: -1}}" == */applications/licasa-appimage.desktop ||
    ( "$1" == -fT && "$3" == */integration-1 ) ]]; then
  exit 73
fi
exec "{real_mv}" "$@"
''')
        result = self.run_installer()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Recovery files are preserved", result.stderr)
        recovery = list((self.user_home / ".local/opt").glob(".licasa-appimage.*"))
        recovery = [path for path in recovery if path.is_dir() and path.name != ".licasa-appimage.licasa-lifecycle"]
        self.assertEqual(len(recovery), 1)
        self.assertEqual((recovery[0] / "integration-1").read_text(), "existing icon")
        self.assertEqual((self.user_home / ".local/opt/licasa-appimage/AppRun").read_text(),
                         paths[self.user_home / ".local/opt/licasa-appimage/AppRun"])

    def test_uninstall_uses_recorded_xdg_paths_after_environment_changes(self):
        data = self.work / "custom data 日本語"
        config = self.work / "custom config"
        result = self.run_installer(XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(config))
        self.assertEqual(result.returncode, 0, result.stderr)
        target = self.user_home / ".local/opt/licasa-appimage"
        settings = config / "licasa/settings.ini"
        settings.parent.mkdir(parents=True)
        settings.write_text("user settings")
        photo = self.user_home / "photo.jpg"
        photo.write_bytes(b"photo")
        unrelated = data / "applications/another.desktop"
        unrelated.write_text("another application")
        startup = config / "autostart/licasa-background.desktop"
        startup.parent.mkdir()
        startup.write_text(f'[Desktop Entry]\nExec="{target}/AppRun" --background\nX-Licasa-Background=1\n')
        identity = hashlib.sha256(f"{os.geteuid()}\nappimage\n{target}".encode()).hexdigest()[:32]
        scoped = startup.with_name(f"licasa-background-{identity}.desktop")
        scoped.write_text(f'[Desktop Entry]\nExec=/usr/bin/env "{target}/AppRun" --background\n'
                          f'X-Licasa-Background=1\nX-Licasa-Installation={identity}\n')
        result = self.run_installer(uninstall=True, installed_script=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(target.exists())
        self.assertFalse((data / "applications/licasa-appimage.desktop").exists())
        self.assertFalse((data / "icons/hicolor/512x512/apps/licasa-appimage.png").exists())
        self.assertFalse(startup.exists())
        self.assertFalse(scoped.exists())
        self.assertEqual(settings.read_text(), "user settings")
        self.assertEqual(photo.read_bytes(), b"photo")
        self.assertEqual(unrelated.read_text(), "another application")
        result = self.run_installer(uninstall=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_reinstall_keeps_recorded_integration_location(self):
        data = self.work / "custom data"
        for environment in ({"XDG_DATA_HOME": str(data)}, {}):
            result = self.run_installer(**environment)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((data / "applications/licasa-appimage.desktop").is_file())
        self.assertFalse((self.user_home / ".local/share/applications/licasa-appimage.desktop").exists())

    def test_uninstall_preserves_modified_and_symlinked_integration(self):
        self.assertEqual(self.run_installer().returncode, 0)
        desktop = self.user_home / ".local/share/applications/licasa-appimage.desktop"
        desktop.write_text("user replacement")
        icon = self.user_home / ".local/share/icons/hicolor/512x512/apps/licasa-appimage.png"
        outside = self.work / "another-icon"
        outside.write_text("do not remove")
        icon.unlink()
        icon.symlink_to(outside)
        result = self.run_installer(uninstall=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(desktop.read_text(), "user replacement")
        self.assertTrue(icon.is_symlink())
        self.assertEqual(outside.read_text(), "do not remove")

    def test_uninstall_refuses_unmanaged_and_invalid_path_records(self):
        target = self.user_home / ".local/opt/licasa-appimage"
        target.mkdir(parents=True)
        (target / "precious").write_text("keep")
        result = self.run_installer(uninstall=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((target / "precious").read_text(), "keep")
        shutil.rmtree(target)
        self.assertEqual(self.run_installer().returncode, 0)
        (target / ".licasa-appimage-paths").write_bytes(b"1\0relative/path\0/absolute\0")
        result = self.run_installer(uninstall=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(target.exists())

    def test_failed_uninstall_restores_application_and_integration(self):
        self.assertEqual(self.run_installer().returncode, 0)
        target = self.user_home / ".local/opt/licasa-appimage"
        paths = {path: path.read_bytes() for path in self.user_home.rglob("*")
                 if path.is_file() and ".local/state" not in str(path.relative_to(self.user_home))
                 and ".licasa-lifecycle" not in str(path)}
        self.fail_rename('*/previous-app')
        result = self.run_installer(uninstall=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(target.exists())
        for path, data in paths.items():
            self.assertEqual(path.read_bytes(), data)

    def test_legacy_install_can_be_removed_with_its_original_xdg_directory(self):
        data = self.work / "legacy custom"
        self.assertEqual(self.run_installer(XDG_DATA_HOME=str(data)).returncode, 0)
        target = self.user_home / ".local/opt/licasa-appimage"
        for name in (".licasa-appimage-paths", ".licasa-appimage.desktop", "install-licasa-appimage.sh"):
            (target / name).unlink()
        desktop = data / "applications/licasa-appimage.desktop"
        desktop.write_text(desktop.read_text().replace('Exec=bash "', 'Exec="'))
        result = self.run_installer(uninstall=True, XDG_DATA_HOME=str(data))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(target.exists())
        self.assertFalse((data / "applications/licasa-appimage.desktop").exists())

    def test_standalone_remover_requires_full_quit_for_pre_helper_runtime(self):
        self.assertEqual(self.run_installer().returncode, 0)
        target = self.user_home / ".local/opt/licasa-appimage"
        shutil.rmtree(target / "usr/libexec")
        shutil.rmtree(target.parent / ".licasa-appimage.licasa-lifecycle")
        shutil.copy2("/bin/sleep", target / "usr/bin/licasa")
        download = self.work / "download"
        download.mkdir()
        standalone = download / "install-licasa-appimage.sh"
        shutil.copy2(INSTALLER, standalone)
        photo = self.user_home / "photo.jpg"
        photo.write_bytes(b"precious")
        foreign = self.user_home / ".config/autostart/foreign.desktop"
        foreign.parent.mkdir(parents=True, exist_ok=True)
        foreign.write_text("foreign startup")
        paths = {path: path.read_bytes() for path in self.user_home.rglob("*")
                 if path.is_file() and ".local/state" not in str(path)}
        process = start_fixture(self, [str(target / "usr/bin/licasa"), "60"])
        completed = subprocess.run([BASH, str(standalone), "--uninstall"], env=self.environment,
                                   capture_output=True, text=True, timeout=10)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("fully Quit", completed.stderr)
        self.assertIsNone(process.poll())
        for path, content in paths.items():
            self.assertEqual(path.read_bytes(), content)
        process.terminate()
        process.wait(timeout=5)
        completed = subprocess.run([BASH, str(standalone), "--uninstall"], env=self.environment,
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Legacy removal", completed.stdout)
        self.assertFalse(target.exists())
        self.assertEqual(photo.read_bytes(), b"precious")
        self.assertEqual(foreign.read_text(), "foreign startup")

    def test_standalone_scoped_unknown_process_and_strict_option(self):
        self.assertEqual(self.run_installer().returncode, 0)
        target = self.user_home / ".local/opt/licasa-appimage"
        shutil.rmtree(target / "usr/libexec")
        download = self.work / "download"
        download.mkdir()
        standalone = download / "install-licasa-appimage.sh"
        shutil.copy2(INSTALLER, standalone)
        before = (target / "usr/bin/licasa").read_bytes()
        runtime = self.work / "runtime"
        runtime.mkdir(mode=0o700)
        self.environment["XDG_RUNTIME_DIR"] = str(runtime)
        identity = hashlib.sha256(f"{os.geteuid()}\nappimage\n{target}".encode()).hexdigest()[:32]
        endpoint = runtime / f"licasa-lc-{identity}.sock"
        code = '''import ctypes, os, socket, sys, time
server=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sys.argv[1]); os.chmod(sys.argv[1], 0o600); server.listen(8)
libc = ctypes.CDLL(None)
assert libc.prctl(15, b"unrelated-name", 0, 0, 0) == 0
assert libc.prctl(4, 0, 0, 0, 0) == 0
print("ready", flush=True)
time.sleep(60)
'''
        process = start_fixture(self, [sys.executable, "-c", code, str(endpoint)])
        try:
            wait_for_message(process)
            try:
                os.readlink(f"/proc/{process.pid}/exe")
            except PermissionError:
                pass
            else:
                self.skipTest("This test requires an unprivileged process without ptrace override")
            completed = subprocess.run([BASH, str(standalone), "--uninstall"],
                                       env=dict(self.environment, LICASA_STRICT_PROCESS_INSPECTION="1"),
                                       capture_output=True, text=True, timeout=10)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("Process inspection is unavailable", completed.stderr)
            self.assertEqual((target / "usr/bin/licasa").read_bytes(), before)
            self.assertIsNone(process.poll())
            completed = subprocess.run([BASH, str(standalone), "--uninstall"], env=self.environment,
                                       capture_output=True, text=True, timeout=10)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("fully Quit", completed.stderr)
            self.assertEqual((target / "usr/bin/licasa").read_bytes(), before)
            endpoint.unlink()
            journal = target.parent / ".licasa-appimage.licasa-lifecycle/transaction.json"
            record = json.loads(journal.read_text())
            record["predecessors"] = {str(process.pid): start_identity(process.pid)}
            journal.write_text(json.dumps(record))
            completed = subprocess.run([BASH, str(standalone), "--uninstall"], env=self.environment,
                                       capture_output=True, text=True, timeout=10)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("fully Quit", completed.stderr)
            self.assertEqual((target / "usr/bin/licasa").read_bytes(), before)
            record["predecessors"] = {}
            journal.write_text(json.dumps(record))
            completed = subprocess.run([BASH, str(standalone), "--uninstall"], env=self.environment,
                                       capture_output=True, text=True, timeout=10)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn('"inspectionScope": "installation"', completed.stderr)
            self.assertIn('"globalAbsenceProven": false', completed.stderr)
            self.assertIn('"legacyFirstQuitRequired": true', completed.stderr)
            self.assertFalse(target.exists())
            self.assertIsNone(process.poll())
        finally:
            stop_fixture(process)

    def test_real_desktop_parser_launches_from_special_path_with_exact_photo_argument(self):
        python = "/usr/bin/python3"
        available = subprocess.run([python, "-c", "from gi.repository import Gio"],
                                   capture_output=True).returncode == 0
        if not available:
            self.skipTest("GIO Python bindings are required for desktop activation")
        self.user_home = self.work / 'home "quoted" $literal \\backslash % `literal` 日本語'
        self.environment["HOME"] = str(self.user_home)
        self.assertEqual(self.run_installer().returncode, 0)
        target = self.user_home / ".local/opt/licasa-appimage"
        (target / "AppRun").write_text('#!/bin/bash\nprintf "%s\\0" "$@" > "$APP_STARTED.pending"\n'
                                     'mv -- "$APP_STARTED.pending" "$APP_STARTED"\n')
        photo = self.user_home / 'photo "quoted" % 日本語.png'
        photo.write_bytes(b"fixture")
        desktop = self.user_home / ".local/share/applications/licasa-appimage.desktop"
        result = subprocess.run([python, "-c", """import sys
from gi.repository import Gio
app = Gio.DesktopAppInfo.new_from_filename(sys.argv[1])
assert app is not None, 'Desktop entry could not be parsed'
app.launch([Gio.File.new_for_path(sys.argv[2])], None)
""", str(desktop), str(photo)], env=self.environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        started = self.work / "app-started"
        deadline = time.monotonic() + 5
        while not started.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(started.read_bytes(), os.fsencode(photo) + b"\0")

    def test_unsupported_platform_stops_before_creating_installation(self):
        for environment in ({"TEST_ARCH": "aarch64"}, {"TEST_BITS": "32"},
                            {"TEST_OS": "Darwin"}):
            with self.subTest(environment=environment):
                result = self.run_installer(**environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("64-bit x86 Linux (Intel or AMD)", result.stderr)
                self.assertFalse((self.user_home / ".local/opt/licasa-appimage").exists())

    def test_non_glibc_system_has_explanation(self):
        result = self.run_installer(TEST_LIBC="musl")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("glibc-based Linux desktop", result.stderr)
        self.assertFalse((self.user_home / ".local/opt/licasa-appimage").exists())

    def diagnose(self, artifact=None, **environment):
        command = [BASH, str(INSTALLER), "--diagnose"]
        if artifact:
            command.append(str(artifact))
        result = subprocess.run(command, env=dict(self.environment, **environment),
                                capture_output=True, text=True, timeout=10)
        self.assertFalse((self.work / "app-started").exists())
        return result

    def test_diagnostics_do_not_execute_artifact_create_logs_or_expose_environment_values(self):
        result = self.diagnose(self.appimage, QT_PLUGIN_PATH="/private/photographs/secret",
                               DBUS_SESSION_BUS_ADDRESS="private-session-address")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("application_started=no", result.stdout)
        self.assertIn("artifact_executed=no", result.stdout)
        self.assertIn(hashlib.sha256(self.appimage.read_bytes()).hexdigest(), result.stdout)
        self.assertIn("environment.QT_PLUGIN_PATH=set", result.stdout)
        self.assertNotIn("/private/photographs", result.stdout)
        self.assertNotIn("private-session-address", result.stdout)
        self.assertFalse(self.user_home.exists())

    def test_diagnostics_survive_missing_tools_without_starting_application(self):
        limited_tools = self.work / "diagnostic-tools"
        limited_tools.mkdir()
        for name in ("uname", "getconf"):
            (limited_tools / name).symlink_to(self.tools / name)
        result = self.diagnose(PATH=str(limited_tools))
        self.assertEqual(result.returncode, 1)
        self.assertIn("tool.ldd=missing", result.stdout)
        self.assertIn("application_started=no", result.stdout)
        self.assertFalse(self.user_home.exists())

    def test_diagnostics_flag_execution_block_and_report_unknown_mount_separately(self):
        self.write_tool('findmnt', "echo rw,noexec,nosuid\n")
        result = self.diagnose(self.appimage)
        self.assertEqual(result.returncode, 1)
        self.assertIn('installation_filesystem=noexec', result.stdout)
        self.assertFalse(self.user_home.exists())
        self.write_tool('findmnt', 'exit 1\n')
        result = self.diagnose(self.appimage)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('installation_filesystem=unknown', result.stdout)

    def test_diagnostics_flag_unsupported_kernel(self):
        result = self.diagnose(TEST_OS='Darwin')
        self.assertEqual(result.returncode, 1)
        self.assertIn('finding=This release requires Linux.', result.stdout)

    def test_diagnostics_explain_installed_library_failure_without_photo_or_home_paths(self):
        self.assertEqual(self.run_installer().returncode, 0)
        result = self.diagnose(FAILING_FILE="reader with spaces.so")
        self.assertEqual(result.returncode, 1)
        self.assertIn("installed_libraries=failed", result.stdout)
        self.assertIn("libmissing.so.1", result.stdout)
        self.assertNotIn(str(self.user_home), result.stdout)

    def test_failed_install_retains_private_log_and_rotates_only_owned_regular_files(self):
        result = self.run_installer(EXTRACTION_FAILURE="1")
        self.assertNotEqual(result.returncode, 0)
        log = self.user_home / ".local/state/licasa/installation.log"
        self.assertIn("fixture extraction failed", log.read_text())
        self.assertEqual(log.stat().st_mode & 0o777, 0o600)
        log.write_text("x" * 524289)
        result = self.run_installer(EXTRACTION_FAILURE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(log.with_name("installation.log.1").stat().st_size, 524289)
        self.assertIn("fixture extraction failed", log.read_text())

    def test_logging_refuses_symlinks_and_installation_still_reports_errors(self):
        outside = self.work / "unrelated-log"
        outside.write_text("keep this")
        log = self.user_home / ".local/state/licasa/installation.log"
        log.parent.mkdir(parents=True)
        log.symlink_to(outside)
        result = self.run_installer(EXTRACTION_FAILURE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not retain an installation log", result.stderr)
        self.assertIn("fixture extraction failed", result.stderr)
        self.assertEqual(outside.read_text(), "keep this")

    def test_missing_system_tool_has_explanation(self):
        limited_tools = self.work / "limited-tools"
        limited_tools.mkdir()
        for name in ("uname", "getconf"):
            (limited_tools / name).symlink_to(self.tools / name)
        result = self.run_installer(PATH=str(limited_tools))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("system tool 'ldd' is missing", result.stderr)
        self.assertFalse(self.user_home.exists())

    def test_library_failures_preserve_existing_application_launcher_and_icon(self):
        paths = self.existing_installation()
        for failure in ("missing", "version", "version-status-zero", "status"):
            with self.subTest(failure=failure):
                result = self.run_installer(FAILING_FILE="reader with spaces.so",
                                            LOADER_FAILURE=failure)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Missing or incompatible runtime libraries", result.stderr)
                self.assertIn("before replacing", result.stderr)
                self.assert_preserved(paths)

    def test_extraction_failure_preserves_existing_installation(self):
        paths = self.existing_installation()
        result = self.run_installer(EXTRACTION_FAILURE="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Could not extract the AppImage", result.stderr)
        self.assertIn("SHA-256", result.stderr)
        self.assertIn("fixture extraction failed", result.stderr)
        self.assert_preserved(paths)

    def test_success_checks_nested_plugins_and_reinstalls_without_launch(self):
        for _ in range(2):
            result = self.run_installer()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Runtime library check passed (2 ELF files)", result.stdout)
        checks = (self.work / "checked-files").read_text().splitlines()
        self.assertEqual(len(checks), 4)
        self.assertTrue(any("reader with spaces.so" in line for line in checks))
        for line in checks:
            library_path, _ = line.split("|", 1)
            self.assertNotIn("/unrelated/developer", library_path)
            self.assertIn("squashfs-root/usr/lib/licasa/lib:", library_path)
        launcher = self.user_home / ".local/share/applications/licasa-appimage.desktop"
        self.assertIn(f'Exec=bash "{self.user_home}/.local/opt/licasa-appimage/AppRun" %f',
                      launcher.read_text())

    def test_desktop_cache_failure_reports_success_with_warning(self):
        result = self.run_installer(CACHE_STATUS="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Licasa was installed", result.stderr)
        self.assertIn("Installed Licasa", result.stdout)

    def test_home_path_containing_not_found_is_not_a_missing_library(self):
        self.user_home = self.work / "home not found"
        result = self.run_installer(HOME=str(self.user_home))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_sourcing_checker_does_not_start_installation(self):
        result = subprocess.run(
            [BASH, "-c", '. "$1"; declare -F licasa_check_runtime_dependencies',
             "_", str(INSTALLER)], env=self.environment, capture_output=True, text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.user_home.exists())
        self.assertFalse((self.work / "checked-files").exists())

    def test_missing_scan_paths_and_empty_scans_fail(self):
        empty = self.work / "empty"
        empty.mkdir()
        for path in (empty, self.work / "missing"):
            with self.subTest(path=path):
                result = subprocess.run(
                    [BASH, "-c", '. "$1"; licasa_check_runtime_dependencies /private "$2"',
                     "_", str(INSTALLER), str(path)], env=self.environment,
                    capture_output=True, text=True, timeout=10,
                )
                self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
