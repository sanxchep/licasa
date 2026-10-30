#!/usr/bin/env python3
"""Protocol/evidence negative controls without a display or host installation."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from process_fixture import start_fixture, stop_fixture


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "verify_update_lifecycle", ROOT / "tools/verify_update_lifecycle.py")
LIFECYCLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIFECYCLE)


class LifecycleProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="lc-protocol-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.identity = {"protocol": 1, "installationId": "a" * 32,
                         "generation": "b" * 64, "result": "ready"}

    def reply(self, **changes):
        return {**self.identity, "pid": os.getpid(),
                "startIdentity": LIFECYCLE.process_start_identity(os.getpid()), **changes}

    def serve_once(self, response, verify=None):
        endpoint = self.root / "fixture.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(endpoint))
        listener.listen(1)
        listener.settimeout(2)
        failures = []

        def serve():
            try:
                with listener.accept()[0] as client:
                    client.settimeout(2)
                    request = bytearray()
                    while b"\n" not in request:
                        chunk = client.recv(4096)
                        if not chunk:
                            break
                        request.extend(chunk)
                    if verify:
                        verify(bytes(request))
                    for fragment in response:
                        client.sendall(fragment)
            except Exception as error:
                failures.append(error)
            finally:
                listener.close()

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()

        def finish():
            worker.join(timeout=3)
            self.assertFalse(worker.is_alive(), "Fixture server exceeded its deadline")
            if failures:
                raise failures[0]

        self.addCleanup(finish)
        return endpoint

    def test_json_protocol_accepts_known_result_and_exact_installation(self):
        decoded = LIFECYCLE.decode_reply(json.dumps(self.reply()).encode())
        LIFECYCLE.validate_reply(decoded, self.identity, 0, running=True)

    def test_unknown_protocol_is_not_treated_as_success(self):
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "Unsupported"):
            LIFECYCLE.decode_reply(json.dumps(self.reply(protocol=2)).encode())

    def test_unknown_result_is_not_treated_as_success(self):
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "Unknown"):
            LIFECYCLE.decode_reply(json.dumps(self.reply(result="maybe-stopped")).encode())

    def test_empty_malformed_array_and_oversized_replies_fail(self):
        for payload in (b"", b"not JSON", b"[]", b"x" * 16385):
            with self.subTest(payload=payload[:20]):
                with self.assertRaises(LIFECYCLE.QualificationFailure):
                    LIFECYCLE.decode_reply(payload)

    def test_wrong_installation_cannot_qualify_as_this_installation(self):
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "another installation"):
            LIFECYCLE.validate_reply(self.reply(installationId="c" * 32), self.identity)

    def test_wrong_generation_cannot_qualify_running_replacement(self):
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "wrong generation"):
            LIFECYCLE.validate_reply(self.reply(generation="c" * 64), self.identity, running=True)

    def test_missing_pid_or_start_identity_cannot_qualify_process(self):
        for change in ({"pid": 0}, {"pid": "123"}, {"startIdentity": ""}):
            with self.subTest(change=change):
                with self.assertRaises(LIFECYCLE.QualificationFailure):
                    LIFECYCLE.validate_reply(self.reply(**change), self.identity, running=True)

    def test_exit_status_must_agree_with_structured_result(self):
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "exit code disagree"):
            LIFECYCLE.validate_reply(self.reply(result="busy"), self.identity, returncode=0)

    def test_fragmented_socket_reply_and_request_framing(self):
        expected = {"protocol": 1, "installationId": "a" * 32, "operation": "status"}

        def verify(payload):
            self.assertTrue(payload.endswith(b"\n"))
            self.assertEqual(json.loads(payload), expected)

        payload = json.dumps(self.reply()).encode() + b"\n"
        endpoint = self.serve_once([payload[:3], payload[3:17], payload[17:]], verify)
        result = LIFECYCLE.socket_request(endpoint, expected, 2)
        self.assertEqual(result, self.reply())

    def test_socket_eof_without_reply_is_explicit_absence(self):
        endpoint = self.serve_once([])
        self.assertIsNone(LIFECYCLE.socket_request(endpoint, {"operation": "status"}, 2))

    def test_acknowledgment_alone_does_not_establish_process_exit(self):
        child = start_fixture(self, [sys.executable, "-c", "import sys; sys.stdin.buffer.read()"])
        try:
            with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "still alive"):
                LIFECYCLE.wait_for_exit(child, 0.01)
            self.assertIsNone(child.poll())
            self.assertTrue(LIFECYCLE.process_start_identity(child.pid))
        finally:
            child.stdin.close()
            try:
                child.wait(timeout=2)
            finally:
                stop_fixture(child)
        self.assertIsNone(LIFECYCLE.process_start_identity(child.pid))

    def test_normal_exit_is_observed_independently_of_reply(self):
        child = start_fixture(self, [sys.executable, "-c", "raise SystemExit(0)"])
        self.assertEqual(LIFECYCLE.wait_for_exit(child, 2), 0)
        self.assertIsNone(LIFECYCLE.process_start_identity(child.pid))

    def test_control_rejects_zero_exit_with_busy_result_and_retains_output(self):
        candidate = self.root / "candidate"
        candidate.write_text(f"#!{sys.executable}\nimport json\n"
                             f"print(json.dumps({self.reply(result='busy')!r}))\n")
        candidate.chmod(0o700)
        output = self.root / "evidence"
        output.mkdir()
        campaign = LIFECYCLE.ProcessCampaign(candidate, output, 2)
        campaign.identity = self.identity
        campaign.environment = dict(os.environ)
        with self.assertRaisesRegex(LIFECYCLE.QualificationFailure, "exit code disagree"):
            campaign.control("stop-for-update")
        trace = [json.loads(line) for line in (output / "trace.jsonl").read_text().splitlines()]
        self.assertEqual(len(trace), 1)
        self.assertIn('"result": "busy"', trace[0]["stdout"])

    def test_private_environment_never_inherits_host_session(self):
        output = self.root / "evidence"
        output.mkdir()
        campaign = LIFECYCLE.ProcessCampaign(Path(sys.executable), output, 2)
        private = self.root / "private"
        private.mkdir()
        campaign.configure(private)
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                     "SNAP", "SNAP_USER_DATA", "SNAP_USER_COMMON", "APPIMAGE", "APPDIR"):
            self.assertNotIn(name, campaign.environment)
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR"):
            path = Path(campaign.environment[name])
            self.assertTrue(path.is_relative_to(private))
            self.assertEqual(path.stat().st_mode & 0o777, 0o700)


if __name__ == "__main__":
    unittest.main()
