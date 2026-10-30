"""Bounded readiness and cleanup for real test-only child processes."""

import os
import selectors
import signal
import subprocess
import time


class FixtureError(AssertionError):
    pass


def stop_fixture(process, timeout=2):
    """Reap the child and close its pipes even when startup or an assertion fails."""
    if process.returncode is not None and all(stream is None or stream.closed
                                              for stream in (process.stdin, process.stdout, process.stderr)):
        return

    def send(sig):
        try:
            if getattr(process, "fixture_process_group", False):
                os.killpg(process.pid, sig)
            else:
                process.send_signal(sig)
        except ProcessLookupError:
            pass

    try:
        send(signal.SIGTERM)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            send(signal.SIGKILL)
            process.wait(timeout=timeout)
    finally:
        # A descendant can retain stdout after the direct child has exited.
        send(signal.SIGKILL)
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None:
                stream.close()


def start_fixture(testcase, command, **kwargs):
    """Register cleanup before the caller can fail, without inheriting CTest pipes."""
    # Ordinary children stay in the module runner's group so its timeout can
    # stop the whole module. Opt in only for fixtures that own a process tree.
    separate_group = kwargs.pop("start_new_session", False)
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0,
                               start_new_session=separate_group, **kwargs)
    process.fixture_process_group = separate_group and os.name == "posix"
    testcase.addCleanup(stop_fixture, process)
    return process


def wait_for_message(process, expected=b"ready\n", timeout=5):
    """Wait for a complete explicit signal, including a deadline for partial lines."""
    received = b""
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while received != expected:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise FixtureError(f"Timed out waiting for {expected!r}; received {received!r}")
            part = os.read(process.stdout.fileno(), 1)
            if not part:
                raise FixtureError(f"Fixture exited before {expected!r}; received {received!r}")
            received += part
            if not expected.startswith(received):
                raise FixtureError(f"Unexpected fixture signal {received!r}; expected {expected!r}")
