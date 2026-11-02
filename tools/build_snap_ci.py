#!/usr/bin/env python3
"""Build a Snap in a disposable hosted VM with bounded Store retries."""

import argparse
from functools import partial
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess

from retry_snap_store import execute, interrupt_on_term, retry


def build(root: Path, *, run=subprocess.run, retry_command=retry, executable=shutil.which):
    if os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        raise ValueError("Snap construction requires a disposable hosted runner")
    user = pwd.getpwuid(os.getuid()).pw_name
    reports = root / "build/snap-store-retries"
    budget = Path(os.environ["RUNNER_TEMP"]) / "snap-store-retry-budget.json"

    def checked(command):
        run(command, check=True, cwd=root)

    def store(label, command, **options):
        code = retry_command(command, budget_file=budget,
                             report=reports / (label + ".json"),
                             runner=partial(execute, cwd=root), **options)
        if code:
            raise subprocess.CalledProcessError(code, command)

    if not executable("snap"):
        checked(["sudo", "apt-get", "update", "-q"])
        checked(["sudo", "apt-get", "install", "-qy", "snapd"])
    identity = Path("/").stat()
    if identity.st_uid != 0 or identity.st_gid != 0:
        checked(["sudo", "chown", "root:root", "/"])
    if Path("/usr/bin/lxd").exists():
        checked(["sudo", "apt-get", "remove", "-qy", "lxd", "lxd-client"])
    checked(["sudo", "groupadd", "--force", "--system", "lxd"])
    checked(["sudo", "usermod", "--append", "--groups", "lxd", user])
    store("lxd", ["sudo", "snap", "refresh" if executable("lxd") else "install", "lxd"])
    checked(["sudo", "lxd", "init", "--auto"])
    # Preserve the hosted action's existing Docker/LXD bridge compatibility.
    checked(["sudo", "iptables", "-P", "FORWARD", "ACCEPT"])
    store("snapcraft", ["sudo", "snap", "refresh" if executable("snapcraft") else "install",
                        "--channel=stable", "--classic", "snapcraft"])
    os.environ["SNAPCRAFT_BUILD_ENVIRONMENT"] = "lxd"
    os.environ["SNAPCRAFT_BUILD_INFO"] = "1"
    os.environ["SNAPCRAFT_IMAGE_INFO"] = json.dumps({
        "build_url": f"https://github.com/{os.environ['GITHUB_REPOSITORY']}/actions/runs/"
                     f"{os.environ['GITHUB_RUN_ID']}"})
    # A normal full-source build keeps its existing 180-minute allowance. Only
    # a transient Store failure opens the shared 30-minute retry window.
    store("build", ["sudo", "-u", user, "-E", "snapcraft"], initial_timeout=10800)
    snaps = sorted(root.glob("*.snap"))
    if len(snaps) != 1:
        raise ValueError(f"Expected exactly one built Snap, found {len(snaps)}")
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as output:
        output.write(f"snap={snaps[0]}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    interrupt_on_term()
    try:
        build(args.root.resolve())
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
