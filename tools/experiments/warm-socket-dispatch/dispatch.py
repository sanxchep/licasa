"""Opt-in, outside-the-AppImage desktop dispatch experiment for standalone Linux."""
import time

SCRIPT_ENTRY = time.monotonic_ns()

import errno
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import struct
import sys

STANDARD_IMPORTS = time.monotonic_ns()
from lifecycle_update import (  # noqa: E402
    LIMIT, LifecycleError, generation, json_object, protected_ancestry, start_identity,
)

IMPORTS_DONE = time.monotonic_ns()
VIEWER_SHA256 = "82080c537c8c9f9b446e73775ef3811b4d2a5f9024cbaa823c5a2741fdd731da"


def stamp(event, **values):
    print("WARM08D " + json.dumps(dict(event=event, ns=time.monotonic_ns(), **values)),
          file=sys.stderr, flush=True)


def phase(event, ns=None):
    if os.environ.get("LICASA_DISPATCH_PROFILE") == "1":
        print(f"WARM08P {ns if ns is not None else time.monotonic_ns()} {event}",
              file=sys.stderr, flush=True)


def file_object(info):
    mtime, mtime_ns = divmod(info.st_mtime_ns, 10**9)
    ctime, ctime_ns = divmod(info.st_ctime_ns, 10**9)
    return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{mtime}:{mtime_ns}:{ctime}:{ctime_ns}"


def private_endpoint(archive, runtime):
    directory = runtime.lstat()
    if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != os.geteuid() or stat.S_IMODE(directory.st_mode) != 0o700:
        raise LifecycleError("Runtime directory is not private")
    protected_ancestry(runtime)
    identity = hashlib.sha256(os.fsencode(f"{os.geteuid()}\nportable\n{archive}")).hexdigest()[:32]
    endpoint = runtime / f"licasa-lc-{identity}.sock"
    if len(os.fsencode(endpoint)) > 103:
        raise LifecycleError("Runtime endpoint is too long")
    return endpoint, identity


def connect(endpoint):
    try:
        info = endpoint.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise LifecycleError("Runtime endpoint is not an owned private socket")
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(1.0)
    try:
        connection.connect(str(endpoint))
    except OSError as error:
        connection.close()
        if error.errno in (errno.ENOENT, errno.ECONNREFUSED):
            return None
        raise
    return connection


def credentials(connection):
    return struct.unpack("3i", connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))


def peer(archive, connection, viewer_sha):
    pid, uid, _ = credentials(connection)
    if uid != os.geteuid():
        raise LifecycleError("Runtime peer has a different UID")
    proc = Path("/proc") / str(pid)
    executable = proc / "exe"
    started = start_identity(pid)
    node = file_object(executable.stat())
    mapped = Path(os.readlink(executable))
    mount = mapped.parents[2] if mapped.parts[-3:] == ("usr", "bin", "licasa") else None
    flags = os.statvfs(executable).f_flag
    environ = (proc / "environ").read_bytes().split(b"\0")
    if (mount is None or not mount.name.startswith(".mount_") or not flags & os.ST_RDONLY
            or os.fsencode(f"APPIMAGE={archive}") not in environ
            or os.fsencode(f"APPDIR={mount}") not in environ
            or generation(executable) != viewer_sha
            or file_object(executable.stat()) != node or start_identity(pid) != started):
        raise LifecycleError("Runtime peer does not map the pinned AppImage viewer")
    return pid, started, node


def exchange(connection, identity, operation, **fields):
    message = dict(protocol=1, installationId=identity, operation=operation, **fields)
    data = json.dumps(message, separators=(",", ":")).encode() + b"\n"
    if len(data) > LIMIT:
        raise LifecycleError("Launch request exceeds the lifecycle limit")
    deadline = time.monotonic() + 1.0
    connection.sendall(data)
    response = bytearray()
    while b"\n" not in response and len(response) < LIMIT:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise LifecycleError("Lifecycle reply timed out")
        connection.settimeout(remaining)
        chunk = connection.recv(min(4096, LIMIT - len(response)))
        if not chunk:
            break
        response.extend(chunk)
    body, delimiter, _ = response.partition(b"\n")
    if not delimiter:
        raise LifecycleError("No complete lifecycle receipt")
    return json_object(body, "Invalid lifecycle reply")


def validate(reply, identity, owner, session, viewer_sha):
    pid, started, node = owner
    if (reply.get("protocol"), reply.get("installationId"), reply.get("pid"),
            reply.get("startIdentity"), reply.get("generation"), reply.get("generationObject"),
            reply.get("desktopSessionId")) != (1, identity, pid, started, viewer_sha, node, session):
        raise LifecycleError("Reply does not match the authenticated resident")


def decision(archive, archive_object, target, env, viewer_sha=VIEWER_SHA256):
    """Return the first reason to use stock activation, or None for acknowledged open."""
    phase("guard-start")
    if "XDG_ACTIVATION_TOKEN" in env or "DESKTOP_STARTUP_ID" in env:
        return "desktop-activation-context"
    session = env.get("XDG_SESSION_ID", env.get("WAYLAND_DISPLAY", env.get("DISPLAY", "")))
    if not session or not target.is_absolute() or not target.is_file():
        return "unsupported-launch"
    sent = False
    try:
        if file_object(archive.lstat()) != archive_object:
            return "archive-changed"
        endpoint, identity = private_endpoint(archive, Path(env["XDG_RUNTIME_DIR"]))
        phase("discovery-start")
        status_socket = connect(endpoint)
        phase("discovery-end")
        if status_socket is None:
            return "no-listener"
        with status_socket:
            phase("identity-start")
            owner = peer(archive, status_socket, viewer_sha)
            phase("identity-end")
            status = exchange(status_socket, identity, "status")
            phase("status-reply")
        validate(status, identity, owner, session, viewer_sha)
        stamp("status", owner=owner[0], state=status.get("state"),
              consent=status.get("backgroundEnabled"))
        if (status.get("result") != "ready" or status.get("state") != "running"
                or status.get("backgroundEnabled") is not True):
            return "not-a-ready-opted-in-owner"

        phase("open-connect-start")
        launch_socket = connect(endpoint)
        if launch_socket is None:
            return "owner-gone-before-open"
        with launch_socket:
            pid, uid, _ = credentials(launch_socket)
            if (pid != owner[0] or uid != os.geteuid() or start_identity(pid) != owner[1]
                    or file_object((Path("/proc") / str(pid) / "exe").stat()) != owner[2]):
                return "owner-changed-before-open"
            if file_object(archive.lstat()) != archive_object:
                return "archive-changed-before-open"
            url = target.as_uri()
            if len(url) > LIMIT - 1024:
                return "unsupported-url-length"
            stamp("open-write", owner=pid)
            sent = True
            reply = exchange(launch_socket, identity, "open", url=url,
                             generation=viewer_sha, desktopSessionId=session)
            phase("open-reply")
        validate(reply, identity, owner, session, viewer_sha)
        if reply.get("result") == "accepted":
            stamp("direct-accepted", owner=owner[0])
            return None
        if reply.get("result") == "busy" and reply.get("state") == "stopping":
            return "explicit-stopping-rejection"
        raise LifecycleError("The resident declined the desktop open")
    except (OSError, KeyError, IndexError, ValueError, LifecycleError) as error:
        if sent:
            raise LifecycleError("Open was sent without an accepted or safely retryable receipt") from error
        stamp("pre-open-fallback", reason=str(error))
        return "pre-open-admission-failed"


def main(archive, archive_object, target, viewer_sha):
    phase("script-entry", SCRIPT_ENTRY)
    phase("stdlib-imported", STANDARD_IMPORTS)
    phase("all-imported", IMPORTS_DONE)
    try:
        route = decision(archive, archive_object, target, os.environ, viewer_sha)
    except LifecycleError as error:
        stamp("direct-failed", reason=str(error))
        return 1
    if route is None:
        return 0
    stamp("appimage-fallback", reason=route)
    phase("before-appimage-exec")
    os.execv(archive, [str(archive), str(target)])


if __name__ == "__main__":
    archive, receipt, viewer_sha, target = sys.argv[1:]
    raise SystemExit(main(Path(archive), receipt, Path(target), viewer_sha))
