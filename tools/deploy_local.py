#!/usr/bin/env python3
"""Install an already-built Licasa tree into the current user's local prefix."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path


def run(command: list[str]) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


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

    prefix.mkdir(parents=True, exist_ok=True)
    run(["cmake", "--install", str(build_dir), "--prefix", str(prefix)])

    desktop_dir = prefix / "share" / "applications"
    update_desktop_database = shutil.which("update-desktop-database")
    if update_desktop_database and desktop_dir.is_dir():
        run([update_desktop_database, str(desktop_dir)])

    binary = prefix / "bin" / "licasa"
    if not binary.is_file():
        raise SystemExit(f"Install completed but the Licasa binary is missing: {binary}")

    print(f"\nLicasa deployed locally: {binary}")
    if args.launch:
        print(f"Launching {binary}")
        subprocess.Popen(
            [str(binary)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )


if __name__ == "__main__":
    main()
