#!/usr/bin/env python3
"""Fetch the pinned public AVIF qualification corpus without trusting mutable URLs.

The source manifest pins a repository commit, exact byte length, and Git blob
SHA-1 for every file. Downloads are bounded, verified before publication, and
written atomically. Ordinary tests never access the network; they consume only
this verified cache.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
import urllib.error
import urllib.request
from pathlib import Path, PurePosixPath

COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
BLOB_RE = re.compile(r"^[0-9a-f]{40}$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
SOURCE_ID_RE = re.compile(r"^[A-Za-z0-9_.-]+$")
DEFAULT_TIMEOUT = 45
USER_AGENT = "Licasa-AVIF-corpus-fetcher/1"


def git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def safe_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise ValueError(f"unsafe corpus path: {value!r}")
    return path


def load_manifest(path: Path) -> dict:
    raw = path.read_bytes()
    data = json.loads(raw)
    if data.get("schema") != 1:
        raise ValueError("unsupported AVIF corpus manifest schema")
    sources = data.get("sources")
    if not isinstance(sources, list) or not sources:
        raise ValueError("manifest has no sources")

    total = 0
    seen_destinations: set[tuple[str, str]] = set()
    for source in sources:
        source_id = source.get("id", "")
        repository = source.get("repository", "")
        commit = source.get("commit", "")
        if not SOURCE_ID_RE.fullmatch(source_id):
            raise ValueError(f"invalid source id: {source_id!r}")
        if not REPOSITORY_RE.fullmatch(repository):
            raise ValueError(f"invalid GitHub repository: {repository!r}")
        if not COMMIT_RE.fullmatch(commit):
            raise ValueError(f"invalid pinned commit for {source_id}: {commit!r}")
        files = source.get("files")
        if not isinstance(files, list) or not files:
            raise ValueError(f"source {source_id} has no files")
        for item in files:
            rel = safe_relative_path(item.get("path", ""))
            blob = item.get("git_blob_sha1", "")
            size = item.get("size")
            if not BLOB_RE.fullmatch(blob):
                raise ValueError(f"invalid Git blob identity for {source_id}/{rel}")
            if not isinstance(size, int) or size <= 0:
                raise ValueError(f"invalid size for {source_id}/{rel}")
            key = (source_id, str(rel))
            if key in seen_destinations:
                raise ValueError(f"duplicate corpus path: {source_id}/{rel}")
            seen_destinations.add(key)
            total += size

    maximum = data.get("maximum_total_bytes")
    if not isinstance(maximum, int) or maximum <= 0 or total > maximum:
        raise ValueError(f"declared corpus size {total} exceeds manifest bound {maximum}")
    data["_manifest_sha256"] = hashlib.sha256(raw).hexdigest()
    data["_expected_total_bytes"] = total
    return data


def verify_file(path: Path, expected_size: int, expected_blob: str) -> tuple[bool, str]:
    if not path.is_file() or path.is_symlink():
        return False, "missing"
    try:
        data = path.read_bytes()
    except OSError as exc:
        return False, f"read failed: {exc}"
    if len(data) != expected_size:
        return False, f"size {len(data)} != {expected_size}"
    actual = git_blob_sha1(data)
    if actual != expected_blob:
        return False, f"Git blob {actual} != {expected_blob}"
    return True, "verified"


def download(url: str, expected_size: int, timeout: int) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/octet-stream"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        final_url = response.geturl()
        if not final_url.startswith("https://"):
            raise RuntimeError(f"refusing non-HTTPS redirect: {final_url}")
        length = response.headers.get("Content-Length")
        if length is not None and int(length) != expected_size:
            raise RuntimeError(f"Content-Length {length} != expected {expected_size}")
        data = response.read(expected_size + 1)
    if len(data) != expected_size:
        raise RuntimeError(f"downloaded {len(data)} bytes, expected {expected_size}")
    return data


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    finally:
        try:
            os.unlink(temp_name)
        except FileNotFoundError:
            pass


def provenance(manifest: dict) -> dict:
    return {
        "schema": 1,
        "manifest_sha256": manifest["_manifest_sha256"],
        "expected_total_bytes": manifest["_expected_total_bytes"],
        "sources": [
            {
                "id": source["id"],
                "repository": source["repository"],
                "commit": source["commit"],
                "files": [
                    {
                        "path": item["path"],
                        "git_blob_sha1": item["git_blob_sha1"],
                        "size": item["size"],
                        "license": item.get("license", ""),
                    }
                    for item in source["files"]
                ],
            }
            for source in manifest["sources"]
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    repo_root = Path(__file__).resolve().parents[1]
    parser.add_argument("--manifest", type=Path, default=repo_root / "tests/test-assets/modern/avif/external-sources.json")
    parser.add_argument("--output", type=Path, default=repo_root / "build/deps/avif-samples")
    parser.add_argument("--offline", action="store_true", help="verify the cache without downloading")
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()
    if args.timeout <= 0 or args.timeout > 300:
        parser.error("--timeout must be in 1..300 seconds")

    manifest_path = args.manifest.resolve()
    output = args.output.resolve()
    manifest = load_manifest(manifest_path)
    output.mkdir(parents=True, exist_ok=True)

    verified = 0
    downloaded = 0
    for source in manifest["sources"]:
        source_id = source["id"]
        repository = source["repository"]
        commit = source["commit"]
        for item in source["files"]:
            rel = safe_relative_path(item["path"])
            destination = output / source_id / Path(*rel.parts)
            ok, reason = verify_file(destination, item["size"], item["git_blob_sha1"])
            if ok:
                verified += 1
                print(f"verified  {source_id}/{rel}")
                continue
            if args.offline:
                raise SystemExit(f"offline verification failed: {source_id}/{rel}: {reason}")

            url = f"https://raw.githubusercontent.com/{repository}/{commit}/{rel.as_posix()}"
            print(f"fetching   {source_id}/{rel}")
            try:
                data = download(url, item["size"], args.timeout)
            except (urllib.error.URLError, OSError, RuntimeError, ValueError) as exc:
                raise SystemExit(f"download failed for {source_id}/{rel}: {exc}") from exc
            actual = git_blob_sha1(data)
            if actual != item["git_blob_sha1"]:
                raise SystemExit(
                    f"identity mismatch for {source_id}/{rel}: Git blob {actual} != {item['git_blob_sha1']}"
                )
            atomic_write(destination, data)
            downloaded += 1
            verified += 1

    evidence = provenance(manifest)
    evidence_bytes = (json.dumps(evidence, indent=2, sort_keys=True) + "\n").encode("utf-8")
    atomic_write(output / "sources.json", evidence_bytes)

    # Final offline pass catches publication/path mistakes and proves every
    # cache entry matches the source-tree manifest after all writes complete.
    for source in manifest["sources"]:
        for item in source["files"]:
            rel = safe_relative_path(item["path"])
            destination = output / source["id"] / Path(*rel.parts)
            ok, reason = verify_file(destination, item["size"], item["git_blob_sha1"])
            if not ok:
                raise SystemExit(f"post-fetch verification failed: {source['id']}/{rel}: {reason}")

    print(f"AVIF corpus verified: {verified} files, {manifest['_expected_total_bytes']} bytes; downloaded {downloaded}")
    print(f"Evidence: {output / 'sources.json'}")


if __name__ == "__main__":
    main()
