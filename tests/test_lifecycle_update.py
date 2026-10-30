#!/usr/bin/env python3
"""Exercise installation refusal, authenticated control and interrupted recovery."""

import json
import os
import random
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import lifecycle_update as lifecycle
from deploy_local import desktop_exec_argument
from process_fixture import FixtureError, start_fixture, stop_fixture, wait_for_message


RECEIPT_SERVER = """
import json, os, pathlib, socket, sys
sys.path.insert(0, sys.argv[1])
import lifecycle_update as lc
endpoint, identity, mode, gated = sys.argv[2:]
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
    os.umask(0o022)
    server.bind(endpoint)
    if gated == 'yes':
        print('bound', flush=True)
        assert sys.stdin.buffer.read(1) == b'x'
    os.chmod(endpoint, 0o600)
    server.listen(1)
    print('ready', flush=True)
    connection, _ = server.accept()
    with connection:
        request = json.loads(connection.recv(16384))
        result = dict(protocol=1, installationId=identity, result='ready', pid=os.getpid(),
                      startIdentity=lc.start_identity(os.getpid()), generation=lc.generation(pathlib.Path('/proc/self/exe')))
        if mode == 'generation': result['generation'] = '0' * 64
        if mode == 'protocol': result['protocol'] = 77
        if mode == 'non-object': result = None
        connection.sendall(json.dumps(result).encode() + b'\\n')
"""


class LifecycleUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="licasa-lifecycle-test-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.root = self.work / "installed app"
        self.binary = self.root / "bin/licasa"
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b"old viewer")

    def test_identity_keeps_lineage_separate_from_generation_and_other_formats(self):
        first = lifecycle.installation_id(self.binary, self.root, "filesystem")
        self.binary.write_bytes(b"new viewer")
        self.assertEqual(first, lifecycle.installation_id(self.binary, self.root, "filesystem"))
        self.assertNotEqual(first, lifecycle.installation_id(self.binary, self.root, "appimage"))
        self.assertNotEqual(first, lifecycle.installation_id(self.work / "other/bin/licasa", self.root, "filesystem"))

    def test_gate_excludes_an_independent_installer(self):
        with lifecycle.InstallationGate(self.root) as gate:
            gate.write("prepared")
            with self.assertRaisesRegex(lifecycle.LifecycleError, "Another launch"):
                with lifecycle.InstallationGate(self.root):
                    self.fail("A second exclusive installer acquired the gate")

    def test_gate_refuses_symlinks_and_shared_permissions(self):
        location = self.work / f".{self.root.name}.licasa-lifecycle"
        target = self.work / "foreign"
        target.mkdir()
        location.symlink_to(target, target_is_directory=True)
        with self.assertRaises(lifecycle.LifecycleError):
            with lifecycle.InstallationGate(self.root):
                pass
        location.unlink()
        location.mkdir(mode=0o755)
        with self.assertRaises(lifecycle.LifecycleError):
            with lifecycle.InstallationGate(self.root):
                pass

    def test_failed_gate_entry_closes_new_and_inherited_descriptors(self):
        gate = lifecycle.InstallationGate(self.root)
        gate.directory.mkdir(mode=0o700)
        lock = gate.directory / "lock"
        lock.write_bytes(b"")
        lock.chmod(0o644)
        with mock.patch.object(lifecycle.os, "close", wraps=os.close) as close:
            with self.assertRaisesRegex(lifecycle.LifecycleError, "Unsafe lifecycle file"):
                gate.__enter__()
            close.assert_called_once()
        with self.assertRaises(OSError):
            os.fstat(close.call_args.args[0])
        self.assertIsNone(gate.fd)

        lock.chmod(0o600)
        with self.binary.open("rb") as unrelated:
            gate = lifecycle.InstallationGate(self.root, inherited_fd=unrelated.fileno())
            with mock.patch.object(lifecycle.os, "close", wraps=os.close) as close:
                with self.assertRaisesRegex(lifecycle.LifecycleError, "not the installation lock"):
                    gate.__enter__()
                close.assert_called_once()
            with self.assertRaises(OSError):
                os.fstat(close.call_args.args[0])
            self.assertIsNone(gate.fd)
            self.assertTrue(unrelated.readable(), "The caller retains its original descriptor")

    def test_updater_and_launcher_reject_the_same_invalid_journal(self):
        gate = lifecycle.InstallationGate(self.root)
        gate.directory.mkdir(mode=0o700)
        lock = gate.directory / "lock"
        lock.write_bytes(b"")
        lock.chmod(0o600)
        cases = [b"\xff", "null", "[]", '"text"', "{", json.dumps({"root": str(self.root), "state": []}),
                 json.dumps({"root": str(self.root), "state": "unknown"}),
                 json.dumps({"root": str(self.work), "state": "verified"})]
        for contents in cases:
            with self.subTest(contents=contents):
                gate.journal.write_bytes(contents if isinstance(contents, bytes) else contents.encode())
                gate.journal.chmod(0o600)
                with self.assertRaises(lifecycle.LifecycleError):
                    gate.read()
                with mock.patch.object(lifecycle.os, "execv") as execute:
                    with self.assertRaises(lifecycle.LifecycleError):
                        lifecycle.launch(self.root, self.binary, "filesystem", None, [])
                    execute.assert_not_called()

    def test_gate_refuses_writable_ancestor_and_accepts_sticky_tmp(self):
        outer = Path(tempfile.mkdtemp(prefix="licasa-outer-ancestry-test-"))
        self.addCleanup(shutil.rmtree, outer)
        outer.chmod(0o755)
        parent = outer / "shared"
        parent.mkdir(mode=0o775)
        parent.chmod(0o775)
        inner = parent / "private"
        inner.mkdir(mode=0o700)
        with self.assertRaisesRegex(lifecycle.LifecycleError, "Writable lifecycle ancestor"):
            with lifecycle.InstallationGate(inner / "install"):
                pass
        safe_inner = self.work / "shared-inside-private"
        safe_inner.mkdir(mode=0o775)
        safe_inner.chmod(0o775)
        with lifecycle.InstallationGate(safe_inner / "install") as gate:
            gate.write("verified")
        linked = self.work / "linked-parent"
        linked.symlink_to(safe_inner, target_is_directory=True)
        with self.assertRaisesRegex(lifecycle.LifecycleError, "Symlinked lifecycle ancestry"):
            with lifecycle.InstallationGate(linked / "other"):
                pass

    def test_legacy_running_process_is_never_sent_quit_or_replaced(self):
        shutil.copy2("/bin/sleep", self.binary)
        process = start_fixture(self, [str(self.binary), "60"])
        old_hash = lifecycle.generation(self.binary)
        with lifecycle.InstallationGate(self.root) as gate:
            with mock.patch.object(lifecycle, "control", return_value={"result": "absent"}):
                with self.assertRaisesRegex(lifecycle.LifecycleError, "one-time full Quit"):
                    gate.prepare(self.binary, "filesystem", timeout=0.1)
            self.assertEqual(gate.read()["state"], "verified")
        self.assertIsNone(process.poll())
        self.assertEqual(old_hash, lifecycle.generation(self.binary))
        process.terminate()
        process.wait(timeout=5)

    def test_deleted_executable_is_still_discovered(self):
        shutil.copy2("/bin/sleep", self.binary)
        process = start_fixture(self, [str(self.binary), "60"])
        self.binary.unlink()
        self.assertIn(process.pid, lifecycle.owned_processes(self.binary))
        process.terminate()
        process.wait(timeout=5)

    def test_scoped_unknown_process_reports_uncertainty_and_known_receipts_still_refuse(self):
        # These are real kernel credentials and an inaccessible exe, rather
        # than a mocked name heuristic. No identity exclusion is inferred from
        # its mutable name: scoped policy retains an explicit unknown count.
        code = '''import ctypes, time
libc = ctypes.CDLL(None)
assert libc.prctl(15, b"unrelated-name", 0, 0, 0) == 0
assert libc.prctl(4, 0, 0, 0, 0) == 0
print("ready", flush=True)
time.sleep(60)
'''
        process = start_fixture(self, [sys.executable, "-c", code])
        try:
            wait_for_message(process)
            try:
                os.readlink(f"/proc/{process.pid}/exe")
            except PermissionError:
                pass
            else:
                self.skipTest("This test requires an unprivileged process without ptrace override")
            inspection = lifecycle.owned_processes(self.binary)
            self.assertNotIn(process.pid, inspection)
            self.assertGreaterEqual(inspection.metadata()["unknownProcessCount"], 1)
            self.assertFalse(inspection.metadata()["globalAbsenceProven"])
            with lifecycle.InstallationGate(self.root) as gate:
                with mock.patch.object(lifecycle, "control", return_value={"result": "absent"}) as probe:
                    result = gate.prepare(self.binary, "filesystem")
                probe.assert_called_once()
                self.assertEqual(probe.call_args.args[3], "prepare-update")
                self.assertEqual(result["inspectionScope"], "installation")
                self.assertGreaterEqual(result["unknownProcessCount"], 1)
                self.assertFalse(result["globalAbsenceProven"])
                self.assertTrue(result["legacyFirstQuitRequired"])
                self.assertEqual(gate.read()["unknownProcessCount"], result["unknownProcessCount"])
                gate.write("verified", predecessors={str(process.pid): lifecycle.start_identity(process.pid)})
                with self.assertRaisesRegex(lifecycle.LifecycleError, "Process inspection is unavailable"):
                    gate.prepare(self.binary, "filesystem")
            with self.assertRaisesRegex(lifecycle.LifecycleError, "Process inspection is unavailable"):
                lifecycle.owned_processes(self.binary, strict=True)
            self.assertEqual(self.binary.read_bytes(), b"old viewer")
        finally:
            stop_fixture(process)

    def test_registered_nondumpable_peer_refuses_even_without_a_readable_target_process(self):
        runtime = self.work / "runtime"
        runtime.mkdir(mode=0o700)
        identity = lifecycle.installation_id(self.binary, self.root, "filesystem")
        endpoint = runtime / f"licasa-lc-{identity}.sock"
        code = '''import ctypes, os, socket, sys, time
server=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(sys.argv[1]); os.chmod(sys.argv[1], 0o600); server.listen(1)
libc=ctypes.CDLL(None)
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
            with mock.patch.dict(os.environ, XDG_RUNTIME_DIR=str(runtime)):
                with lifecycle.InstallationGate(self.root) as gate:
                    with self.assertRaisesRegex(lifecycle.LifecycleError, "Cannot verify"):
                        gate.prepare(self.binary, "filesystem")
                    self.assertEqual(gate.read()["state"], "verified")
                    self.assertGreaterEqual(gate.read()["unknownProcessCount"], 1)
            self.assertEqual(self.binary.read_bytes(), b"old viewer")
        finally:
            stop_fixture(process)

    def test_historical_renamed_appimage_runtime_remains_a_blocker(self):
        root = self.work / "licasa-appimage"
        binary = root / "usr/bin/licasa"
        binary.parent.mkdir(parents=True)
        shutil.copy2("/bin/sleep", binary)
        process = start_fixture(self, [str(binary), "60"], env=dict(os.environ, APPDIR=str(root)))
        backup = self.work / ".licasa-appimage.historical/previous-app"
        backup.parent.mkdir()
        os.replace(root, backup)
        self.assertIn(process.pid, lifecycle.owned_processes(binary))
        shutil.rmtree(backup)
        self.assertIn(process.pid, lifecycle.owned_processes(binary))
        process.terminate()
        process.wait(timeout=5)

    def test_busy_refusal_clears_only_gate_and_preserves_bytes(self):
        with lifecycle.InstallationGate(self.root) as gate:
            with mock.patch.object(lifecycle, "owned_processes", return_value={123: "boot:1"}), \
                    mock.patch.object(lifecycle, "control", return_value={"result": "busy", "busyReason": "visible window"}):
                with self.assertRaisesRegex(lifecycle.LifecycleError, "visible window"):
                    gate.prepare(self.binary, "filesystem", timeout=0.1)
            self.assertEqual(gate.read()["state"], "verified")
        self.assertEqual(self.binary.read_bytes(), b"old viewer")

    def test_accepted_stop_is_not_an_exit_receipt(self):
        with lifecycle.InstallationGate(self.root) as gate:
            with mock.patch.object(lifecycle, "owned_processes", return_value={123: "boot:1"}), \
                    mock.patch.object(lifecycle, "control", return_value={"result": "accepted"}):
                with self.assertRaisesRegex(lifecycle.LifecycleError, "has not exited"):
                    gate.prepare(self.binary, "filesystem", timeout=0.01)
            self.assertEqual(gate.read()["state"], "verified")

    def receipt_peer(self, mode="valid", gated=False):
        runtime = self.work / "runtime"
        runtime.mkdir(mode=0o700, exist_ok=True)
        binary = Path(os.path.realpath(sys.executable))
        identity = lifecycle.installation_id(binary, self.root, "filesystem")
        endpoint = runtime / f"licasa-lc-{identity}.sock"
        endpoint.unlink(missing_ok=True)
        process = start_fixture(self, [sys.executable, "-c", RECEIPT_SERVER, str(ROOT / "tools"),
                                       str(endpoint), identity, mode, "yes" if gated else "no"])
        return process, runtime, binary, endpoint

    def test_authenticated_socket_receipt_rejects_generation_and_protocol_mismatch(self):
        # The peer is a real separate process. Kernel credentials and /proc
        # identity are checked independently of its supplied JSON fields.
        for mode in ("valid", "generation", "protocol", "non-object"):
            with self.subTest(mode=mode):
                process, runtime, binary, _ = self.receipt_peer(mode)
                wait_for_message(process)
                if mode == "valid":
                    self.assertEqual(lifecycle.control(binary, self.root, "filesystem", "status", runtime=runtime)["result"], "ready")
                else:
                    with self.assertRaisesRegex(lifecycle.LifecycleError, "receipt|reply"):
                        lifecycle.control(binary, self.root, "filesystem", "status", runtime=runtime)
                self.assertEqual(process.wait(timeout=5), 0)

    def test_socket_path_existence_does_not_signal_ready(self):
        process, runtime, binary, endpoint = self.receipt_peer(gated=True)
        # A pipe gate forces bind-before-chmod/listen, independent of CPU timing.
        wait_for_message(process, b"bound\n")
        self.assertTrue(endpoint.exists())
        self.assertEqual(endpoint.stat().st_mode & 0o777, 0o755)
        with self.assertRaisesRegex(lifecycle.LifecycleError, "Unsafe lifecycle endpoint"):
            lifecycle.control(binary, self.root, "filesystem", "status", runtime=runtime)
        with self.assertRaisesRegex(FixtureError, "Timed out"):
            wait_for_message(process, timeout=0.05)
        process.stdin.write(b"x")
        wait_for_message(process)
        self.assertEqual(endpoint.stat().st_mode & 0o777, 0o600)
        self.assertEqual(lifecycle.control(binary, self.root, "filesystem", "status", runtime=runtime)["result"], "ready")
        self.assertEqual(process.wait(timeout=5), 0)

    def test_identity_requires_a_json_object_receipt(self):
        for contents in ("null", "[]", '"text"', "{"):
            with self.subTest(contents=contents):
                completed = subprocess.CompletedProcess([], 0, stdout=contents)
                with mock.patch.object(lifecycle.subprocess, "run", return_value=completed):
                    with self.assertRaisesRegex(lifecycle.LifecycleError, "headless identity receipt"):
                        lifecycle.verify_identity(self.binary, self.root, "filesystem")

    def test_invalid_owned_file_manifest_preserves_runtime_and_unrelated_files(self):
        staged = self.work / "stage"
        (staged / "bin").mkdir(parents=True)
        (staged / "bin/licasa").write_bytes(b"candidate")
        unrelated = self.root / "b"
        unrelated.write_bytes(b"photo")
        manifest = self.root / ".licasa-local-files.json"
        for contents in (b"\xff", "null", "[]", '{"files": "bin"}', '{"files": [null]}'):
            with self.subTest(contents=contents):
                manifest.write_bytes(contents if isinstance(contents, bytes) else contents.encode())
                with lifecycle.InstallationGate(self.root) as gate:
                    with self.assertRaisesRegex(lifecycle.LifecycleError, "Invalid Local file manifest"):
                        lifecycle.install_overlay(gate, staged, self.binary)
                self.assertEqual(self.binary.read_bytes(), b"old viewer")
                self.assertEqual(unrelated.read_bytes(), b"photo")

    def test_appimage_crash_recovery_restores_runtime_and_integration_together(self):
        app_root = self.work / "licasa-appimage"
        app_root.mkdir()
        (app_root / "viewer").write_text("old runtime")
        old_info = app_root.stat()
        desktop = self.work / "licasa.desktop"
        desktop.write_text("old desktop")
        recovery = Path(tempfile.mkdtemp(prefix=".licasa-appimage.", dir=self.work))
        shutil.copy2(desktop, recovery / "integration-0")
        with lifecycle.InstallationGate(app_root) as gate:
            gate.write("replacing", recoveryRoot=str(recovery), hadRoot=True,
                       oldRootIdentity=[old_info.st_dev, old_info.st_ino],
                       integrations=[{"path": str(desktop), "existed": True,
                                      "fingerprint": lifecycle.fingerprint(desktop)}])
            os.replace(app_root, recovery / "previous-app")
            app_root.mkdir()
            (app_root / "viewer").write_text("incomplete candidate")
            desktop.write_text("candidate desktop")
            lifecycle.recover_appimage(gate)
            self.assertEqual((app_root / "viewer").read_text(), "old runtime")
            self.assertEqual(desktop.read_text(), "old desktop")
            self.assertEqual(gate.read()["state"], "verified")
        self.assertFalse(recovery.exists())

    def test_completed_transaction_cleanup_after_crash_does_not_accumulate_backups(self):
        for prefix, recover in ((".licasa-appimage.", lifecycle.recover_appimage),
                                (".licasa-local.", lifecycle.recover_overlay)):
            with lifecycle.InstallationGate(self.root) as gate:
                for _ in range(3):
                    recovery = Path(tempfile.mkdtemp(prefix=prefix, dir=self.work))
                    (recovery / "obsolete-runtime").write_bytes(b"old")
                    gate.write("verified", recoveryRoot=str(recovery))
                    recover(gate)
                    self.assertFalse(recovery.exists())
                    self.assertEqual(gate.read()["recoveryRoot"], "")
                    self.assertEqual(self.binary.read_bytes(), b"old viewer")

    def test_overlay_failed_health_restores_owned_files_and_keeps_foreign_files(self):
        unrelated = self.root / "foreign-photo.jpg"
        unrelated.write_bytes(b"photo stays byte-identical")
        staged = self.work / "stage"
        (staged / "bin").mkdir(parents=True)
        (staged / "bin/licasa").write_bytes(b"candidate")
        (staged / "share").mkdir()
        (staged / "share/new-file").write_bytes(b"new")
        with lifecycle.InstallationGate(self.root) as gate:
            with mock.patch.object(lifecycle, "owned_processes", return_value={}), \
                    mock.patch.object(lifecycle, "verify_identity", side_effect=lifecycle.LifecycleError("bad health")):
                with self.assertRaisesRegex(lifecycle.LifecycleError, "bad health"):
                    lifecycle.install_overlay(gate, staged, self.binary)
            self.assertEqual(gate.read()["state"], "verified")
        self.assertEqual(self.binary.read_bytes(), b"old viewer")
        self.assertEqual(unrelated.read_bytes(), b"photo stays byte-identical")
        self.assertFalse((self.root / "share/new-file").exists())

    def test_overlay_success_and_repeat_remove_only_manifest_owned_obsolete_files(self):
        unrelated = self.root / "foreign-photo.jpg"
        unrelated.write_bytes(b"photo")
        for iteration in range(3):
            staged = self.work / f"stage-{iteration}"
            (staged / "bin").mkdir(parents=True)
            (staged / "bin/licasa").write_bytes(f"candidate-{iteration}".encode())
            (staged / f"owned-{iteration}").write_text("owned")
            with lifecycle.InstallationGate(self.root) as gate:
                with mock.patch.object(lifecycle, "owned_processes", return_value={}), \
                        mock.patch.object(lifecycle, "verify_identity", return_value={}):
                    lifecycle.install_overlay(gate, staged, self.binary)
                self.assertEqual(gate.read()["generation"], lifecycle.generation(self.binary))
            self.assertEqual(self.binary.read_bytes(), f"candidate-{iteration}".encode())
            self.assertEqual(unrelated.read_bytes(), b"photo")
            if iteration:
                self.assertFalse((self.root / f"owned-{iteration - 1}").exists())

    def test_overlay_refuses_symlink_parent_without_touching_target(self):
        foreign = self.work / "foreign"
        foreign.mkdir()
        (foreign / "photo").write_bytes(b"precious")
        (self.root / "share").symlink_to(foreign, target_is_directory=True)
        staged = self.work / "stage"
        (staged / "share").mkdir(parents=True)
        (staged / "share/photo").write_bytes(b"replacement")
        with lifecycle.InstallationGate(self.root) as gate:
            with self.assertRaisesRegex(lifecycle.LifecycleError, "indirect"):
                lifecycle.install_overlay(gate, staged, self.binary)
        self.assertEqual((foreign / "photo").read_bytes(), b"precious")

    def test_seeded_overlay_sequences_preserve_the_model_across_failures(self):
        # These budgets exercise source transactions and fault recovery. Real
        # packages, sessions and Store refresh remain separate acceptance gates.
        for seed in (104729, 130363, 155921):
            generator = random.Random(seed)
            current = self.binary.read_bytes()
            foreign = self.root / "photo.jpg"
            foreign.write_bytes(b"original photo")
            for step in range(100):
                staged = self.work / f"sequence-{seed}-{step}"
                (staged / "bin").mkdir(parents=True)
                candidate = f"generation-{seed}-{step}".encode()
                (staged / "bin/licasa").write_bytes(candidate)
                (staged / "share").mkdir()
                (staged / f"share/owned-{step % 3}").write_text(str(step))
                fail = generator.randrange(4) == 0
                with lifecycle.InstallationGate(self.root) as gate:
                    with mock.patch.object(lifecycle, "owned_processes", return_value={}), \
                            mock.patch.object(lifecycle, "verify_identity",
                                              side_effect=lifecycle.LifecycleError("seeded health fault") if fail else None):
                        if fail:
                            with self.assertRaisesRegex(lifecycle.LifecycleError, "seeded health"):
                                lifecycle.install_overlay(gate, staged, self.binary)
                        else:
                            lifecycle.install_overlay(gate, staged, self.binary)
                            current = candidate
                    self.assertEqual(gate.read()["state"], "verified", (seed, step))
                self.assertEqual(self.binary.read_bytes(), current, (seed, step))
                self.assertEqual(foreign.read_bytes(), b"original photo", (seed, step))
                self.assertFalse(list(self.work.glob(".licasa-local.*")), (seed, step))

    def test_preloader_refuses_locked_or_interrupted_runtime_before_exec(self):
        launcher = self.work / "fixture-launcher"
        marker = self.work / "started"
        launcher.write_text(f'#!/bin/sh\nprintf started > "{marker}"\n')
        launcher.chmod(0o755)
        command = [sys.executable, str(ROOT / "tools/lifecycle_update.py"), "launch",
                   "--root", str(self.root), "--binary", str(self.binary),
                   "--family", "filesystem", "--launcher", str(launcher)]
        with lifecycle.InstallationGate(self.root) as gate:
            gate.write("verified")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists(), "The runtime was executed while replacement owned the gate")
            for arguments in (["--lifecycle", "status"], ["--lifecycle", "identity"], ["--quit"]):
                blocked = subprocess.run(command + ["--", *arguments], capture_output=True, text=True)
                self.assertNotEqual(blocked.returncode, 0)
                self.assertFalse(marker.exists(), "Headless control mapped a locked runtime")
            gate.write("replacing")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(marker.exists(), "An interrupted runtime was executed")
        with lifecycle.InstallationGate(self.root) as gate:
            gate.write("verified")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(marker.read_text(), "started")

    def test_committed_identity_requires_the_inherited_exclusive_gate(self):
        launcher = self.work / "identity-launcher"
        marker = self.work / "identity-started"
        launcher.write_text(f'#!/bin/sh\nprintf ready > "{marker}"\n')
        launcher.chmod(0o755)
        command = [sys.executable, str(ROOT / "tools/lifecycle_update.py"), "launch",
                   "--root", str(self.root), "--binary", str(self.binary),
                   "--family", "filesystem", "--launcher", str(launcher), "--", "--lifecycle", "identity"]
        with lifecycle.InstallationGate(self.root) as gate:
            gate.write("committed")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists())
            result = subprocess.run(command, capture_output=True, text=True, pass_fds=(gate.fd,),
                                    env=dict(os.environ, LICASA_IDENTITY_GATE_FD=str(gate.fd)))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(marker.read_text(), "ready")

    def test_appimage_wrapper_is_idempotent_and_preserves_original_apprun(self):
        appdir = self.work / "appdir"
        appdir.mkdir()
        (appdir / "usr/libexec/licasa").mkdir(parents=True)
        shutil.copy2(ROOT / "tools/lifecycle_update.py", appdir / "usr/libexec/licasa/lifecycle_update.py")
        original = '#!/bin/sh\nprintf original\n'
        (appdir / "AppRun").write_text(original)
        lifecycle.install_appimage_launcher(appdir)
        lifecycle.install_appimage_launcher(appdir)
        self.assertEqual((appdir / "AppRun.lifecycle-original").read_text(), original)
        self.assertIn("--family appimage", (appdir / "AppRun").read_text())

    def test_local_desktop_launcher_preserves_special_paths_in_real_gio_parser(self):
        python = "/usr/bin/python3"
        available = subprocess.run([python, "-c", "from gi.repository import Gio"],
                                   capture_output=True).returncode == 0
        if not available:
            self.skipTest("GIO Python bindings required for desktop activation")
        directory = self.work / 'local "quoted" $literal `literal` \\slash % 日本語'
        directory.mkdir()
        launcher = directory / "licasa-launch"
        marker = self.work / "launched-arguments"
        launcher.write_text('#!/bin/bash\nprintf "%s\\0" "$@" > "$CAPTURE_ARGUMENTS.pending"\n'
                            'mv -- "$CAPTURE_ARGUMENTS.pending" "$CAPTURE_ARGUMENTS"\n')
        launcher.chmod(0o755)
        photo = directory / 'photo "quoted" % 日本語.png'
        photo.write_bytes(b"fixture")
        desktop = self.work / "local.desktop"
        desktop.write_text('[Desktop Entry]\nType=Application\nName=Licasa (Local)\n'
                           f'Exec=bash {desktop_exec_argument(launcher)} %f\n')
        completed = subprocess.run([python, "-c", '''import sys
from gi.repository import Gio
app = Gio.DesktopAppInfo.new_from_filename(sys.argv[1])
assert app is not None
app.launch([Gio.File.new_for_path(sys.argv[2])], None)
''', str(desktop), str(photo)], env=dict(os.environ, CAPTURE_ARGUMENTS=str(marker)),
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(marker.read_bytes(), os.fsencode(photo) + b"\0")

    def test_launchers_do_not_adopt_an_enclosing_package_identity(self):
        launcher = self.work / "environment-launcher"
        launcher.write_text('#!/bin/sh\nprintf "%s|%s|%s" "${APPIMAGE-unset}" "${APPDIR-unset}" "${SNAP_NAME-unset}"\n')
        launcher.chmod(0o755)
        environment = dict(os.environ, APPIMAGE="/foreign.AppImage", APPDIR="/foreign-appdir",
                           SNAP_NAME="foreign-snap", SNAP_USER_DATA="/foreign-data")
        command = [sys.executable, str(ROOT / "tools/lifecycle_update.py"), "launch",
                   "--root", str(self.root), "--binary", str(self.binary),
                   "--family", "filesystem", "--launcher", str(launcher)]
        completed = subprocess.run(command, env=environment, capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout, "unset|unset|unset")
        appdir = self.work / "portable-appdir"
        (appdir / "usr/libexec/licasa").mkdir(parents=True)
        shutil.copy2(ROOT / "tools/lifecycle_update.py", appdir / "usr/libexec/licasa/lifecycle_update.py")
        shutil.copy2(launcher, appdir / "AppRun")
        lifecycle.install_appimage_launcher(appdir)
        completed = subprocess.run([str(appdir / "AppRun")], env=environment,
                                   capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout, f"unset|{appdir}|unset")
        completed = subprocess.run([str(appdir / "AppRun")],
                                   env=dict(environment, APPDIR=str(appdir), APPIMAGE="/this.AppImage"),
                                   capture_output=True, text=True)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout, f"/this.AppImage|{appdir}|unset")


if __name__ == "__main__":
    unittest.main()
