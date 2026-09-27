#!/usr/bin/env python3
"""Check (or explicitly format) first-party C++ with the project's LLVM-based style."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--formatter", default="clang-format-18")
    parser.add_argument("--fix", action="store_true", help="Apply formatting instead of checking")
    args = parser.parse_args()
    formatter = shutil.which(args.formatter)
    if not formatter:
        parser.error(f"{args.formatter} is unavailable; install clang-format-18 or pass --formatter")
    files = sorted(path for directory in ("src", "tests", "tools")
                   for path in (ROOT / directory).rglob("*") if path.suffix in (".cpp", ".h"))
    options = ["-i"] if args.fix else ["--dry-run", "--Werror"]
    subprocess.run([formatter, *options, *map(str, files)], cwd=ROOT, check=True)
    print(f"{'Formatted' if args.fix else 'Checked'} {len(files)} first-party C++ files.")


if __name__ == "__main__":
    main()
