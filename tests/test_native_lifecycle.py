#!/usr/bin/env python3
"""Native lifecycle trust, bounded protocol and package-handler regressions."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import native_lifecycle as native
import package_deb
from process_fixture import start_fixture, wait_for_message


SERVER = r'''import json,os,socket,sys,time
path,reply=sys.argv[1:]
server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
server.bind(path);os.chmod(path,0o600);server.listen()
print('ready',flush=True)
connection,_=server.accept()
with connection:
    request=b''
    while b'\n' not in request:
        part=connection.recv(4096)
        if not part: break
        request+=part
    if reply=='oversized': connection.sendall(b'x'*20000+b'\n')
    elif reply!='closed':
        value=json.loads(reply)
        target=value.pop('_execTarget',None) if isinstance(value,dict) else None
        if target:
            connection.set_inheritable(True)
            source='import socket,sys,time; c=socket.socket(fileno=int(sys.argv[1])); c.sendall(sys.argv[2].encode()+b"\\n"); time.sleep(3)'
            os.execv(target,[target,'-c',source,str(connection.fileno()),json.dumps(value)])
        connection.sendall(reply.encode()+b'\n')
time.sleep(1)
'''


@unittest.skipUnless(sys.platform == "linux", "Qualified Linux native control boundary")
class NativeLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lc-native-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.peers = []

    def test_private_runtime_accepts_only_owned_non_symlinked_private_path(self):
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        native.private_runtime(runtime, os.geteuid())
        with self.assertRaises(native.NativeLifecycleError):
            native.private_runtime(runtime, os.geteuid() + 1)
        runtime.chmod(0o755)
        with self.assertRaises(native.NativeLifecycleError):
            native.private_runtime(runtime, os.geteuid())
        runtime.chmod(0o700)
        alias = self.root / "alias"
        alias.symlink_to(runtime, target_is_directory=True)
        with self.assertRaises(native.NativeLifecycleError):
            native.private_runtime(alias, os.geteuid())
        with self.assertRaises(native.NativeLifecycleError):
            native.private_runtime(Path("relative/runtime"), os.geteuid())

    def test_writable_runtime_ancestor_is_rejected(self):
        parent = self.root / "shared"
        parent.mkdir(mode=0o777)
        parent.chmod(0o777)
        runtime = parent / "private"
        runtime.mkdir(mode=0o700)
        with self.assertRaises(native.NativeLifecycleError):
            native.private_runtime(runtime, os.geteuid())

    def test_start_identity_is_stable_and_excludes_zombie(self):
        identity = native.start_identity(os.getpid())
        self.assertIn(":", identity)
        self.assertEqual(native.start_identity(os.getpid()), identity)
        child = os.fork()
        if child == 0:
            os._exit(0)
        try:
            os.waitid(os.P_PID, child, os.WEXITED | os.WNOWAIT)
            self.assertEqual(native.start_identity(child), "")
        finally:
            os.waitpid(child, 0)

    def test_only_exact_recognized_headless_invocations_are_transient_controls(self):
        for operation in (b"identity", b"status", b"health", b"prepare-update",
                          b"stop-for-update", b"cancel-update", b"quit"):
            with self.subTest(operation=operation):
                self.assertTrue(native.is_headless_control(b"/usr/bin/licasa\0--lifecycle\0" + operation + b"\0"))
        self.assertTrue(native.is_headless_control(b"/usr/bin/licasa\0--quit\0"))
        for command_line in (b"", b"\0--quit\0", b"licasa\0", b"licasa\0--lifecycle\0",
                             b"licasa\0--lifecycle\0unknown\0", b"licasa\0--quit\0photo.jpg\0",
                             b"licasa\0--lifecycle\0status\0photo.jpg\0",
                             b"licasa\0--lifecycle\0status", b"x" * 65537 + b"\0"):
            with self.subTest(command_line=command_line[:60]):
                self.assertFalse(native.is_headless_control(command_line))

    def test_root_observes_transient_client_exit_and_refuses_nonexiting_claim(self):
        transient = start_fixture(self, [sys.executable, "-c", "import time; time.sleep(0.1)"])
        self.peers.append(transient)
        native.wait_for_control_exit(transient.pid, native.start_identity(transient.pid), timeout=2.0)
        self.assertEqual(transient.wait(timeout=2), 0)
        live = start_fixture(self, [sys.executable, "-c", "import time; time.sleep(5)"])
        self.peers.append(live)
        with self.assertRaisesRegex(native.NativeLifecycleError, "has not exited"):
            native.wait_for_control_exit(live.pid, native.start_identity(live.pid), timeout=0.05)
        self.assertIsNone(live.poll())

    def test_inaccessible_executable_cannot_be_omitted_using_mutable_comm(self):
        source = ('import ctypes,time; c=ctypes.CDLL(None); '
                  'assert c.prctl(15,b"unrelated")==0; '
                  'print("ready",flush=True); time.sleep(5)')
        peer = start_fixture(self, [sys.executable, "-c", source])
        self.peers.append(peer)
        wait_for_message(peer)
        directory = Path(f"/proc/{peer.pid}")
        self.assertEqual((directory / "comm").read_text().strip(), "unrelated")
        with patch.object(Path, "iterdir", return_value=iter([directory])):
            with patch.object(native.os, "readlink", side_effect=PermissionError("inspection denied")):
                with self.assertRaisesRegex(native.NativeLifecycleError, "process inspection is unavailable"):
                    native.processes("native")

    def peer(self, *, mode: str = "valid", changed: dict | None = None):
        identity = "a" * 32
        runtime = self.root / f"runtime-{len(self.peers)}"
        runtime.mkdir(mode=0o700)
        endpoint = runtime / f"licasa-lc-{identity}.sock"
        # The server's response file is filled after its independently observed
        # PID/start/generation are available, before its first request arrives.
        reply_file = self.root / f"reply-{len(self.peers)}.json"
        source = SERVER.replace("path,reply=sys.argv[1:]", "path,reply_file=sys.argv[1:]\nfrom pathlib import Path")
        source = source.replace("    if reply=='oversized':", "    reply=Path(reply_file).read_text()\n    if reply=='oversized':")
        peer = start_fixture(self, [sys.executable, "-c", source, str(endpoint), str(reply_file)])
        self.peers.append(peer)
        wait_for_message(peer)
        process = {"pid": peer.pid, "uid": os.geteuid(), "gid": os.getegid(),
                   "runtime": str(runtime), "installationId": identity,
                   "startIdentity": native.start_identity(peer.pid),
                   "generation": hashlib.sha256(Path(f"/proc/{peer.pid}/exe").read_bytes()).hexdigest()}
        executable = Path(f"/proc/{peer.pid}/exe").stat()
        process.update(executableDevice=executable.st_dev, executableInode=executable.st_ino)
        reply = {"protocol": 1, "result": "ready", "installationId": identity,
                 "pid": peer.pid, "startIdentity": process["startIdentity"],
                 "generation": process["generation"]}
        reply.update(changed or {})
        reply_file.write_text(json.dumps(reply) if mode == "valid" else mode)
        return process, endpoint

    def test_real_peer_receipt_accepts_independently_observed_identity(self):
        process, _ = self.peer()
        reply = native.user_request(process, "status")
        self.assertEqual(reply["result"], "ready")
        self.assertEqual(reply["pid"], process["pid"])

    def test_real_peer_mismatched_receipts_are_rejected(self):
        for changed in ({"protocol": 2}, {"installationId": "b" * 32}, {"pid": -1},
                        {"startIdentity": "reused-pid"}, {"generation": "wrong-build"}):
            with self.subTest(changed=changed):
                process, _ = self.peer(changed=changed)
                with self.assertRaises(native.NativeLifecycleError):
                    native.user_request(process, "prepare-update")

    def test_real_peer_oversized_and_incomplete_receipts_are_rejected(self):
        for mode in ("oversized", "closed"):
            with self.subTest(mode=mode):
                process, _ = self.peer(mode=mode)
                with self.assertRaises(native.NativeLifecycleError):
                    native.user_request(process, "status")

    def test_real_peer_non_object_receipts_report_a_protocol_error(self):
        for mode in ("null", "[]", '"text"'):
            with self.subTest(mode=mode):
                process, _ = self.peer(mode=mode)
                with self.assertRaisesRegex(native.NativeLifecycleError, "invalid lifecycle receipt"):
                    native.user_request(process, "status")

    def test_invalid_journal_and_payload_objects_report_repair_errors(self):
        gate = native.NativeGate("native")
        gate.directory = self.root
        journal = self.root / "transaction.json"
        for contents in (b"\xff", "null", "[]", '"text"', "{"):
            with self.subTest(contents=contents), patch.object(native, "trusted"):
                journal.write_bytes(contents if isinstance(contents, bytes) else contents.encode())
                with self.assertRaisesRegex(native.NativeLifecycleError, "journal is invalid"):
                    gate.read()
                configuration = dict(native.CONFIGURATIONS["native"], receipt=str(journal))
                with patch.dict(native.CONFIGURATIONS, native=configuration):
                    with self.assertRaisesRegex(native.NativeLifecycleError, "receipt is invalid"):
                        native.verify_payload("native")

    def test_invalid_payload_file_records_are_rejected_before_inspection(self):
        receipt = self.root / "native-runtime.json"
        configuration = dict(native.CONFIGURATIONS["native"], receipt=str(receipt))
        for value in (None, [], "text", {}, {"type": []}, {"type": "unknown"}):
            receipt.write_text(json.dumps({"schema": 1, "installation": "native",
                                           "files": {"/usr/bin/licasa.bin": value}}))
            with self.subTest(value=value), patch.object(native, "trusted"), \
                    patch.dict(native.CONFIGURATIONS, native=configuration):
                with self.assertRaisesRegex(native.NativeLifecycleError, "invalid file record"):
                    native.verify_payload("native")

    def test_failed_native_gate_entry_closes_the_lock_descriptor(self):
        gate = native.NativeGate("native")
        gate.directory = self.root
        opened = []
        original_open = os.open

        def open_lock(*args, **kwargs):
            descriptor = original_open(*args, **kwargs)
            opened.append(descriptor)
            return descriptor

        def check_trust(path, **kwargs):
            if path.name == "lock":
                raise native.NativeLifecycleError("unsafe lock fixture")

        with patch.object(native.os, "geteuid", return_value=0), \
                patch.object(native.os, "open", side_effect=open_lock), \
                patch.object(native, "trusted", side_effect=check_trust):
            with self.assertRaisesRegex(native.NativeLifecycleError, "unsafe lock fixture"):
                gate.__enter__()
        self.assertEqual(len(opened), 1)
        with self.assertRaises(OSError):
            os.fstat(opened[0])
        self.assertIsNone(gate.lock)

    def test_socket_alias_and_wrong_process_are_rejected_before_command(self):
        process, endpoint = self.peer()
        actual = endpoint.with_name("actual.sock")
        endpoint.rename(actual)
        endpoint.symlink_to(actual)
        with self.assertRaises(native.NativeLifecycleError):
            native.user_request(process, "stop-for-update")
        endpoint.unlink()
        actual.rename(endpoint)
        process["pid"] = os.getpid()
        with self.assertRaises(native.NativeLifecycleError):
            native.user_request(process, "stop-for-update")

    def test_exec_preserving_pid_and_start_time_cannot_forge_old_receipt(self):
        replacement = self.root / "replacement-python"
        shutil.copy2(Path(sys.executable).resolve(), replacement)
        replacement.chmod(0o755)
        process, _ = self.peer(changed={"_execTarget": str(replacement)})
        with self.assertRaisesRegex(native.NativeLifecycleError, "executed another program"):
            native.user_request(process, "prepare-update")

    def test_oversized_journal_is_refused_before_any_durable_state_change(self):
        gate = native.NativeGate("native")
        with patch.object(gate, "atomic") as atomic:
            with self.assertRaises(native.NativeLifecycleError):
                gate.record("prepared", predecessors=[{"identity": "x" * native.LIMIT}])
            atomic.assert_not_called()

    def test_staged_receipt_identifies_files_links_and_executable_permissions(self):
        stage = self.root / "package"
        binary = stage / "usr/bin/licasa.bin"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"\x7fELFqualification-fixture")
        binary.chmod(0o755)
        link = binary.with_name("licasa-link")
        link.symlink_to("licasa.bin")
        native.stage_payload(stage, "native")
        record = json.loads((stage / "usr/share/licasa/native-runtime.json").read_text())
        self.assertEqual(record["files"]["/usr/bin/licasa.bin"]["sha256"], hashlib.sha256(binary.read_bytes()).hexdigest())
        self.assertEqual(record["files"]["/usr/bin/licasa.bin"]["mode"], 0o755)
        self.assertEqual(record["files"]["/usr/bin/licasa-link"], {"type": "symlink", "target": "licasa.bin"})
        for installation in native.CONFIGURATIONS:
            subprocess.run(["bash", "-n"], input=native.launcher_text(installation), text=True, check=True)

    def test_debian_abort_and_upgrade_handlers_do_not_reconfigure_or_remove(self):
        package_deb.write_maintainer_scripts(self.root)
        for name, argument in (("postinst", "abort-upgrade"), ("postinst", "abort-remove"),
                               ("prerm", "upgrade"), ("postrm", "upgrade"), ("postrm", "abort-install")):
            with self.subTest(name=name, argument=argument):
                script = self.root / name
                subprocess.run(["sh", "-n", str(script)], check=True)
                result = subprocess.run(["sh", str(script), argument], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "")

    @unittest.skipIf(os.geteuid() == 0, "Root-only operations are exercised in disposable manager guests")
    def test_incoming_self_contained_script_refuses_unprivileged_package_mutation(self):
        source = native.embedded_script("native", "prepare")
        script = self.root / "incoming-preinst"
        script.write_text(source)
        result = subprocess.run(["sh", str(script)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("through the package manager as root", result.stderr)


if __name__ == "__main__":
    unittest.main()
