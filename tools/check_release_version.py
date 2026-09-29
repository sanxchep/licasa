#!/usr/bin/env python3
"""Reject a release unless the selected branch, tag, and package versions agree."""

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


def check(tag: str, channel: str, expected_sha: str, source_ref: str) -> str:
    tag_match = TAG.fullmatch(tag)
    if not tag_match:
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH")
    if channel not in CHANNELS:
        raise ValueError(f"Unsupported Snap channel: {channel}")
    if not source_ref.startswith("refs/heads/"):
        raise ValueError("Start the release from a branch, not a tag")
    head = git("rev-parse", "HEAD")
    if head != expected_sha:
        raise ValueError(f"Checkout {head} does not match selected branch tip {expected_sha}")
    tag_commit = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    if tag_commit != head:
        raise ValueError(f"{tag} points to {tag_commit}, not selected branch tip {head}")

    version = ".".join(tag_match.groups())
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
    if version != cmake_version or version != snap_version:
        raise ValueError(f"Version mismatch: tag={version}, CMake={cmake_version}, "
                         f"Snap={snap_version}")
    if channel in {"stable", "candidate"} and grade != "stable":
        raise ValueError(f"{channel} requires grade: stable; recipe is {grade}")
    notes = ROOT / "packaging/release-notes" / f"{tag}.md"
    if not notes.is_file() or not notes.read_text().strip():
        raise ValueError(f"Missing release notes: {notes.relative_to(ROOT)}")
    return version


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--channel", required=True)
    parser.add_argument("--expected-sha", required=True)
    parser.add_argument("--source-ref", required=True)
    args = parser.parse_args()
    try:
        version = check(args.tag, args.channel, args.expected_sha, args.source_ref)
    except ValueError as error:
        parser.error(str(error))
    print(version)


if __name__ == "__main__":
    main()
