#!/usr/bin/env python3
"""Fetch and SHA-256 verify Licasa's pinned CC0 phone RAW fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests/test-assets/modern/raw/vendor-sources.json"
DEFAULT_DESTINATION = ROOT / "build/deps/vendor-samples/raw"
MAX_SAMPLE_BYTES = 64 * 1024 * 1024
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _load_manifest(path: Path) -> list[dict]:
    entries = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(entries, list) or not entries:
        raise RuntimeError(f"RAW vendor manifest is empty or invalid: {path}")

    names: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise RuntimeError(f"Invalid RAW vendor manifest entry: {entry!r}")
        name = entry.get("file", "")
        digest = entry.get("sha256", "")
        expected_bytes = entry.get("bytes")
        source = entry.get("source", "")
        if not name or Path(name).name != name or name in names:
            raise RuntimeError(f"Unsafe or duplicate RAW sample name: {name!r}")
        if not isinstance(digest, str) or not _SHA256_RE.fullmatch(digest):
            raise RuntimeError(f"Invalid SHA-256 for RAW sample: {name}")
        if not isinstance(expected_bytes, int) or not (0 < expected_bytes <= MAX_SAMPLE_BYTES):
            raise RuntimeError(f"Invalid size bound for RAW sample: {name}")
        if not isinstance(source, str) or not source.startswith("https://"):
            raise RuntimeError(f"RAW sample must use HTTPS: {name}")
        names.add(name)
    return entries


def _digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _valid_existing(path: Path, entry: dict) -> bool:
    return (path.is_file()
            and path.stat().st_size == entry["bytes"]
            and _digest(path) == entry["sha256"])


def fetch_samples(destination: Path = DEFAULT_DESTINATION,
                  manifest: Path = DEFAULT_MANIFEST,
                  *, check_only: bool = False,
                  verbose: bool = True) -> tuple[list[Path], list[dict]]:
    """Return verified sample paths and manifest entries, fetching when needed."""
    entries = _load_manifest(manifest)
    destination.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []

    for entry in entries:
        target = destination / entry["file"]
        if not _valid_existing(target, entry):
            if check_only:
                raise RuntimeError(f"Missing or invalid RAW vendor sample: {target}")

            partial = target.with_name(target.name + ".partial")
            partial.unlink(missing_ok=True)
            if verbose:
                print(f"Fetching {entry['camera']}: {entry['file']}", flush=True)
            try:
                request = Request(entry["source"], headers={"User-Agent": "Licasa RAW test fixture fetcher/1"})
                with urlopen(request, timeout=60) as response, partial.open("wb") as output:
                    remaining = entry["bytes"]
                    while True:
                        block = response.read(min(1024 * 1024, remaining + 1))
                        if not block:
                            break
                        remaining -= len(block)
                        if remaining < 0:
                            raise RuntimeError(f"Oversized RAW sample: {entry['file']}")
                        output.write(block)
                if remaining != 0:
                    raise RuntimeError(f"Truncated RAW sample: {entry['file']}")
                if _digest(partial) != entry["sha256"]:
                    raise RuntimeError(f"SHA-256 mismatch: {entry['file']}")
                partial.replace(target)
            finally:
                partial.unlink(missing_ok=True)

        if not _valid_existing(target, entry):
            raise RuntimeError(f"RAW sample verification failed: {target}")
        if verbose:
            print(f"Verified {entry['file']} ({entry['bytes']} bytes)", flush=True)
        paths.append(target)

    return paths, entries


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, default=DEFAULT_DESTINATION,
                        help=f"cache directory (default: {DEFAULT_DESTINATION})")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--check-only", action="store_true",
                        help="verify the cache without downloading anything")
    args = parser.parse_args()
    paths, _ = fetch_samples(args.destination.resolve(), args.manifest.resolve(),
                             check_only=args.check_only)
    print(f"RAW vendor corpus ready: {paths[0].parent}")


if __name__ == "__main__":
    main()
