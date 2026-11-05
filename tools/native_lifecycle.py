#!/usr/bin/python3 -I
"""Root-owned native-package lifecycle adapter; never executes a user's program.

Incoming pre-install scripts embed this file so the old package need not have
installed a cooperative helper. Missing/legacy control requires full Quit.
Native partial transactions are repaired by their package manager, not rollback.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
import sys
import time


LIMIT = 16384
CONFIGURATIONS = {
    "native": {
        "executables": ("/usr/bin/licasa", "/usr/bin/licasa.bin"),
        "binary": "/usr/bin/licasa.bin",
        "launcher": "/usr/bin/licasa",
        "helper": "/usr/lib/licasa/native_lifecycle.py",
        "receipt": "/usr/share/licasa/native-runtime.json",
    },
    "appimage-native": {
        "executables": ("/opt/licasa/usr/bin/licasa",),
        "binary": "/opt/licasa/usr/bin/licasa",
        "launcher": "/opt/licasa/AppRun",
        "helper": "/opt/licasa/usr/libexec/licasa/native_lifecycle.py",
        "receipt": "/opt/licasa/usr/share/licasa/native-runtime.json",
    },
}


class NativeLifecycleError(RuntimeError):
    pass


def json_object(contents: str | bytes, message: str) -> dict:
    """Keep the embedded adapter self-contained and reject non-object JSON."""
    try:
        value = json.loads(contents)
    except (ValueError, UnicodeError) as error:
        raise NativeLifecycleError(message) from error
    if not isinstance(value, dict):
        raise NativeLifecycleError(message)
    return value


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            checksum.update(block)
    return checksum.hexdigest()


def trusted(path: Path, *, directory: bool = False) -> os.stat_result:
    info = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise NativeLifecycleError(f"Unsafe package-owned {'directory' if directory else 'file'}: {path}")
    return info


def start_identity(pid: int) -> str:
    try:
        record = Path(f"/proc/{pid}/stat").read_text()
        fields = record[record.rfind(")") + 2:].split()
        if fields[0] == "Z":
            return ""
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        return f"{boot}:{fields[19]}"
    except (FileNotFoundError, ProcessLookupError):
        return ""


def is_headless_control(command_line: bytes) -> bool:
    if not command_line or len(command_line) > 65536 or not command_line.endswith(b"\0"):
        return False
    arguments = command_line[:-1].split(b"\0")
    if not arguments[0]:
        return False
    if len(arguments) == 2:
        return arguments[1] == b"--quit"
    return (len(arguments) == 3 and arguments[1] == b"--lifecycle" and
            arguments[2] in {b"identity", b"status", b"health", b"prepare-update",
                             b"stop-for-update", b"cancel-update", b"quit"})


def wait_for_control_exit(pid: int, started: str, timeout: float = 2.0) -> None:
    # A user's argv is mutable. Root never treats a claimed control invocation
    # as proof of absence: it must observe this exact process actually exit.
    deadline = time.monotonic() + timeout
    while start_identity(pid) == started:
        if time.monotonic() >= deadline:
            raise NativeLifecycleError("A native lifecycle control client has not exited; retry the package transaction shortly.")
        time.sleep(0.02)


def processes(installation: str) -> list[dict]:
    expected = CONFIGURATIONS[installation]["executables"]
    result = []
    for directory in Path("/proc").iterdir():
        if not directory.name.isdecimal():
            continue
        try:
            executable = os.readlink(directory / "exe").removesuffix(" (deleted)")
            if executable not in expected:
                continue
            pid = int(directory.name)
            started = start_identity(pid)
            if not started:
                continue
            status = (directory / "status").read_text()
            credentials = dict(line.split(":", 1) for line in status.splitlines() if ":" in line)
            uid = int(credentials["Uid"].split()[1])
            gid = int(credentials["Gid"].split()[1])
            info = (directory / "exe").stat()
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
                raise NativeLifecycleError(f"Process {pid} is not running a trusted native payload.")
            with (directory / "cmdline").open("rb") as command_line:
                if is_headless_control(command_line.read(65537)):
                    wait_for_control_exit(pid, started)
                    continue
            environment_bytes = (directory / "environ").read_bytes()
            if len(environment_bytes) > 131072:
                raise NativeLifecycleError(f"Cannot inspect process {pid}'s oversized environment.")
            environment = dict(item.split(b"=", 1) for item in environment_bytes.split(b"\0") if b"=" in item)
            runtime = os.fsdecode(environment.get(b"XDG_RUNTIME_DIR", f"/run/user/{uid}".encode()))
            family, anchor = ("appimage", "/opt/licasa") if installation == "appimage-native" else ("filesystem", executable)
            identity = hashlib.sha256(f"{uid}\n{family}\n{anchor}".encode()).hexdigest()[:32]
            result.append({"pid": pid, "startIdentity": started, "uid": uid, "gid": gid,
                           "executable": executable, "generation": digest(directory / "exe"),
                           "executableDevice": info.st_dev, "executableInode": info.st_ino,
                           "installationId": identity, "runtime": runtime})
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError as error:
            # comm and argv are user-mutable and cannot establish that a denied
            # process is unrelated. Root must prove the executable identity.
            raise NativeLifecycleError(
                f"Cannot independently inspect process {directory.name}; native process inspection is unavailable. "
                "Retry with package-manager privileges that can inspect all users' executables. "
                "No payload replacement is authorized.") from error
    return result


def private_runtime(path: Path, uid: int) -> None:
    if not path.is_absolute():
        raise NativeLifecycleError("The viewer has no absolute private runtime directory.")
    current = Path("/")
    for part in path.parts[1:]:
        current /= part
        info = current.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise NativeLifecycleError("The runtime directory contains a symlink or non-directory.")
        if current == path:
            if info.st_uid != uid or stat.S_IMODE(info.st_mode) != 0o700:
                raise NativeLifecycleError("The viewer runtime directory is not private.")
        elif info.st_mode & 0o022 and not (info.st_mode & stat.S_ISVTX and info.st_uid == 0):
            raise NativeLifecycleError("The viewer runtime directory has an untrusted writable ancestor.")


def user_request(process: dict, operation: str) -> dict:
    """Run only trusted Python code after dropping credentials; never a viewer."""
    runtime = Path(process["runtime"])
    def revalidate_executable(*, stopped: bool = False) -> None:
        current_start = start_identity(process["pid"])
        if current_start != process["startIdentity"]:
            if stopped:
                return
            raise NativeLifecycleError("The predecessor exited or changed start identity during control.")
        try:
            executable = Path(f"/proc/{process['pid']}/exe").stat()
        except (FileNotFoundError, ProcessLookupError):
            if stopped and start_identity(process["pid"]) != process["startIdentity"]:
                return
            raise NativeLifecycleError("The predecessor executable disappeared during control.")
        if (executable.st_dev != process["executableDevice"] or
                executable.st_ino != process["executableInode"]):
            raise NativeLifecycleError("The predecessor executed another program during control.")
    revalidate_executable()
    private_runtime(runtime, process["uid"])
    endpoint = runtime / f"licasa-lc-{process['installationId']}.sock"
    info = endpoint.lstat()
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != process["uid"] or info.st_mode & 0o077:
        raise NativeLifecycleError("The lifecycle endpoint is not an owned private socket.")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2.0)
        connection.connect(str(endpoint))
        pid, uid, _ = struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if pid != process["pid"] or uid != process["uid"] or start_identity(pid) != process["startIdentity"]:
            raise NativeLifecycleError("The lifecycle listener is not the independently identified predecessor.")
        command = {"protocol": 1, "installationId": process["installationId"], "operation": operation}
        connection.sendall(json.dumps(command, separators=(",", ":")).encode() + b"\n")
        data = bytearray()
        while b"\n" not in data:
            fragment = connection.recv(min(4096, LIMIT + 1 - len(data)))
            if not fragment:
                raise NativeLifecycleError("The predecessor closed without a lifecycle receipt.")
            data.extend(fragment)
            if len(data) > LIMIT:
                raise NativeLifecycleError("The predecessor sent an oversized lifecycle receipt.")
        reply = json_object(data.split(b"\n", 1)[0], "The predecessor returned an invalid lifecycle receipt.")
        if (reply.get("protocol") != 1 or reply.get("installationId") != process["installationId"]
                or reply.get("pid") != process["pid"] or reply.get("startIdentity") != process["startIdentity"]
                or reply.get("generation") != process["generation"]):
            raise NativeLifecycleError("The lifecycle receipt does not identify the predecessor.")
        revalidate_executable(stopped=operation == "stop-for-update" and reply.get("result") == "accepted")
        return reply


def dropped_request(process: dict, operation: str) -> dict:
    reader, writer = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(reader)
        try:
            os.setgroups([])
            os.setgid(process["gid"])
            os.setuid(process["uid"])
            reply = user_request(process, operation)
        except Exception as error:
            reply = {"result": "unavailable", "message": str(error)}
        encoded = json.dumps(reply, separators=(",", ":")).encode()
        try:
            with os.fdopen(writer, "wb") as stream:
                stream.write(encoded[:LIMIT])
        finally:
            os._exit(0)
    os.close(writer)
    with os.fdopen(reader, "rb") as stream:
        data = stream.read(LIMIT + 1)
    _, status = os.waitpid(child, 0)
    if status != 0 or len(data) > LIMIT:
        raise NativeLifecycleError("The per-user lifecycle check failed.")
    return json_object(data, "The per-user lifecycle check returned no valid receipt.")


class NativeGate:
    def __init__(self, installation: str):
        self.installation = installation
        self.directory = Path("/var/lib/licasa/lifecycle") / installation
        self.lock = None

    def __enter__(self):
        if os.geteuid() != 0:
            raise NativeLifecycleError("Native lifecycle preparation must run through the package manager as root.")
        current = Path("/")
        for part in self.directory.parts[1:]:
            current /= part
            if not current.exists() and not current.is_symlink():
                current.mkdir(mode=0o755)
                # Package-manager invocations may inherit a restrictive umask;
                # the protected metadata path must remain traversable by users.
                current.chmod(0o755)
            trusted(current, directory=True)
        descriptor = os.open(self.directory / "lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
        self.lock = os.fdopen(descriptor, "rb+")
        try:
            trusted(self.directory / "lock")
            os.fchmod(descriptor, 0o644)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise NativeLifecycleError("A Licasa launch or package transaction is in progress; retry shortly.") from error
        except BaseException:
            self.lock.close()
            self.lock = None
            raise
        return self

    def __exit__(self, *unused):
        if self.lock:
            self.lock.close()
            self.lock = None

    def read(self) -> dict:
        path = self.directory / "transaction.json"
        if not path.exists() and not path.is_symlink():
            return {}
        trusted(path)
        if path.stat().st_size > LIMIT:
            raise NativeLifecycleError("The native lifecycle journal is oversized.")
        journal = json_object(path.read_bytes(), "The native lifecycle journal is invalid; use package-manager repair.")
        if journal.get("installation") != self.installation:
            raise NativeLifecycleError("The lifecycle journal belongs to another installation.")
        return journal

    def atomic(self, name: str, content: bytes) -> None:
        path = self.directory / name
        if path.exists() or path.is_symlink():
            trusted(path)
        temporary = self.directory / f".{name}.{os.getpid()}.tmp"
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
        try:
            os.fchmod(descriptor, 0o644)
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
            directory = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            temporary.unlink(missing_ok=True)

    def record(self, state: str, **details) -> None:
        record = {"schema": 1, "installation": self.installation, "state": state,
                  "updatedAt": int(time.time()), **details}
        encoded = (json.dumps(record, sort_keys=True) + "\n").encode()
        if len(encoded) > LIMIT:
            raise NativeLifecycleError("The native transaction has too many predecessors for a bounded recovery record.")
        self.atomic("state", ("verified\n" if state == "verified" else "blocked\n").encode())
        self.atomic("transaction.json", encoded)


def prepare(gate: NativeGate, action: str) -> dict:
    previous = gate.read()
    targets = processes(gate.installation)
    prepared = []
    try:
        for process in targets:
            reply = dropped_request(process, "status")
            if reply.get("result") != "ready":
                raise NativeLifecycleError(f"Process {process['pid']} has no safe lifecycle protocol. Fully Quit this installation for the first upgrade/removal, then retry.")
            if reply.get("visibleWindows", 1) != 0 or reply.get("busyReason"):
                raise NativeLifecycleError(f"Process {process['pid']} has visible or busy work. Finish it and fully Quit Licasa, then retry.")
        for process in targets:
            reply = dropped_request(process, "prepare-update")
            if reply.get("result") != "ready":
                raise NativeLifecycleError(f"Process {process['pid']} became busy; payload replacement was not started.")
            prepared.append(process)
        # Persist a launch block before any predecessor is stopped. A package
        # manager crash after this point requires its repair/reinstall route.
        gate.record("prepared", action=action, predecessors=[{key: value for key, value in target.items() if key not in {"runtime", "gid"}} for target in targets])
        for process in targets:
            reply = dropped_request(process, "stop-for-update")
            if reply.get("result") not in {"ready", "accepted"}:
                raise NativeLifecycleError(f"Process {process['pid']} did not accept a safe stop.")
        deadline = time.monotonic() + 10.0
        while any(start_identity(process["pid"]) == process["startIdentity"] for process in targets):
            if time.monotonic() >= deadline:
                raise NativeLifecycleError("A stop was accepted but the predecessor did not exit. No runtime replacement is authorized.")
            time.sleep(0.02)
        if processes(gate.installation):
            raise NativeLifecycleError("Another predecessor appeared during preparation; fully Quit it and retry.")
        gate.record("stopped", action=action, previousGeneration=previous.get("generation", previous.get("previousGeneration", "")),
                    deferredStart="next-login-or-explicit-user-launch")
        return {"result": "ready", "stopped": len(targets), "deferredStart": True}
    except Exception:
        for process in prepared:
            if start_identity(process["pid"]) == process["startIdentity"]:
                dropped_request(process, "cancel-update")
        # A failed preflight has not changed package files. Keep an already
        # verified installation usable; interrupted mixed payloads stay blocked.
        if previous.get("state") == "verified":
            gate.record("verified", preparationFailed=True)
        raise


def verify_payload(installation: str) -> dict:
    receipt = Path(CONFIGURATIONS[installation]["receipt"])
    trusted(receipt)
    if receipt.stat().st_size > 4 * 1024 * 1024:
        raise NativeLifecycleError("The installed native receipt is oversized.")
    record = json_object(receipt.read_bytes(), "The installed native receipt is invalid.")
    if record.get("schema") != 1 or record.get("installation") != installation:
        raise NativeLifecycleError("The installed native receipt is incompatible.")
    files = record.get("files")
    if not isinstance(files, dict) or not files or len(files) > 10000:
        raise NativeLifecycleError("The installed native receipt has no bounded file inventory.")
    for name, expected in files.items():
        if not isinstance(expected, dict) or expected.get("type") not in ("file", "symlink"):
            raise NativeLifecycleError("The native receipt contains an invalid file record.")
        path = Path(name)
        allowed = name.startswith("/usr/") if installation == "native" else (name.startswith("/opt/licasa/") or name == "/usr/bin/licasa" or name.startswith("/usr/share/"))
        if not allowed or ".." in path.parts:
            raise NativeLifecycleError("The native receipt contains an invalid package path.")
        info = path.lstat()
        if info.st_uid != 0 or (not stat.S_ISLNK(info.st_mode) and info.st_mode & 0o022):
            raise NativeLifecycleError(f"The installed package file is not trusted: {path}")
        if expected.get("type") == "symlink":
            if not stat.S_ISLNK(info.st_mode) or os.readlink(path) != expected.get("target"):
                raise NativeLifecycleError(f"Installed symlink does not match its receipt: {path}")
        elif not stat.S_ISREG(info.st_mode) or digest(path) != expected.get("sha256"):
            raise NativeLifecycleError(f"Installed file does not match its receipt: {path}")
        elif stat.S_IMODE(info.st_mode) != expected.get("mode"):
            raise NativeLifecycleError(f"Installed file permissions do not match its receipt: {path}")
    binary = Path(CONFIGURATIONS[installation]["binary"])
    trusted(binary)
    with binary.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise NativeLifecycleError("The native viewer is not an ELF executable.")
    return {"generation": digest(binary), "filesVerified": len(files)}


def launcher_text(installation: str) -> str:
    binary = CONFIGURATIONS[installation]["binary"]
    target = binary if installation == "native" else "/opt/licasa/AppRun.lifecycle-original"
    environment = ("unset APPIMAGE APPDIR SNAP SNAP_NAME SNAP_INSTANCE_NAME SNAP_REVISION SNAP_VERSION SNAP_USER_DATA SNAP_USER_COMMON\n"
                   + ("" if installation == "native" else "export APPDIR=/opt/licasa\n"))
    return f'''#!/bin/bash
# Package-owned launcher: hold the gate before loading a replaceable ELF/runtime.
set -eu
gate=/var/lib/licasa/lifecycle/{installation}
for ancestor in /var /var/lib /var/lib/licasa /var/lib/licasa/lifecycle "$gate"; do
    if [[ ! -d "$ancestor" || -L "$ancestor" || $(/usr/bin/stat -c '%u' "$ancestor") != 0 ]]; then
        echo 'The native Licasa update path is unsafe; repair the package.' >&2
        exit 1
    fi
    permissions=$(/usr/bin/stat -c '%a' "$ancestor")
    if (( (8#$permissions & 8#022) != 0 )); then
        echo 'The native Licasa update path is writable by another account.' >&2
        exit 1
    fi
done
if [[ ! -d "$gate" || -L "$gate" || ! -f "$gate/lock" || -L "$gate/lock" || ! -f "$gate/state" || -L "$gate/state" ]]; then
    echo 'Licasa needs package repair/reconfiguration before launch.' >&2
    exit 1
fi
if [[ $(/usr/bin/stat -c '%u:%a' "$gate") != 0:755 || $(/usr/bin/stat -c '%u:%a' "$gate/lock") != 0:644 || $(/usr/bin/stat -c '%u:%a' "$gate/state") != 0:644 ]]; then
    echo 'The native Licasa update gate is unsafe; repair the package.' >&2
    exit 1
fi
exec 198<"$gate/lock"
if ! /usr/bin/flock -sn 198; then
    echo 'Licasa is being updated. Retry after the package transaction.' >&2
    exit 1
fi
IFS= read -r state < "$gate/state"
if [[ "$state" != verified ]]; then
    echo 'An interrupted Licasa transaction needs package-manager repair.' >&2
    exit 1
fi
export LICASA_LAUNCH_GATE_FD=198
export LICASA_NATIVE_INSTALLATION={installation}
{environment}exec {target} "$@"
'''


def stage_payload(destination: Path, installation: str) -> None:
    """Called only by build tools; records every installed file/link, not caches."""
    configuration = CONFIGURATIONS[installation]
    helper = destination / configuration["helper"].lstrip("/")
    helper.parent.mkdir(parents=True, exist_ok=True)
    helper.write_bytes(Path(__file__).read_bytes())
    helper.chmod(0o755)
    launcher = destination / configuration["launcher"].lstrip("/")
    launcher.parent.mkdir(parents=True, exist_ok=True)
    launcher.write_text(launcher_text(installation))
    launcher.chmod(0o755)
    if installation == "appimage-native":
        outer = destination / "usr/bin/licasa"
        outer.parent.mkdir(parents=True, exist_ok=True)
        outer.write_text(launcher_text(installation))
        outer.chmod(0o755)
    receipt = destination / configuration["receipt"].lstrip("/")
    files = {}
    for path in sorted(destination.rglob("*")):
        if path == receipt or "DEBIAN" in path.relative_to(destination).parts:
            continue
        name = "/" + path.relative_to(destination).as_posix()
        if path.is_symlink():
            files[name] = {"type": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            files[name] = {"type": "file", "sha256": digest(path),
                           "mode": stat.S_IMODE(path.stat().st_mode)}
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps({"schema": 1, "installation": installation, "files": files}, sort_keys=True) + "\n")
    receipt.chmod(0o644)


def embedded_script(installation: str, operation: str, action: str = "upgrade") -> str:
    """The incoming manager executes this trusted source, never an old binary."""
    return ("#!/bin/sh\nset -eu\n"
            "if [ ! -x /usr/bin/python3 ]; then\n"
            "  echo 'Licasa lifecycle requires python3 before package replacement. Install that prerequisite, fully Quit Licasa, and retry.' >&2\n"
            "  exit 1\nfi\n"
            f"/usr/bin/python3 -I - --installation {installation} {operation} --action {action} <<'LICASA_NATIVE_PYTHON'\n"
            + Path(__file__).read_text() + "\nLICASA_NATIVE_PYTHON\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installation", choices=tuple(CONFIGURATIONS), required=True)
    parser.add_argument("operation", choices=("prepare", "complete", "removed", "purge", "status"))
    parser.add_argument("--action", choices=("install", "upgrade", "remove", "transaction"), default="upgrade")
    args = parser.parse_args()
    try:
        with NativeGate(args.installation) as gate:
            if args.operation == "prepare":
                result = prepare(gate, args.action)
            elif args.operation == "complete":
                try:
                    if processes(args.installation):
                        raise NativeLifecycleError("A predecessor is still running; package configuration cannot report a verified upgrade.")
                    result = verify_payload(args.installation)
                except (NativeLifecycleError, OSError, ValueError, KeyError) as error:
                    gate.record("recovery-needed", message=str(error))
                    raise
                gate.record("verified", **result, deferredStart="next-login-or-explicit-user-launch")
                result["result"] = "verified"
            elif args.operation in {"removed", "purge"}:
                if processes(args.installation):
                    raise NativeLifecycleError("A predecessor is still running; removal is not complete.")
                if Path(CONFIGURATIONS[args.installation]["binary"]).exists():
                    raise NativeLifecycleError("Native runtime files remain; use package-manager repair.")
                if args.operation == "purge":
                    # Purge retires root transaction history. Per-user consent,
                    # photos and settings are deliberately outside root cleanup.
                    gate.record("purged")
                    result = {"result": "purged", "userPreferencesRetained": True}
                else:
                    previous = gate.read()
                    gate.record("removed", previousGeneration=previous.get("previousGeneration", ""),
                                deferredStartupCleanup=True)
                    result = {"result": "removed", "deferredStartupCleanup": True}
            else:
                result = gate.read()
            print(json.dumps(result, sort_keys=True))
            return 0
    except (NativeLifecycleError, OSError, ValueError, KeyError, IndexError) as error:
        print(f"Licasa native lifecycle: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
