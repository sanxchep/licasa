#!/usr/bin/env python3
"""Check a main release commit or matching tag and derive its version tag."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
CHANNELS = {"edge", "beta", "candidate", "stable"}


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip()


def match_one(pattern: str, content: str, label: str) -> str:
    matches = re.findall(pattern, content, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one {label}; found {len(matches)}")
    return matches[0]


def check(channel: str, expected_sha: str, source_ref: str) -> str:
    if channel not in CHANNELS:
        raise ValueError(f"Unsupported Snap channel: {channel}")
    tagged = source_ref.startswith("refs/tags/")
    if source_ref != "refs/heads/main" and not tagged:
        raise ValueError("Release must be started from main or a version tag")
    head = git("rev-parse", "HEAD")
    if head != expected_sha:
        raise ValueError(f"Checkout {head} does not match selected branch tip {expected_sha}")
    cmake = (ROOT / "CMakeLists.txt").read_text()
    cmake_version = match_one(
        r"\bproject\(\s*Licasa\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\b",
        cmake, "CMake project version")
    snapcraft = (ROOT / "snap/snapcraft.yaml").read_text()
    snap_name = match_one(r"^name:\s*['\"]?([A-Za-z0-9-]+)['\"]?\s*$", snapcraft,
                          "Snap name")
    snap_version = match_one(r"^version:\s*['\"]?([0-9]+\.[0-9]+\.[0-9]+)['\"]?\s*$",
                             snapcraft, "Snap version")
    grade = match_one(r"^grade:\s*(devel|stable)\s*$", snapcraft, "Snap grade")
    if snap_name != "licasa":
        raise ValueError(f"Expected Snap name licasa, found {snap_name}")
    if cmake_version != snap_version:
        raise ValueError(f"Version mismatch: CMake={cmake_version}, Snap={snap_version}")
    version = cmake_version
    tag = f"v{version}"
    if not TAG.fullmatch(tag):
        raise ValueError(f"Version must use vMAJOR.MINOR.PATCH without leading zeros: {tag}")
    if tagged:
        if source_ref != f"refs/tags/{tag}":
            raise ValueError(f"Release tag {source_ref} does not match source version {tag}")
        if git("rev-parse", f"{tag}^{{commit}}") != head:
            raise ValueError(f"Release tag {tag} does not point to selected commit {head}")
        result = subprocess.run(["git", "merge-base", "--is-ancestor", head,
                                 "origin/main"], cwd=ROOT, capture_output=True)
        if result.returncode:
            raise ValueError(f"Release tag {tag} must point to a main commit")
    version_parts = tuple(map(int, version.split(".")))
    existing_versions = [
        tuple(map(int, match.groups()))
        for name in git("tag", "--list", "v*").splitlines()
        if not (tagged and name == tag)
        if (match := TAG.fullmatch(name))
    ]
    if existing_versions and version_parts <= max(existing_versions):
        raise ValueError(f"{tag} must be newer than all existing version tags")
    if channel in {"stable", "candidate"} and grade != "stable":
        raise ValueError(f"{channel} requires grade: stable; recipe is {grade}")
    notes = ROOT / "packaging/release-notes" / f"{tag}.md"
    if not notes.is_file() or not notes.read_text().strip():
        raise ValueError(f"Missing release notes: {notes.relative_to(ROOT)}")
    return version


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--source-ref", required=True)
    args = parser.parse_args()
    try:
        version = check(args.channel, args.expected_sha, args.source_ref)
    except ValueError as error:
        parser.error(str(error))
    print(version)


if __name__ == "__main__":
    main()
