#!/usr/bin/env python3
"""Retry transient Snap Store errors with a shared, bounded retry budget."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import time


DEFAULT_BUDGET = 30 * 60
PERMANENT = re.compile(
    r"unauthori[sz]ed|forbidden|\b40[13]\b|invalid credentials|expired credentials|"
    r"not logged in|permission denied|already been uploaded|manual review|"
    r"CMake Error|ninja: build stopped|make(?:\[\d+\])?: \*\*\*", re.I)
TRANSIENT = re.compile(
    r"unable to contact snap store|cannot contact (?:the )?snap store|"
    r"Client\.Timeout exceeded|i/o timeout|TLS handshake timeout|"
    r"timeout awaiting response headers|context deadline exceeded|"
    r"timeout exceeded while waiting for response|RemoteDisconnected|"
    r"temporary failure in name resolution|connection reset by peer|"
    r"service unavailable|bad gateway|gateway timeout|too many requests|"
    r"(?:HTTP|status(?: code)?|response)[^\n]{0,40}\b(?:408|429|500|502|503|504)\b", re.I)


def transient_error(output: str) -> bool:
    # Ignore earlier recovered warnings if the final failure is a build/auth error.
    tail = "\n".join(output.splitlines()[-60:])
    return not PERMANENT.search(tail) and bool(TRANSIENT.search(tail))


@dataclass
class Attempt:
    code: int
    output: str
    timed_out: bool = False


def terminate(process: subprocess.Popen) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    finally:
        # The parent can exit before a descendant closes the output pipe.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3)


def execute(command: list[str], timeout: float, *, cwd=None) -> Attempt:
    """Stream diagnostics, keeping only a bounded tail for error classification."""
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True, cwd=cwd)
    tail = bytearray()
    deadline = time.monotonic() + timeout
    timed_out = False
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    break
                for key, _ in selector.select(min(remaining, 0.2)):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                    tail.extend(chunk)
                    del tail[:-262144]
            if not timed_out:
                try:
                    process.wait(timeout=max(0.001, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    timed_out = True
    finally:
        terminate(process)
        process.stdout.close()
    return Attempt(124 if timed_out else process.returncode,
                   tail.decode(errors="replace"), timed_out)


def positive(value: str) -> float:
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("Expected a finite positive number")
    return number


def retry(command: list[str], *, seconds=DEFAULT_BUDGET, delay=30, initial_timeout=None,
          budget_file: Path | None = None, report: Path | None = None,
          runner=execute, clock=time.monotonic, sleep=time.sleep) -> int:
    remaining = seconds
    if budget_file and budget_file.exists():
        state = json.loads(budget_file.read_text())
        remaining = state["remaining_seconds"]
        if (state.get("schema_version") != 1 or state.get("budget_seconds") != seconds
                or type(remaining) not in (int, float) or not math.isfinite(remaining)
                or not 0 <= remaining <= seconds):
            raise ValueError("Invalid Snap retry budget; refusing to reset it")
    records = []
    retry_start = None
    deadline = None
    reason = "command failed"
    code = 1
    try:
        while True:
            available = max(0, deadline - clock()) if deadline is not None else remaining
            if deadline is not None and available <= 0:
                code, reason = 124, "retry budget exhausted"
                break
            limit = available if deadline is not None else (initial_timeout or seconds)
            started = clock()
            result = runner(command, limit)
            records.append({"attempt": len(records) + 1, "exit_code": result.code,
                            "duration_seconds": max(0, clock() - started),
                            "timed_out": result.timed_out})
            code = result.code if result.code >= 0 else 128 - result.code
            if deadline is not None and clock() >= deadline:
                code, reason = 124, "retry budget exhausted"
                break
            if code == 0:
                reason = "success"
                break
            if result.timed_out:
                reason = "command deadline exceeded"
                break
            if not transient_error(result.output):
                reason = "non-transient failure; not retried"
                break
            if retry_start is None:
                retry_start = clock()
                deadline = retry_start + remaining
            available = max(0, deadline - clock())
            pause = min(delay, available)
            print(f"Snap Store unavailable; retrying in {pause:g}s "
                  f"({available:.1f}s retry budget left)", flush=True)
            sleep(pause)
    except KeyboardInterrupt:
        code, reason = 130, "interrupted"
        raise
    finally:
        if retry_start is not None:
            remaining = max(0, remaining - max(0, clock() - retry_start))
        state = {"schema_version": 1, "budget_seconds": seconds,
                 "remaining_seconds": remaining}
        if budget_file:
            budget_file.parent.mkdir(parents=True, exist_ok=True)
            temporary = budget_file.with_name(budget_file.name + ".tmp")
            temporary.write_text(json.dumps(state, indent=2) + "\n")
            temporary.replace(budget_file)
        if report:
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text(json.dumps({**state, "reason": reason, "exit_code": code,
                                          "attempts": records}, indent=2) + "\n")
    if code:
        print(f"Snap operation stopped: {reason} (exit {code})", file=sys.stderr)
    return code


def interrupt_on_term():
    def interrupted(_signum, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=positive, default=DEFAULT_BUDGET)
    parser.add_argument("--delay", type=positive, default=30)
    parser.add_argument("--initial-timeout", type=positive)
    parser.add_argument("--budget-file", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Provide a command after --")
    interrupt_on_term()
    try:
        return retry(command, seconds=args.seconds, delay=args.delay,
                     initial_timeout=args.initial_timeout,
                     budget_file=args.budget_file, report=args.report)
    except (OSError, ValueError, KeyError) as error:
        print(f"Snap retry setup failed: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
