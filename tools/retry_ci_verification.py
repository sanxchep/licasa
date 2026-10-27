#!/usr/bin/env python3
"""Retry failed platform checks against unchanged packages and retained passing evidence."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from check_support_manifest import ROOT, load_manifest
from ci_plan import package_matrix
from hosted_candidate import source_version

# Reusing binaries is safe only when every change concerns this verification
# harness. Runtime, build recipes, fixtures and the support contract must match.
VERIFIER_FILES = {
    ".github/workflows/ci.yml", ".github/workflows/qualify-candidate.yml",
    ".github/workflows/qualify-packages.yml", ".github/workflows/retry-verification.yml",
    "tools/retry_ci_verification.py", "tools/qualification_evidence.py",
    "tools/verify_mate_accessibility_container.py", "tools/verify_snap_desktop.py",
    "tests/test_retry_ci_verification.py", "tests/test_qualification_evidence.py",
    "tests/test_mate_accessibility_wrapper.py", "tests/test_snap_dialog_qualification.py",
}
LEGACY_COMMON_BUILD_JOBS = {"plan", "package_contract", "ubuntu / build", "debian / build", "wrappers", "snap"}
COMMON_BUILD_JOBS = LEGACY_COMMON_BUILD_JOBS | {"Public file and history contract", "rpm_preflight"}
BUILD_JOBS = COMMON_BUILD_JOBS | {"test / core",
    "test / Validate source verification request", "test / Shared Qt runtime",
    "test / test (release)", "test / test (asan)", "test / test (ubsan)",
    "test / Source verification complete",
}
# Retained candidates created before source verification became reusable remain
# eligible, but only when their complete original graph and source gates match.
LEGACY_BUILD_JOBS = LEGACY_COMMON_BUILD_JOBS | {
    "Shared Qt runtime", "test (release)", "test (asan)", "test (ubsan)",
}
PACKAGE_ARTIFACTS = {"verification-ubuntu-packages", "verification-debian-packages",
                     "verification-portable-packages", "verification-snap"}


def validate_verifier(candidate: str, verifier: str) -> list[str]:
    if not all(re.fullmatch(r"[0-9a-f]{40}", value) for value in (candidate, verifier)):
        raise ValueError("Expected full candidate and verifier commit identities")
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if actual != verifier:
        raise ValueError("The verifier checkout does not match its requested commit")
    if subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=normal"],
                               cwd=ROOT, text=True).strip():
        raise ValueError("Retry verification requires a clean verifier checkout")
    subprocess.run(["git", "merge-base", "--is-ancestor", candidate, verifier], cwd=ROOT, check=True)
    paths = subprocess.check_output(
        ["git", "diff", "--no-renames", "--name-only", "-z", candidate, verifier, "--"], cwd=ROOT)
    changed = sorted(value.decode() for value in paths.split(b"\0") if value)
    if set(changed) - VERIFIER_FILES:
        raise ValueError("Candidate inputs changed; a full build is required: "
                         + ", ".join(sorted(set(changed) - VERIFIER_FILES)))
    return changed


def retry_plan(run: dict, jobs: list[dict], artifacts: list[dict], *, repository: str,
               branch: str, plan: dict | None = None) -> dict:
    manifest = load_manifest()
    if (run.get("repository", {}).get("full_name") != repository
            or run.get("path") != ".github/workflows/ci.yml"
            or run.get("event") not in {"workflow_dispatch", "push", "schedule"} or run.get("status") != "completed"
            or run.get("conclusion") != "failure" or run.get("head_branch") != branch
            or not re.fullmatch(r"[0-9a-f]{40}", run.get("head_sha", ""))):
        raise ValueError("Retry requires a completed failed manual verification on this repository and branch")
    if any(job.get("status") != "completed" for job in jobs):
        raise ValueError("Previous verification still has unfinished jobs")
    by_name = {job["name"]: job for job in jobs}
    if len(by_name) != len(jobs):
        raise ValueError("Duplicate previous job identities")
    # A normal candidate run explicitly skips the retry branch of ci.yml. It
    # remains in the Actions job list, but is not an omitted verification gate.
    # Accept only this exact routing shape; every other skipped job still fails.
    routing = by_name.pop("retry", None)
    if routing is None or routing.get("conclusion") != "skipped":
        raise ValueError("Previous verification must have skipped only its retry routing job")
    build_jobs = next((required for required in (BUILD_JOBS, LEGACY_BUILD_JOBS)
                       if required <= by_name.keys()), None)
    if build_jobs is None or any(by_name[name].get("conclusion") != "success" for name in build_jobs):
        raise ValueError("Every original build and source check must have succeeded before retrying platforms")
    cold = by_name.pop("reproducibility", None)
    if build_jobs == BUILD_JOBS:
        if (plan is None or plan.get("cold") != "false" or cold is None
                or cold.get("conclusion") != "skipped"):
            raise ValueError("Cold comparison campaigns require a full rerun; warm retry needs its exact skipped comparison route")
    packages = package_matrix(manifest)
    names = {f"packages ({row['target']}, {row['artifact']})": row for row in packages}
    desktops = {f"snap_desktops ({row['desktop']})": row["desktop"]
                for row in manifest["targets"] if row.get("desktop")}
    final_names = {"Required verification complete", "Verification feedback complete"} & by_name.keys()
    if len(final_names) != 1 or by_name.keys() != build_jobs | names.keys() | desktops.keys() | {"attest"} | final_names:
        raise ValueError("Previous run did not execute the complete expected verification graph")
    final_name = next(iter(final_names))
    if build_jobs == BUILD_JOBS:
        if (not isinstance(plan, dict) or plan.get("profile") != "FULL"
                or plan.get("complete") != "true" or plan.get("sha") != run["head_sha"]
                or plan.get("run_id") != str(run.get("id"))
                or plan.get("run_attempt") != str(run.get("run_attempt"))):
            raise ValueError("Retry requires the exact original FULL plan and attempt")
    if any(by_name[name].get("conclusion") not in {"success", "failure"}
           for name in names.keys() | desktops.keys() | {"attest", final_name}):
        raise ValueError("Cancelled, skipped or missing checks cannot be reused")
    failed_packages = [row for name, row in names.items() if by_name[name]["conclusion"] == "failure"]
    failed_desktops = [desktop for name, desktop in desktops.items() if by_name[name]["conclusion"] == "failure"]
    if not failed_packages and not failed_desktops:
        raise ValueError("No failed platform checks to retry")
    available = [row["name"] for row in artifacts if not row.get("expired")]
    required_artifacts = PACKAGE_ARTIFACTS | {"licasa-verification-evidence"}
    if build_jobs == BUILD_JOBS:
        required_artifacts.add("verification-plan")
    for name in required_artifacts:
        if available.count(name) != 1:
            raise ValueError(f"Missing, expired or ambiguous retained artifact: {name}")
    return {"sha": run["head_sha"], "matrix": failed_packages, "desktops": failed_desktops,
            "targets": [row["target"] for row in failed_packages]
                       + [f"ubuntu24-snap-{desktop}" for desktop in failed_desktops]}


def prepare_evidence(evidence: Path, targets: list[str], source: str, build_run: str) -> None:
    """Remove only failed receipts; successful evidence remains byte-for-byte intact."""
    manifest = load_manifest()
    expected = {row["id"] for row in manifest["targets"]}
    if len(set(targets)) != len(targets) or not set(targets).issubset(expected):
        raise ValueError("Invalid retry target selection")
    receipts = {path.parent.name: json.loads(path.read_text())
                for path in evidence.glob("*/outcome.json")}
    if receipts.keys() != expected:
        raise ValueError("Retained evidence must contain every original target")
    for target, row in receipts.items():
        if row.get("source_sha") != source or row.get("build_run_id") != build_run:
            raise ValueError("Retained evidence describes a different candidate build")
        if (target in targets) != (row.get("status") == "fail"):
            raise ValueError("Retry selection must exactly match the retained failed outcomes")
    for target in targets:
        directory = evidence / target
        if directory.is_symlink():
            raise ValueError("Refusing a linked evidence directory")
        shutil.rmtree(directory)
    (evidence / "qualification.json").unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    plan = sub.add_parser("plan")
    plan.add_argument("--run", type=Path, required=True)
    plan.add_argument("--jobs", type=Path, required=True)
    plan.add_argument("--artifacts", type=Path, required=True)
    plan.add_argument("--github-output", type=Path, required=True)
    plan.add_argument("--plan", type=Path)
    prepare = sub.add_parser("prepare-evidence")
    prepare.add_argument("--evidence", type=Path, required=True)
    prepare.add_argument("--targets", required=True)
    prepare.add_argument("--source-sha", required=True)
    prepare.add_argument("--build-run-id", required=True)
    args = parser.parse_args()
    if args.command == "prepare-evidence":
        prepare_evidence(args.evidence, json.loads(args.targets), args.source_sha, args.build_run_id)
        return
    run = json.loads(args.run.read_text())
    jobs = json.loads(args.jobs.read_text())
    artifacts = json.loads(args.artifacts.read_text())
    if (str(run.get("id")) != os.environ["BUILD_RUN_ID"]
            or jobs.get("total_count") != len(jobs["jobs"])
            or artifacts.get("total_count") != len(artifacts["artifacts"])):
        raise ValueError("Run metadata is mismatched or incomplete; no checks can be reused")
    result = retry_plan(run, jobs["jobs"], artifacts["artifacts"],
                        repository=os.environ["GITHUB_REPOSITORY"], branch=os.environ["GITHUB_REF_NAME"],
                        plan=json.loads(args.plan.read_text()) if args.plan else None)
    validate_verifier(result["sha"], os.environ["GITHUB_SHA"])
    def original(path):
        return subprocess.check_output(["git", "show", f"{result['sha']}:{path}"], cwd=ROOT, text=True)
    result["version"] = source_version(original("CMakeLists.txt"), original("snap/snapcraft.yaml"))
    with args.github_output.open("a") as output:
        output.writelines(f"{key}={json.dumps(value) if isinstance(value, list) else value}\n"
                          for key, value in result.items())


if __name__ == "__main__":
    main()
