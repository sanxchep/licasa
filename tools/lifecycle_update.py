#!/usr/bin/env python3
"""Unprivileged installation-scoped lifecycle coordination and recovery journal.

This component never downloads packages, signals a legacy viewer, or launches a
graphical session. Package managers need separate transaction adapters.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time


LIMIT = 16384
JOURNAL_LIMIT = 1024 * 1024
STATES = {"preflight", "prepared", "stopped", "replacing", "committed",
          "verified", "recovery-needed"}


class LifecycleError(RuntimeError):
    pass


def json_object(contents: str | bytes, message: str) -> dict:
    """Decode protocol and recovery objects through the same error boundary."""
    try:
        value = json.loads(contents)
    except (ValueError, UnicodeError) as error:
        raise LifecycleError(message) from error
    if not isinstance(value, dict):
        raise LifecycleError(message)
    return value


def absolute(path: Path | str) -> Path:
    # Keep stable installation spelling; resolving a generation symlink loses
    # the lineage used by the viewer and its predecessor.
    return Path(os.path.abspath(os.fspath(path)))


def generation(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            checksum.update(block)
    return checksum.hexdigest()


def installation_id(binary: Path, root: Path, family: str) -> str:
    anchor = root if family == "appimage" else binary
    return hashlib.sha256(f"{os.geteuid()}\n{family}\n{absolute(anchor)}".encode()).hexdigest()[:32]


def start_identity(pid: int, proc: Path = Path("/proc")) -> str:
    contents = (proc / str(pid) / "stat").read_text()
    # comm is parenthesized and can itself contain spaces or parentheses.
    fields = contents[contents.rfind(")") + 2:].split()
    started = fields[19]
    boot = (proc / "sys/kernel/random/boot_id").read_text().strip()
    return f"{boot}:{started}"


class ProcessInspection(dict):
    """Positive installation matches plus explicitly unclassified processes."""

    def __init__(self):
        super().__init__()
        self.unknown_process_count = 0

    def metadata(self) -> dict:
        return {"inspectionScope": "installation", "unknownProcessCount": self.unknown_process_count,
                "globalAbsenceProven": False}


def owned_processes(binary: Path, proc: Path = Path("/proc"), *, strict: bool = False,
                    known_receipts: dict | None = None) -> ProcessInspection:
    result = ProcessInspection()
    expected = str(absolute(binary))
    receipts = {str(pid): str(value) for pid, value in (known_receipts or {}).items()}
    for directory in proc.iterdir():
        if not directory.name.isdecimal():
            continue
        positive = False
        try:
            if directory.name in receipts:
                try:
                    positive = start_identity(int(directory.name), proc) == receipts[directory.name]
                except (PermissionError, ValueError, IndexError):
                    # A previously authenticated PID/start receipt is positive
                    # evidence until exit or a different start is established.
                    positive = True
            # A nondumpable process's /proc directory can be root-owned even
            # while its kernel credentials still belong to the desktop user.
            # comm and argv are mutable and cannot establish an exclusion.
            status = (directory / "status").read_text()
            credentials = next(line for line in status.splitlines() if line.startswith("Uid:"))
            if not positive and os.geteuid() not in {int(value) for value in credentials.split()[1:]}:
                continue
            process_stat = (directory / "stat").read_text()
            if process_stat[process_stat.rfind(")") + 2:].split()[0] == "Z":
                # A reaping parent may retain /proc after all owned runtime
                # resources have exited. It cannot serve another launch.
                continue
            executable = os.readlink(directory / "exe").removesuffix(" (deleted)")
            matches = positive or executable == expected
            positive = matches
            if not matches and binary.parts[-3:] == ("usr", "bin", "licasa"):
                managed_root = absolute(binary).parent.parent.parent
                backup_prefix = str(managed_root.parent / ".licasa-appimage.")
                if executable.startswith(backup_prefix) and executable.endswith("/previous-app/usr/bin/licasa"):
                    positive = True
                    # Historical installers renamed/deleted the prior runtime
                    # while it remained alive. Its /proc path follows that
                    # rename; its stable APPDIR still establishes lineage.
                    environment = (directory / "environ").read_bytes().split(b"\0")
                    matches = os.fsencode(f"APPDIR={managed_root}") in environment
            if matches:
                result[int(directory.name)] = start_identity(int(directory.name), proc)
        except (FileNotFoundError, ProcessLookupError):
            continue
        except (PermissionError, StopIteration, ValueError, IndexError) as error:
            # Mutable command/environment bytes may add a positive blocker;
            # their absence never identifies a process as unrelated.
            try:
                positive = positive or os.fsencode(expected) in (directory / "cmdline").read_bytes().split(b"\0")
            except OSError:
                pass
            if binary.parts[-3:] == ("usr", "bin", "licasa"):
                try:
                    positive = positive or os.fsencode(f"APPDIR={absolute(binary).parent.parent.parent}") in (
                        directory / "environ").read_bytes().split(b"\0")
                except OSError:
                    pass
            if positive or strict:
                raise LifecycleError(
                    f"Process inspection is unavailable for PID {directory.name}; "
                    "cannot establish the required installation identity. No files were replaced. "
                    "Resolve the inspection restriction before retrying.") from error
            # This is unknown, never evidence of an unrelated process or of
            # global absence. Scoped policy permits the transaction subject to
            # its separate legacy full-Quit prerequisite and endpoint checks.
            result.unknown_process_count += 1
    return result


def _private(path: Path, directory: bool = False) -> os.stat_result:
    info = path.lstat()
    expected = stat.S_ISDIR if directory else stat.S_ISREG
    if not expected(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise LifecycleError(f"Unsafe lifecycle {'directory' if directory else 'file'}: {path}")
    return info


def protected_ancestry(path: Path, allow_private_shield: bool = False) -> None:
    """Check trusted ancestry, preserving inner paths shielded by a private home."""
    path = absolute(path)
    if path.resolve() != path:
        raise LifecycleError(f"Symlinked lifecycle ancestry is unsafe: {path}")
    trusted = {0, os.geteuid(), Path("/").stat().st_uid}
    shielded = False
    ancestry = list(reversed(path.parents))
    for index, parent in enumerate(ancestry):
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid not in trusted:
            raise LifecycleError(f"Untrusted lifecycle ancestor: {parent}")
        child = ancestry[index + 1] if index + 1 < len(ancestry) else path
        child_owner = child.lstat().st_uid
        sticky = info.st_mode & stat.S_ISVTX and child_owner in trusted
        if info.st_mode & 0o022 and not sticky and not shielded:
            raise LifecycleError(f"Writable lifecycle ancestor is unsafe: {parent}")
        if allow_private_shield and info.st_uid == os.geteuid() and not (info.st_mode & 0o011):
            shielded = True


def control(binary: Path, root: Path, family: str, operation: str,
            timeout: float = 10.0, runtime: Path | None = None) -> dict:
    identity = installation_id(binary, root, family)
    runtime = runtime or Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.geteuid()}"))
    if not runtime.exists():
        return {"result": "absent"}
    _private(runtime, directory=True)
    protected_ancestry(runtime)
    endpoint = runtime / f"licasa-lc-{identity}.sock"
    try:
        info = endpoint.lstat()
    except FileNotFoundError:
        return {"result": "absent"}
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise LifecycleError(f"Unsafe lifecycle endpoint: {endpoint}")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(timeout)
        try:
            connection.connect(str(endpoint))
        except (FileNotFoundError, ConnectionRefusedError):
            return {"result": "absent"}
        credentials = connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        peer_pid, peer_uid, _ = struct.unpack("3i", credentials)
        if peer_uid != os.geteuid():
            raise LifecycleError("Lifecycle endpoint belongs to another user.")
        try:
            peer_exe = os.readlink(f"/proc/{peer_pid}/exe").removesuffix(" (deleted)")
            peer_start = start_identity(peer_pid)
            peer_generation = generation(Path(f"/proc/{peer_pid}/exe"))
        except (OSError, ValueError, IndexError) as error:
            raise LifecycleError("Cannot verify the running viewer's identity.") from error
        if peer_exe != str(absolute(binary)):
            raise LifecycleError("Lifecycle endpoint is not the targeted installation.")
        request = {"protocol": 1, "installationId": identity, "operation": operation}
        connection.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        data = bytearray()
        while b"\n" not in data:
            fragment = connection.recv(min(4096, LIMIT + 1 - len(data)))
            if not fragment:
                raise LifecycleError("Lifecycle endpoint closed without a complete receipt.")
            data.extend(fragment)
            if len(data) > LIMIT:
                raise LifecycleError("Lifecycle reply exceeds the protocol limit.")
        reply = json_object(data.split(b"\n", 1)[0], "Invalid lifecycle reply.")
        if (reply.get("protocol") != 1 or reply.get("installationId") != identity
                or reply.get("pid") != peer_pid or str(reply.get("startIdentity")) != peer_start
                or reply.get("generation") != peer_generation):
            raise LifecycleError("Lifecycle receipt does not match the authenticated running process.")
        return reply


def read_journal(path: Path, root: Path) -> dict | None:
    """Validate the same recovery record for updater and pre-loader admission."""
    try:
        info = _private(path)
    except FileNotFoundError:
        return None
    if info.st_size > JOURNAL_LIMIT:
        raise LifecycleError("Lifecycle journal is too large; retained for manual recovery.")
    value = json_object(path.read_bytes(), "Invalid lifecycle journal; retained for manual recovery.")
    state = value.get("state")
    if value.get("root") != str(root) or not isinstance(state, str) or state not in STATES:
        raise LifecycleError("Lifecycle journal does not match this installation.")
    return value


class InstallationGate:
    def __init__(self, root: Path, inherited_fd: int | None = None,
                 strict_process_inspection: bool | None = None):
        self.root = absolute(root)
        self.directory = self.root.parent / f".{self.root.name}.licasa-lifecycle"
        self.journal = self.directory / "transaction.json"
        self.inherited_fd = inherited_fd
        self.strict_process_inspection = (os.environ.get("LICASA_STRICT_PROCESS_INSPECTION") == "1"
                                          if strict_process_inspection is None else strict_process_inspection)
        self.fd = None

    def __enter__(self):
        if os.geteuid() == 0:
            raise LifecycleError("Per-user lifecycle deployment must run as the desktop user, without sudo.")
        try:
            self.directory.mkdir(mode=0o700)
        except FileExistsError:
            pass
        _private(self.directory, directory=True)
        protected_ancestry(self.directory, allow_private_shield=True)
        lock = self.directory / "lock"
        try:
            if self.inherited_fd is None:
                self.fd = os.open(lock, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            else:
                self.fd = os.dup(self.inherited_fd)
                opened = os.fstat(self.fd)
                linked = lock.lstat()
                if (opened.st_dev, opened.st_ino) != (linked.st_dev, linked.st_ino):
                    raise LifecycleError("Inherited lifecycle lock is not the installation lock.")
            _private(lock)
            try:
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise LifecycleError("Another launch or installation is in progress; retry when it finishes.") from error
        except BaseException:
            if self.fd is not None:
                os.close(self.fd)
            self.fd = None
            raise
        return self

    def __exit__(self, *_):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def read(self) -> dict | None:
        return read_journal(self.journal, self.root)

    def write(self, state: str, **updates) -> dict:
        if state not in STATES:
            raise LifecycleError(f"Invalid transaction state: {state}")
        previous = self.read() or {}
        value = dict(previous, schema=1, root=str(self.root), state=state,
                     updated=time.time(), **updates)
        fd, temporary = tempfile.mkstemp(prefix=".transaction-", dir=self.directory)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(value, stream, sort_keys=True)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.journal)
            directory_fd = os.open(self.directory, os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        finally:
            Path(temporary).unlink(missing_ok=True)
        return value

    def prepare(self, binary: Path, family: str, timeout: float = 10.0, **recovery) -> dict:
        previous = self.read()
        if previous and previous["state"] != "verified":
            raise LifecycleError("An interrupted installation needs recovery before another update.")
        metadata = {"inspectionScope": "installation", "unknownProcessCount": 0,
                    "globalAbsenceProven": False, "legacyFirstQuitRequired": False}
        self.write("preflight", **recovery, **metadata)
        processes = {}
        prepared = False
        try:
            processes = owned_processes(binary, strict=self.strict_process_inspection,
                                        known_receipts=(previous or {}).get("predecessors", {}))
            metadata["unknownProcessCount"] = getattr(processes, "unknown_process_count", 0)
            # Always inspect the targeted endpoint. An inaccessible registered
            # peer must not disappear merely because /proc yielded no match.
            status = control(binary, self.root, family, "prepare-update", timeout)
            if status.get("result") == "absent":
                metadata["legacyFirstQuitRequired"] = binary.exists()
                if processes:
                    raise LifecycleError("A legacy viewer is still running; a one-time full Quit is required before updating.")
            elif status.get("result") not in {"ready", "accepted"}:
                reason = status.get("busyReason") or status.get("result", "unknown")
                raise LifecycleError(f"Licasa cannot stop safely ({reason}). Finish work and fully Quit, then retry.")
            else:
                prepared = True
                if status.get("pid") is not None and status.get("startIdentity"):
                    processes[int(status["pid"])] = str(status["startIdentity"])
            receipts = {str(pid): value for pid, value in processes.items()}
            self.write("prepared", predecessors=receipts, **metadata)
            if prepared:
                stopped = control(binary, self.root, family, "stop-for-update", timeout)
                if stopped.get("result") not in {"ready", "accepted", "absent"}:
                    raise LifecycleError("Licasa refused the update stop; no package files were replaced.")
                deadline = time.monotonic() + timeout
                while True:
                    remaining = owned_processes(binary, strict=self.strict_process_inspection, known_receipts=receipts)
                    metadata["unknownProcessCount"] = max(metadata["unknownProcessCount"],
                                                          getattr(remaining, "unknown_process_count", 0))
                    if not remaining:
                        break
                    if time.monotonic() >= deadline:
                        raise LifecycleError("Licasa has not exited; no package files were replaced. Fully Quit and retry.")
                    time.sleep(0.05)
        except (OSError, LifecycleError) as error:
            if prepared:
                try:
                    control(binary, self.root, family, "cancel-update", timeout)
                except (OSError, LifecycleError):
                    pass
            self.write("verified", outcome="preflight-refused", **metadata)
            raise LifecycleError(str(error)) from error
        self.write("stopped", **metadata)
        return {"stopped": bool(processes), "restart": "deferred", **metadata}


def remove_path(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def fingerprint(path: Path) -> dict | None:
    if path.is_symlink():
        return {"link": os.readlink(path)}
    if path.is_file():
        return {"sha256": generation(path), "mode": stat.S_IMODE(path.stat().st_mode)}
    if path.exists():
        raise LifecycleError(f"Refusing to replace a directory with an application file: {path}")
    return None


def verify_identity(binary: Path, root: Path, family: str, launcher: Path | None = None,
                    gate_fd: int | None = None) -> dict:
    environment = dict(os.environ)
    for variable in ("APPIMAGE", "APPDIR", "LD_PRELOAD", "LD_AUDIT"):
        environment.pop(variable, None)
    for variable in list(environment):
        if variable == "SNAP" or variable.startswith("SNAP_"):
            environment.pop(variable, None)
    if family == "appimage":
        environment["APPDIR"] = str(root)
    inherited = ()
    if gate_fd is not None:
        environment["LICASA_IDENTITY_GATE_FD"] = str(gate_fd)
        inherited = (gate_fd,)
    completed = subprocess.run([str(launcher or binary), "--lifecycle", "identity"],
                               env=environment, capture_output=True, text=True, timeout=10,
                               pass_fds=inherited)
    result = json_object(completed.stdout, "Replacement did not produce a headless identity receipt.")
    if (completed.returncode or result.get("protocol") != 1
            or result.get("generation") != generation(binary)
            or result.get("installationId") != installation_id(binary, root, family)):
        raise LifecycleError("Replacement identity does not match the installed source generation.")
    return result


def inspect_recovery(gate: InstallationGate, journal: dict, binary: Path, family: str) -> None:
    processes = owned_processes(binary, strict=gate.strict_process_inspection,
                                known_receipts=journal.get("predecessors", {}))
    metadata = {"inspectionScope": "installation",
                "unknownProcessCount": getattr(processes, "unknown_process_count", 0),
                "globalAbsenceProven": False}
    gate.write(journal["state"], **metadata)
    status = control(binary, gate.root, family, "status")
    if processes or status.get("result") != "absent":
        raise LifecycleError("Recovery found an owned or registered viewer; fully Quit it before retrying. No recovery files were replaced.")


def recover_appimage(gate: InstallationGate) -> None:
    journal = gate.read()
    if not journal:
        return
    if journal["state"] == "verified":
        cleanup_completed_recovery(gate, journal, ".licasa-appimage.")
        return
    work = absolute(journal.get("recoveryRoot", ""))
    if work.parent != gate.root.parent or not work.name.startswith(".licasa-appimage."):
        raise LifecycleError("Interrupted transaction has no safe AppImage recovery record.")
    _private(work, directory=True)
    if journal["state"] in {"replacing", "committed", "recovery-needed"}:
        inspect_recovery(gate, journal, gate.root / "usr/bin/licasa", "appimage")
        backup = work / "previous-app"
        if backup.exists():
            remove_path(gate.root)
            os.replace(backup, gate.root)
        elif not journal.get("hadRoot"):
            remove_path(gate.root)
        elif gate.root.exists():
            info = gate.root.stat()
            if [info.st_dev, info.st_ino] != journal.get("oldRootIdentity"):
                raise LifecycleError("Old runtime recovery is missing; keep the gate and repair from a verified package.")
        else:
            raise LifecycleError("Both the old runtime and recovery copy are missing.")
        for index, record in enumerate(journal.get("integrations", [])):
            destination = Path(record["path"])
            saved = work / f"integration-{index}"
            if record["existed"]:
                if not saved.exists() and not saved.is_symlink():
                    if fingerprint(destination) == record.get("fingerprint"):
                        continue
                    raise LifecycleError(f"Missing integration recovery copy: {saved}")
                remove_path(destination)
                os.replace(saved, destination)
            else:
                remove_path(destination)
    gate.write("verified", outcome="recovered", restart="deferred")
    shutil.rmtree(work)


def _relative(value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or not path.parts or ".." in path.parts or value in {"", "."}:
        raise LifecycleError("Invalid file in Local deployment manifest.")
    return path


def cleanup_completed_recovery(gate: InstallationGate, journal: dict, prefix: str) -> None:
    value = journal.get("recoveryRoot")
    if not value:
        return
    work = absolute(value)
    if work.parent != gate.root.parent or not work.name.startswith(prefix):
        raise LifecycleError("Completed transaction has an unsafe recovery location.")
    if work.exists() or work.is_symlink():
        _private(work, directory=True)
        shutil.rmtree(work)
    gate.write("verified", recoveryRoot="")


def _safe_parent(root: Path, relative: Path) -> None:
    current = root
    for name in relative.parts[:-1]:
        current /= name
        if current.is_symlink() or (current.exists() and not current.is_dir()):
            raise LifecycleError(f"Refusing an indirect application destination: {current}")


def recover_overlay(gate: InstallationGate) -> None:
    journal = gate.read()
    if not journal:
        return
    if journal["state"] == "verified":
        cleanup_completed_recovery(gate, journal, ".licasa-local.")
        return
    work = absolute(journal.get("recoveryRoot", ""))
    if work.parent != gate.root.parent or not work.name.startswith(".licasa-local."):
        raise LifecycleError("Invalid Local recovery directory.")
    _private(work, directory=True)
    if journal["state"] in {"replacing", "committed", "recovery-needed"}:
        inspect_recovery(gate, journal, gate.root / "bin/licasa", "filesystem")
        for index, record in enumerate(journal.get("files", [])):
            relative = _relative(record["path"])
            _safe_parent(gate.root, relative)
            destination = gate.root / relative
            backup = work / f"file-{index}"
            if record["fingerprint"] is not None:
                if not backup.exists() and not backup.is_symlink():
                    if fingerprint(destination) == record["fingerprint"]:
                        continue
                    raise LifecycleError(f"Missing Local recovery file: {backup}")
                destination.parent.mkdir(parents=True, exist_ok=True)
                remove_path(destination)
                os.replace(backup, destination)
            else:
                remove_path(destination)
        for value in reversed(journal.get("createdDirectories", [])):
            try:
                (gate.root / _relative(value)).rmdir()
            except (FileNotFoundError, OSError):
                pass
    gate.write("verified", outcome="recovered", restart="deferred")
    shutil.rmtree(work)


def install_overlay(gate: InstallationGate, staged: Path, binary: Path) -> dict:
    """Replace only manifest-owned files, preserving unrelated prefix contents."""
    previous = gate.read()
    if previous:
        recover_overlay(gate)
    paths = sorted(str(path.relative_to(staged)) for path in staged.rglob("*")
                   if path.is_file() or path.is_symlink())
    manifest_name = ".licasa-local-files.json"
    old_manifest = gate.root / manifest_name
    obsolete = set()
    if old_manifest.exists():
        if old_manifest.is_symlink():
            raise LifecycleError("Unsafe Local file manifest.")
        try:
            manifest = json_object(old_manifest.read_bytes(), "Invalid Local file manifest.")
            names = manifest.get("files")
            if not isinstance(names, list) or any(not isinstance(value, str) for value in names):
                raise LifecycleError("Invalid Local file manifest.")
            obsolete = {str(_relative(value)) for value in names}
        except (ValueError, KeyError, TypeError) as error:
            raise LifecycleError("Invalid Local file manifest.") from error
    (staged / manifest_name).write_text(json.dumps({"schema": 1, "files": paths}, sort_keys=True) + "\n")
    changed = sorted(set(paths) | obsolete | {manifest_name})
    required_space = sum((staged / value).stat().st_size for value in paths
                         if not (staged / value).is_symlink())
    if shutil.disk_usage(gate.root.parent).free < required_space * 2 + 1024 * 1024:
        raise LifecycleError("Insufficient space for the staged Local runtime and recovery copy.")
    work = Path(tempfile.mkdtemp(prefix=".licasa-local.", dir=gate.root.parent))
    records = []
    created = set()
    try:
        for index, value in enumerate(changed):
            relative = _relative(value)
            _safe_parent(gate.root, relative)
            destination = gate.root / relative
            old = fingerprint(destination)
            records.append({"path": value, "fingerprint": old})
            if old is not None:
                shutil.copy2(destination, work / f"file-{index}", follow_symlinks=False)
            parent = relative.parent
            while str(parent) != ".":
                if not (gate.root / parent).exists():
                    created.add(str(parent))
                parent = parent.parent
        result = gate.prepare(binary, "filesystem", kind="overlay", recoveryRoot=str(work),
                              files=records, createdDirectories=sorted(created, key=lambda value: len(Path(value).parts)))
        gate.write("replacing")
        for value in changed:
            destination = gate.root / value
            source = staged / value
            if not source.exists() and not source.is_symlink():
                remove_path(destination)
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix=".licasa-new-", dir=destination.parent)
            os.close(fd)
            temporary_path = Path(temporary)
            try:
                temporary_path.unlink()
                shutil.copy2(source, temporary_path, follow_symlinks=False)
                os.replace(temporary_path, destination)
            finally:
                temporary_path.unlink(missing_ok=True)
        gate.write("committed")
        verify_identity(binary, gate.root, "filesystem", gate_fd=gate.fd)
        gate.write("verified", generation=generation(binary), restart="deferred")
        shutil.rmtree(work)
        return result
    except BaseException:
        journal = gate.read()
        if journal and journal.get("recoveryRoot") == str(work) and journal["state"] != "verified":
            try:
                recover_overlay(gate)
            except (LifecycleError, OSError):
                gate.write("recovery-needed")
                raise LifecycleError(f"Local recovery incomplete; retained files: {work}")
        elif work.exists():
            shutil.rmtree(work)
        raise


def install_appimage_launcher(root: Path) -> None:
    """Put the launch gate before linuxdeploy's hooks and ELF loader."""
    if not (root / "usr/libexec/licasa/lifecycle_update.py").is_file():
        raise LifecycleError("AppDir has no lifecycle launcher helper; rebuild its matching application package.")
    runner = root / "AppRun"
    original = root / "AppRun.lifecycle-original"
    if not original.exists():
        os.replace(runner, original)
    runner.write_text('''#!/usr/bin/env bash
set -euo pipefail
licasa_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
for licasa_variable in ${!SNAP@}; do
  case "$licasa_variable" in SNAP|SNAP_*) unset "$licasa_variable" ;; esac
done
# Raw portable AppImages have no managed transaction root or Python dependency.
if [[ -n ${APPIMAGE:-} && ${APPDIR:-} == "$licasa_root" ]]; then
  exec "$licasa_root/AppRun.lifecycle-original" "$@"
fi
unset APPIMAGE
export APPDIR="$licasa_root"
exec python3 "$licasa_root/usr/libexec/licasa/lifecycle_update.py" launch \\
  --root "$licasa_root" --binary "$licasa_root/usr/bin/licasa" --family appimage \\
  --launcher "$licasa_root/AppRun.lifecycle-original" -- "$@"
''')
    runner.chmod(0o755)


def install_local_launcher(staged: Path) -> None:
    runner = staged / "bin/licasa-launch"
    runner.parent.mkdir(parents=True, exist_ok=True)
    runner.write_text('''#!/usr/bin/env bash
set -euo pipefail
licasa_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
exec python3 "$licasa_root/libexec/licasa/lifecycle_update.py" launch \\
  --root "$licasa_root" --binary "$licasa_root/bin/licasa" --family filesystem -- "$@"
''')
    runner.chmod(0o755)


def launch(root: Path, binary: Path, family: str, launcher: Path | None, arguments: list[str]) -> None:
    root = absolute(root)
    for variable in list(os.environ):
        if variable == "SNAP" or variable.startswith("SNAP_") or (family == "filesystem" and variable in {"APPIMAGE", "APPDIR"}):
            os.environ.pop(variable, None)
    command = [str(launcher or binary), *arguments]
    # Portable raw AppImages have no managed transaction root. An installer
    # contacts the running managed application through authenticated IPC rather
    # than loading a CLI executable while its payload is being replaced.
    if family == "appimage":
        if os.environ.get("APPIMAGE") and os.environ.get("APPDIR") == str(root):
            os.execv(command[0], command)
        os.environ.pop("APPIMAGE", None)
        os.environ["APPDIR"] = str(root)
    directory = root.parent / f".{root.name}.licasa-lifecycle"
    if directory.exists() or directory.is_symlink():
        _private(directory, directory=True)
        protected_ancestry(directory, allow_private_shield=True)
        lock = directory / "lock"
        _private(lock)
        journal = directory / "transaction.json"
        record = read_journal(journal, root)
        identity_check = ("--lifecycle" in arguments
                          and arguments[arguments.index("--lifecycle") + 1:] == ["identity"])
        inherited = os.environ.pop("LICASA_IDENTITY_GATE_FD", "")
        if identity_check and inherited and record and record.get("state") == "committed":
            descriptor = int(inherited)
            opened, linked = os.fstat(descriptor), lock.lstat()
            if (opened.st_dev, opened.st_ino) != (linked.st_dev, linked.st_ino):
                raise LifecycleError("Identity verification did not inherit this installation's gate.")
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # This inherited exclusive descriptor remains open through the
            # short headless identity check; the updater still owns its copy.
            os.set_inheritable(descriptor, True)
            os.execv(command[0], command)
        descriptor = os.open(lock, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise LifecycleError("Licasa is being updated; retry after installation completes.") from error
            # The first read can race with a completed or interrupted transaction.
            # Re-read after acquiring shared exclusion before executing any loader.
            record = read_journal(journal, root)
            if record and record.get("state") != "verified":
                raise LifecycleError("An interrupted Licasa update needs recovery. Run the same installer again.")
        except BaseException:
            os.close(descriptor)
            raise
        os.dup2(descriptor, 198, inheritable=True)
        if descriptor != 198:
            os.close(descriptor)
        os.environ["LICASA_LAUNCH_GATE_FD"] = "198"
    os.execv(command[0], command)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "mark", "recover", "verify", "installation-id", "launch"))
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--family", choices=("appimage", "filesystem"), default="appimage")
    parser.add_argument("--lock-fd", type=int)
    parser.add_argument("--state", choices=sorted(STATES))
    parser.add_argument("--recovery-root", type=Path)
    parser.add_argument("--integration", action="append", default=[])
    parser.add_argument("--identity-check", action="store_true")
    parser.add_argument("--launcher", type=Path)
    parser.add_argument("--strict-process-inspection", action="store_true", default=None)
    args, arguments = parser.parse_known_args()
    if arguments and arguments[0] == "--":
        arguments.pop(0)
    if arguments and args.operation != "launch":
        parser.error("Unexpected arguments")
    try:
        if args.operation == "launch":
            launch(args.root, args.binary or args.root / "usr/bin/licasa", args.family, args.launcher, arguments)
            return
        with InstallationGate(args.root, args.lock_fd, args.strict_process_inspection) as gate:
            if args.operation == "installation-id":
                print(installation_id(args.binary or gate.root / "usr/bin/licasa", gate.root, args.family))
            elif args.operation == "prepare":
                info = gate.root.stat() if gate.root.exists() else None
                result = gate.prepare(args.binary or gate.root / "usr/bin/licasa", args.family,
                             recoveryRoot=str(absolute(args.recovery_root)) if args.recovery_root else "",
                             hadRoot=info is not None,
                             oldRootIdentity=[info.st_dev, info.st_ino] if info else None,
                             integrations=[{"path": path, "existed": Path(path).exists() or Path(path).is_symlink(),
                                            "fingerprint": fingerprint(Path(path))}
                                           for path in args.integration])
                print(json.dumps(result, sort_keys=True))
            elif args.operation == "mark":
                gate.write(args.state or "recovery-needed")
            elif args.operation == "recover":
                recover_appimage(gate)
            else:
                binary = args.binary or gate.root / "usr/bin/licasa"
                if not binary.is_file():
                    raise LifecycleError("Replacement is missing its viewer executable.")
                if args.identity_check:
                    verify_identity(binary, gate.root, args.family,
                                    gate.root / "AppRun" if args.family == "appimage" else None,
                                    gate_fd=gate.fd)
                gate.write("verified", generation=generation(binary), restart="deferred")
    except (LifecycleError, OSError, ValueError) as error:
        print(f"Licasa lifecycle: {error}", file=sys.stderr)
        raise SystemExit(1) from error


if __name__ == "__main__":
    main()
