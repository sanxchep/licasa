#!/usr/bin/env python3
"""Run Python or native desktop-user tests with real kernel credentials."""

import os
from pathlib import Path
import pwd
import subprocess
import sys
import tempfile


def main():
    arguments = sys.argv[1:]
    native = arguments[:1] == ["--exec"]
    command = arguments[1:] if native else [sys.executable, *arguments]
    if not arguments or not command:
        raise SystemExit("Expected Python test arguments or --exec followed by an executable")
    if os.geteuid() != 0:
        os.execv(command[0], command)

    # Native builders compile/package as root. Only this test child changes
    # user; do not bypass the installer's root refusal or mock /proc identity.
    user = pwd.getpwnam("nobody")
    with tempfile.TemporaryDirectory(prefix="lc-test-user-", dir="/tmp") as temporary:
        home = Path(temporary)
        runtime = home / "runtime"
        runtime.mkdir(mode=0o700)
        for directory in (home, runtime):
            os.chown(directory, user.pw_uid, user.pw_gid)
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("XDG_", "DBUS_", "SUDO_"))}
        # Keep fixture paths short enough for Linux's Unix-socket path limit.
        environment.update(HOME=str(home), TMPDIR="/tmp", XDG_RUNTIME_DIR=str(runtime),
                           USER=user.pw_name, LOGNAME=user.pw_name, PYTHONDONTWRITEBYTECODE="1")
        print(f"Running desktop-user tests as uid={user.pw_uid}, gid={user.pw_gid}", flush=True)
        result = subprocess.run(command, cwd=home, env=environment,
                                user=user.pw_uid, group=user.pw_gid, extra_groups=[])
    raise SystemExit(result.returncode if result.returncode >= 0 else 128 - result.returncode)


if __name__ == "__main__":
    main()
