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
    required = {"plan", "quick", "core", "test"}
    failed = sorted(name for name in required if needs.get(name, {}).get("result") != "success")
    if failed:
        raise ValueError("Required source verification did not pass: " + ", ".join(failed))
    return sorted(required)


def make_plan(event: str, complete: bool, changed_paths: list[str], presets: str = "",
              *, profile: str = "") -> dict:
    if not profile:
        profile = "FULL" if complete or event != "pull_request" else (
            "DOCS" if changed_paths and all(p in {"README.md", "LICENSE"} for p in changed_paths)
            else "FULL")
    if profile not in {"FULL", "FAST", "DOCS", "NONE"}:
        raise ValueError("Unknown verification profile")
    selected = source_presets(presets or json.dumps(
        ["release", "asan", "ubsan"] if profile == "FULL" else ["release"]))
    if profile == "FULL" and set(selected) != {"release", "asan", "ubsan"}:
        raise ValueError("Full verification requires all three source presets")
    if profile != "FULL" and selected != ["release"]:
        raise ValueError("Reduced verification cannot masquerade as a full source matrix")
    return {"profile": profile, "build": str(profile in {"FAST", "FULL"}).lower(),
            "packages": str(profile == "FULL").lower(),
            "complete": str(profile == "FULL").lower(), "presets": json.dumps(selected)}


def check_results(needs: dict) -> list[str]:
    if needs.get("plan", {}).get("result") != "success":
        raise ValueError("CI planning did not succeed")
    outputs = needs["plan"].get("outputs", {})
    profile = outputs.get("profile")
    if profile not in {"FULL", "FAST", "DOCS", "NONE"}:
        raise ValueError("Missing or invalid verification profile")
    expected = make_plan("completion", profile == "FULL", [], profile=profile)
    if any(outputs.get(key) != expected[key] for key in ("build", "packages", "complete", "presets")):
        raise ValueError("Contradictory verification plan")
    required = {"plan"}
    if profile != "NONE":
        required.update(("public_files", "package_contract"))
    if profile in {"FAST", "FULL"}:
        required.add("test")
    if profile == "FULL":
        required.update(("rpm_preflight", "ubuntu", "debian", "wrappers", "snap",
                         "packages", "snap_desktops", "attest"))
    if profile == "FULL" and outputs.get("cold") == "true":
        required.add("reproducibility")
    failed = sorted(name for name in required if needs.get(name, {}).get("result") != "success")
    failed += sorted(name for name, job in needs.items()
                     if name not in required and job.get("result") not in {"success", "skipped"})
    if failed:
        raise ValueError("Required CI verification did not pass: " + ", ".join(failed))
    # Draft/branch feedback never emits an eligible success for the merge-required gate.
    if outputs.get("merge_eligible") == "true" and profile not in {"FULL", "DOCS"}:
        raise ValueError("Reduced feedback does not satisfy merge verification")
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
