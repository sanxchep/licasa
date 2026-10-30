#!/usr/bin/env python3
"""Run independent unittest modules in isolated processes on available CPU cores."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fnmatch
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def available_jobs() -> int:
    try:
        return len(os.sched_getaffinity(0)) or 1
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def positive_int(value: str) -> int:
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def terminate(process: subprocess.Popen) -> None:
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGKILL)
        else:
            process.kill()
    except ProcessLookupError:
        pass


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests", type=Path, default=ROOT / "tests")
    parser.add_argument("--pattern", action="append", help="Repeat to select several module patterns")
    parser.add_argument("--jobs", type=positive_int, default=available_jobs())
    parser.add_argument("--timeout", type=positive_int, default=300, help="Seconds per module")
    args = parser.parse_args(argv)
    directory = args.tests.resolve()
    modules = []
    for parent, packages, files in os.walk(directory):
        packages[:] = sorted(name for name in packages if (Path(parent) / name / "__init__.py").is_file())
        modules.extend(Path(parent) / name for name in sorted(files)
                       if name.endswith(".py") and any(fnmatch.fnmatch(name, pattern)
                                                     for pattern in args.pattern or ["test_*.py"]))
    if not modules:
        parser.error(f"No test modules selected in {directory}")
    workers = min(args.jobs, len(modules))
    children = set()
    lock = threading.Lock()
    cancelled = threading.Event()

    def run_module(path):
        started = time.monotonic()
        module = ".".join(path.relative_to(directory).with_suffix("").parts)
        command = [sys.executable, "-B", "-m", "unittest", "-v", module]
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(directory) + os.pathsep + environment.get("PYTHONPATH", "")
        with lock:
            if cancelled.is_set():
                return path.name, False, "Cancelled\n", 0
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, env=environment, start_new_session=os.name == "posix")
            children.add(process)
        try:
            try:
                output, _ = process.communicate(timeout=args.timeout)
                passed = process.returncode == 0
            except subprocess.TimeoutExpired:
                terminate(process)
                output, _ = process.communicate()
                output += f"\nModule timed out after {args.timeout}s\n"
                passed = False
            return path.relative_to(directory).as_posix(), passed, output, time.monotonic() - started
        finally:
            with lock:
                children.discard(process)

    print(f"Running {len(modules)} test modules with {workers} worker processes", flush=True)
    failures = 0
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(run_module, path) for path in modules]
        try:
            for future in as_completed(futures):
                try:
                    name, passed, output, elapsed = future.result()
                except Exception as error:
                    failures += 1
                    print(f"FAIL: worker error: {error}", flush=True)
                    continue
                failures += not passed
                print(f"\n{'PASS' if passed else 'FAIL'}: {name} ({elapsed:.2f}s)\n{output}",
                      end="" if output.endswith("\n") else "\n", flush=True)
        except KeyboardInterrupt:
            with lock:
                cancelled.set()
                for process in children:
                    terminate(process)
            for future in futures:
                future.cancel()
            return 130
    print(f"\n{len(modules)} modules, {failures} failed, {time.monotonic() - started:.2f}s elapsed",
          flush=True)
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
