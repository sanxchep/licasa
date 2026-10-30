"""Exercise fixture failures that previously left CTest waiting on inherited pipes."""

import os
import selectors
import signal
import sys
import unittest

from process_fixture import FixtureError, start_fixture, stop_fixture, wait_for_message


@unittest.skipUnless(os.name == "posix", "Unix fixture pipes and process groups")
class ProcessFixtureTests(unittest.TestCase):
    def child(self, source, **kwargs):
        return start_fixture(self, [sys.executable, "-c", source], **kwargs)

    def test_ready_waits_for_the_complete_signal(self):
        process = self.child("import sys; print('ready', flush=True); sys.stdin.read(1)")
        wait_for_message(process)
        self.assertIsNone(process.poll())

    def test_partial_signal_has_a_deadline(self):
        process = self.child("import os,sys; os.write(1, b'bound\\nrea'); sys.stdin.read(1)")
        wait_for_message(process, b"bound\n")
        with self.assertRaisesRegex(FixtureError, "received b'rea'"):
            wait_for_message(process, timeout=0.05)

    def test_early_exit_and_unexpected_output_fail_startup(self):
        for source, message in (("pass", "exited"), ("print('error', flush=True)", "Unexpected")):
            with self.subTest(source=source):
                process = self.child(source)
                with self.assertRaisesRegex(FixtureError, message):
                    wait_for_message(process)

    def test_assertion_failure_reaps_child_and_closes_pipes(self):
        fixture = unittest.TestCase()
        process = start_fixture(fixture, [sys.executable, "-c",
                                        "import sys; print('ready', flush=True); sys.stdin.read(1)"])
        try:
            wait_for_message(process)
            with self.assertRaisesRegex(AssertionError, "original failure"):
                try:
                    raise AssertionError("original failure")
                finally:
                    fixture.doCleanups()
            self.assertIsNotNone(process.returncode)
            for stream in (process.stdin, process.stdout):
                self.assertTrue(stream.closed)
        finally:
            fixture.doCleanups()

    def test_cleanup_escalates_when_child_ignores_termination(self):
        process = self.child("import signal,sys; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('ready', flush=True); sys.stdin.read(1)")
        wait_for_message(process)
        stop_fixture(process, timeout=0.05)
        self.assertEqual(process.returncode, -signal.SIGKILL)

    def test_cleanup_releases_output_pipe_inherited_by_descendant(self):
        process = self.child("import os,sys; child=os.fork(); print('ready', flush=True) if child else sys.stdin.read(1)",
                             start_new_session=True)
        wait_for_message(process)
        process.wait(timeout=5)
        # Keep a duplicate so closing the parent's stream alone cannot fake EOF.
        with os.fdopen(os.dup(process.stdout.fileno()), "rb", buffering=0) as output:
            stop_fixture(process)
            with selectors.DefaultSelector() as selector:
                selector.register(output, selectors.EVENT_READ)
                self.assertTrue(selector.select(5), "Descendant retained the output pipe")
                self.assertEqual(output.read(), b"")


if __name__ == "__main__":
    unittest.main()
