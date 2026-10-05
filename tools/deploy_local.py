#!/usr/bin/env python3
"""Install an already-built Licasa tree into the current user's local prefix."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import time
from pathlib import Path


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def local_processes(binary: Path) -> set[int]:
    """Find this user's Local install, including a running deleted executable."""
    result = set()
    for process_dir in Path("/proc").iterdir():
        if not process_dir.name.isdecimal():
            continue
        try:
            if process_dir.stat().st_uid != os.geteuid():
                continue
            executable = os.readlink(process_dir / "exe").removesuffix(" (deleted)")
            if executable == str(binary):
                result.add(int(process_dir.name))
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            # The process may have exited while /proc was being scanned.
            continue
    return result


def wait_for_local_processes(binary: Path, present: bool, timeout: float) -> set[int]:
    deadline = time.monotonic() + timeout
    while True:
        processes = local_processes(binary)
        if bool(processes) == present or time.monotonic() >= deadline:
            return processes
        time.sleep(0.1)


def stop_running_local_copy(binary: Path) -> bool:
    running = local_processes(binary)
    if not running:
        return False
    if not binary.is_file():
        raise SystemExit(f"Local Licasa is running, but its launcher is missing: {binary}")

    print(f"Stopping running Local Licasa ({', '.join(map(str, sorted(running)))})", flush=True)
    try:
        run([str(binary), "--quit"])
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"Could not ask Local Licasa to quit: {error}") from error

    still_running = wait_for_local_processes(binary, present=False, timeout=30)
    if still_running:
        raise SystemExit(
            "Local Licasa did not quit, so the install was stopped before replacing its files. "
            f"Still running: {', '.join(map(str, sorted(still_running)))}"
        )
    return True


def launch_local_copy(binary: Path, background: bool) -> None:
    command = [str(binary)] + (["--background"] if background else [])
    print(f"Launching {binary}" + (" in the background" if background else ""), flush=True)
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    running = wait_for_local_processes(binary, present=True, timeout=10)
    if process.poll() is not None or process.pid not in running:
        raise SystemExit("Local Licasa did not start after installation.")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Install a built Licasa tree into ~/.local and refresh desktop metadata."
    )
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local")
    parser.add_argument("--launch", action="store_true")
    args = parser.parse_args()

    build_dir = args.build_dir.expanduser().resolve()
    prefix = args.prefix.expanduser().resolve()

    if not build_dir.is_dir() or not (build_dir / "CMakeCache.txt").is_file():
        raise SystemExit(f"Not a configured CMake build directory: {build_dir}")
    if os.geteuid() == 0:
        raise SystemExit("Refusing to deploy the local development build as root.")

    binary = prefix / "bin" / "licasa"
    was_running = stop_running_local_copy(binary)

    prefix.mkdir(parents=True, exist_ok=True)
    run(["cmake", "--install", str(build_dir), "--prefix", str(prefix)])

    if not binary.is_file():
        raise SystemExit(f"Install completed but the Licasa binary is missing: {binary}")

    desktop_dir = prefix / "share" / "applications"
    desktop_file = desktop_dir / "licasa.desktop"
    if desktop_file.is_file():
        # A Snap install adds a second Licasa launcher. Make the development
        # build explicit and pin it to this prefix so the two are distinguishable.
        entries = desktop_file.read_text(encoding="utf-8").splitlines()
        replacements = {
            "Name": "Licasa (Local)",
            "TryExec": str(binary),
            "Exec": f'"{binary}" %f',
        }
        updated = []
        for entry in entries:
            key = entry.partition("=")[0]
            updated.append(f"{key}={replacements[key]}" if key in replacements else entry)
        desktop_file.write_text("\n".join(updated) + "\n", encoding="utf-8")

    update_desktop_database = shutil.which("update-desktop-database")
    if update_desktop_database and desktop_dir.is_dir():
        run([update_desktop_database, str(desktop_dir)])

    print(f"\nLicasa deployed locally: {binary}")
    if args.launch or was_running:
        launch_local_copy(binary, background=not args.launch)


if __name__ == "__main__":
    main()
