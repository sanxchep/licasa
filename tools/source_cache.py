"""Verified source archives shared by the dependency preparation tools."""

import hashlib
from pathlib import Path
import tempfile
from urllib.request import urlopen


def add_source_arguments(parser):
    parser.add_argument("--source-cache", type=Path,
                        help="Shared cache of verified archives and pinned Git sources")
    parser.add_argument("--offline", action="store_true",
                        help="Require cached inputs; never download or fetch")
    parser.add_argument("--prepare-only", action="store_true",
                        help="Verify and prepare all source inputs without compiling")


def fetch_archive(directory, url, digest, *, name=None, algorithm="sha256",
                  offline=False, limit=256 * 1024 * 1024):
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / (name or url.rsplit("/", 1)[1])
    if archive.is_file():
        _verify_archive(archive, digest, algorithm, limit)
        return archive
    if offline:
        raise RuntimeError(f"Offline source input missing: {archive}")

    # Each writer owns its temporary, including failed downloads. Only verified
    # bytes enter the shared cache; cleanup cannot touch another writer's file.
    with tempfile.TemporaryDirectory(prefix=".licasa-source-", dir=directory) as temporary:
        partial = Path(temporary) / archive.name
        with urlopen(url, timeout=60) as response, partial.open("wb") as output:
            total = 0
            while block := response.read(1024 * 1024):
                total += len(block)
                if total > limit:
                    raise RuntimeError(f"Source archive exceeded size bound: {url}")
                output.write(block)
        _verify_archive(partial, digest, algorithm, limit)
        partial.replace(archive)
    return archive


def _verify_archive(archive, digest, algorithm, limit):
    if archive.stat().st_size > limit:
        raise RuntimeError(f"Source archive exceeded size bound: {archive}")
    with archive.open("rb") as content:
        actual = hashlib.file_digest(content, algorithm).hexdigest()
    if actual != digest:
        raise RuntimeError(f"Checksum mismatch: {archive} (expected {digest}, got {actual})")
