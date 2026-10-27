#!/usr/bin/env python3
"""Select CI work once and reject incomplete required job results."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

from check_support_manifest import ROOT, load_manifest
from hosted_candidate import source_version, validate_requested_source


def source_presets(presets: str) -> list[str]:
    selected = json.loads(presets)
    if (not isinstance(selected, list) or not selected
            or any(not isinstance(value, str) or value not in {"release", "asan", "ubsan"}
                   for value in selected) or len(set(selected)) != len(selected)):
        raise ValueError("Expected distinct supported source verification presets")
    return selected


def source_plan(source_sha: str, actual_sha: str, dirty: str, presets: str,
                runner_environment: str) -> dict:
    if runner_environment != "github-hosted":
        raise ValueError("Source verification requires an ephemeral hosted runner")
    validate_requested_source(source_sha, source_sha, actual_sha)
    if dirty:
        raise ValueError("Source verification requires a clean source checkout")
    return {"sha": actual_sha, "presets": json.dumps(source_presets(presets))}


def check_source_results(needs: dict) -> list[str]:
    required = {"plan", "quick", "test"}
    failed = sorted(name for name in required if needs.get(name, {}).get("result") != "success")
    if failed:
        raise ValueError("Required source verification did not pass: " + ", ".join(failed))
    return sorted(required)


def make_plan(event: str, complete: bool, changed_paths: list[str], presets: str = "") -> dict:
    if complete and event != "workflow_dispatch":
        raise ValueError("Complete candidate verification requires manual dispatch")
    selected = source_presets(presets or json.dumps(
        ["release"] if event == "pull_request" else ["release", "asan", "ubsan"]))
    if complete and set(selected) != {"release", "asan", "ubsan"}:
        raise ValueError("Complete candidate verification requires all three source presets")
    build = event != "pull_request" or any(
        path and not (path.endswith(".md") or path == "LICENSE") for path in changed_paths)
    packages = complete or (event == "pull_request" and any(
        path and not path.endswith(".md") and (
            path.startswith(("src/", "qml/", "cmake/", "packaging/", "snap/", "tools/",
                             "tests/", ".github/workflows/", "assets/icons/"))
            or path in {"CMakeLists.txt", "CMakePresets.json", "assets/licasa.png"})
        for path in changed_paths))
    return {"build": str(build).lower(), "packages": str(packages).lower(),
            "complete": str(complete).lower(), "presets": json.dumps(selected)}


def check_results(needs: dict) -> list[str]:
    if needs.get("plan", {}).get("result") != "success":
        raise ValueError("CI planning did not succeed")
    outputs = needs["plan"].get("outputs", {})
    for name in ("build", "packages", "complete"):
        if outputs.get(name) not in {"true", "false"}:
            raise ValueError(f"Missing or invalid CI plan output: {name}")
    if outputs["complete"] == "true" and (
            outputs["build"] != "true" or outputs["packages"] != "true"):
        raise ValueError("Complete verification cannot omit source or package builds")
    required = {"plan", "package_contract"}
    if outputs["build"] == "true":
        required.add("test")
    if outputs["packages"] == "true":
        required.update(("ubuntu", "debian"))
    if outputs["complete"] == "true":
        required.update(("wrappers", "snap", "packages", "snap_desktops", "attest"))
    failed = sorted(name for name in required if needs.get(name, {}).get("result") != "success")
    failed += sorted(name for name, job in needs.items()
                     if name not in required and job.get("result") not in {"success", "skipped"})
    if failed:
        raise ValueError("Required CI verification did not pass: " + ", ".join(failed))
    return sorted(required)


def package_matrix(manifest: dict) -> list[dict]:
    artifacts = {"ubuntu-deb": "verification-ubuntu-packages",
                 "appimage": "verification-ubuntu-packages",
                 "debian-deb": "verification-debian-packages",
                 "rpm": "verification-portable-packages", "arch": "verification-portable-packages"}
    return [{"target": target["id"], "artifact": artifacts[target["artifact"]]}
            for target in manifest["targets"] if target.get("qualifier")]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    plan = subparsers.add_parser("plan")
    plan.add_argument("--event", required=True)
    plan.add_argument("--source-sha", required=True)
    plan.add_argument("--expected-sha", default="")
    plan.add_argument("--complete", action="store_true")
    plan.add_argument("--presets", default="")
    plan.add_argument("--changed-files", type=Path)
    plan.add_argument("--github-output", type=Path, required=True)
    subparsers.add_parser("check")
    source = subparsers.add_parser("source-plan")
    source.add_argument("--source-sha", required=True)
    source.add_argument("--presets", required=True)
    source.add_argument("--github-output", type=Path, required=True)
    subparsers.add_parser("source-check")
    args = parser.parse_args()
    if args.command == "source-check":
        print("Required source checks passed: " + ", ".join(
            check_source_results(json.loads(os.environ["CI_RESULTS"]))))
        return
    if args.command == "check":
        print("Required checks passed: " + ", ".join(check_results(json.loads(os.environ["CI_RESULTS"]))))
        return
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()
    if args.command == "source-plan":
        result = source_plan(args.source_sha, actual, dirty, args.presets,
                             os.environ.get("RUNNER_ENVIRONMENT", ""))
        with args.github_output.open("a") as output:
            output.writelines(f"{key}={value}\n" for key, value in result.items())
        return
    validate_requested_source(args.source_sha,
                              args.expected_sha if args.complete else args.source_sha, actual)
    if dirty:
        raise ValueError("CI planning requires a clean source checkout")
    paths = ([value.decode() for value in args.changed_files.read_bytes().split(b"\0") if value]
             if args.changed_files else [])
    result = make_plan(args.event, args.complete, paths, args.presets)
    result.update(sha=actual, version=source_version((ROOT / "CMakeLists.txt").read_text(),
                                                   (ROOT / "snap/snapcraft.yaml").read_text()),
                  matrix=json.dumps(package_matrix(load_manifest())))
    with args.github_output.open("a") as output:
        output.writelines(f"{key}={value}\n" for key, value in result.items())


if __name__ == "__main__":
    main()
