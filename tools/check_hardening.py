#!/usr/bin/env python3
"""Fail a package check unless an ELF executable has PIE, NX, RELRO and NOW."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    result = subprocess.run(["readelf", "-W", "-h", "-l", "-d", str(args.executable)],
                            check=True, text=True, capture_output=True).stdout
    stack = next((line.split() for line in result.splitlines() if "GNU_STACK" in line), [])
    checks = {
        "pie": bool(re.search(r"Type:\s+DYN", result)) and "PIE" in result,
        "non_executable_stack": bool(stack) and "E" not in "".join(stack[6:-1]),
        "relro": "GNU_RELRO" in result,
        "immediate_binding": "BIND_NOW" in result or bool(re.search(r"FLAGS_1.*\bNOW\b", result)),
    }
    print(json.dumps({"path": str(args.executable), "checks": checks,
                      "sha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}))
    if not all(checks.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
