"""Scratch-only per-user desktop registration for a verified portable AppImage."""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile


APP_NAME = "warm08.desktop"
MANIFEST = "installed.json"


@dataclass(frozen=True)
class Package:
    path: Path
    sha256: str
    viewer_sha256: str


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def receipt(info):
    mtime, mtime_ns = divmod(info.st_mtime_ns, 10**9)
    ctime, ctime_ns = divmod(info.st_ctime_ns, 10**9)
    return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{mtime}:{mtime_ns}:{ctime}:{ctime_ns}"


def identity(archive):
    return hashlib.sha256(os.fsencode(f"{os.geteuid()}\nportable\n{archive}")).hexdigest()[:32]


def quote(value):
    value = str(value)
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("Desktop Exec contains an unrepresentable character")
    value = re.sub(r'([\\`"$])', r'\\\1', value).replace("%", "%%")
    return '"' + value.replace("\\", "\\\\") + '"'


def consent(archive):
    instance = identity(archive)
    return ("[Desktop Entry]\nType=Application\nVersion=1.0\nName=Licasa\n"
            "Comment=Start Licasa in background at login\n"
            f"Exec=/usr/bin/env {quote(archive)} --background\n"
            f"X-Licasa-Background=1\nX-Licasa-Installation={instance}\nX-Licasa-Schema=1\n"
            "Terminal=false\nNoDisplay=true\nStartupNotify=false\n")


def profile_files(package, helper, route):
    installation = identity(package.path)
    arguments = (helper, route, package.path, receipt(package.path.stat()),
                 f"licasa-lc-{installation}.sock", package.viewer_sha256)
    command = " ".join(quote(piece) for piece in arguments) + " %f"
    desktop = ("[Desktop Entry]\nName=Licasa WARM08\nType=Application\n"
               f"Exec=/usr/bin/env {command}\nMimeType=image/jpeg;image/png;\nStartupNotify=false\n")
    defaults = f"[Default Applications]\nimage/jpeg={APP_NAME}\nimage/png={APP_NAME}\n"
    return {f"data/applications/{APP_NAME}": desktop, "config/mimeapps.list": defaults,
            f"viewer-config/autostart/licasa-background-{installation}.desktop": consent(package.path)}


def read(profile):
    path = profile / MANIFEST
    if not path.exists():
        return {}
    if path.is_symlink():
        raise RuntimeError("Refusing a symlinked scratch manifest")
    return json.loads(path.read_text())


def verify_scope(profile):
    profile = profile.resolve(strict=True)
    root = next((p for p in (profile, *profile.parents)
                 if p.parent == Path("/var/tmp") and p.name.startswith("licasa-dispatch-")), None)
    if root is None or root.stat().st_uid != os.geteuid() or stat.S_IMODE(root.stat().st_mode) != 0o700:
        raise RuntimeError("Registration requires an owned private /var/tmp Licasa pilot tree")
    return profile


def atomic_text(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, mode="w", delete=False, encoding="utf-8") as stream:
        pending = Path(stream.name)
        stream.write(value)
    try:
        pending.replace(path)
    finally:
        pending.unlink(missing_ok=True)


def apply(profile, desired):
    profile = verify_scope(profile)
    old = read(profile)
    previous = old.get("files", {})
    for rel in previous.keys() | desired.keys():
        path = profile / rel
        if not path.resolve().is_relative_to(profile) or path.is_symlink():
            raise RuntimeError(f"Unsafe integration path {path}")
        if path.exists() and (rel not in previous or path.read_text() != previous[rel]):
            raise RuntimeError(f"Preserving independent or edited entry {path}")
    for rel, value in desired.items():
        atomic_text(profile / rel, value)
    for rel in previous.keys() - desired.keys():
        (profile / rel).unlink(missing_ok=True)
    return profile


def bind(profile, package, helper, route, *, opted_in):
    if not opted_in:
        raise RuntimeError("Portable registration requires explicit background opt-in")
    if (package.path.resolve(strict=True) != package.path or helper.resolve(strict=True) != helper
            or not package.path.is_file() or digest(package.path) != package.sha256):
        raise RuntimeError("Portable run requires the verified archive at a stable regular path")
    desired = profile_files(package, helper, route)
    profile = apply(profile, desired)
    state = dict(archive=str(package.path), archive_sha256=package.sha256,
                 viewer_sha256=package.viewer_sha256, archive_object=receipt(package.path.stat()),
                 installation_id=identity(package.path), route=route, files=desired)
    atomic_text(profile / MANIFEST, json.dumps(state, indent=2) + "\n")
    return state


def remove(profile):
    profile = apply(profile, {})
    (profile / MANIFEST).unlink(missing_ok=True)
