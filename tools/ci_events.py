#!/usr/bin/env python3
"""Route repository events without granting publication authority."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request

from check_support_manifest import load_manifest
from ci_plan import make_plan, package_matrix
from hosted_candidate import source_version, validate_requested_source

NIGHTLY = "17 2 * * 1-6"
WEEKLY = "43 3 * * 0"
SHA = re.compile(r"[0-9a-f]{40}")
# The public repository permits README and release notes, not an arbitrary docs tree.
PROSE = {"README.md", "LICENSE"}
PRODUCT_CLAIMS = re.compile(
    r"support|requirement|version|release|ubuntu|debian|fedora|arch linux|snap|\d+\.\d+", re.I)


def docs_only(changes: list[dict]) -> bool:
    return bool(changes) and all(
        row.get("path") in PROSE and row.get("status") == "M"
        and isinstance(row.get("patch"), str) and bool(row["patch"])
        and not PRODUCT_CLAIMS.search(row["patch"])
        for row in changes)


def enforce_coverage(policy: dict, candidate: dict) -> None:
    """A PR may add targets; removing or weakening reviewed coverage needs a new policy."""
    proposed = {row['id']: row for row in candidate['targets']}
    for target in policy['targets']:
        row = proposed.get(target['id'])
        fields = ('artifact', 'profile', 'status', 'qualifier', 'desktop', 'contract')
        if row is None or any(row.get(key) != target.get(key) for key in fields):
            raise ValueError('PR cannot reduce or redefine reviewed qualification coverage: ' + target['id'])
    for name, definition in policy.get('profiles', {}).items():
        if candidate.get('profiles', {}).get(name) != definition:
            raise ValueError('PR cannot redefine a reviewed runtime profile: ' + name)
    for name, path in policy['artifacts'].items():
        if candidate['artifacts'].get(name) != path:
            raise ValueError('PR cannot redefine reviewed package identity: ' + name)


def event_profile(event: str, ref: str, payload: dict, *, requested="FULL",
                  open_pr: bool | None = None, reusable=False) -> dict:
    """Unknown events/ambiguous changes cannot silently qualify reduced work."""
    profile, reason, eligible, cold = "NONE", "metadata-only event", False, False
    if reusable:
        if requested not in {"FULL", "FAST"}:
            raise ValueError("Reusable verification requires FULL or FAST")
        profile, reason = requested, "explicit reusable verification"
    elif event == "push":
        if payload.get("deleted"):
            if ref == "refs/heads/main" or ref.startswith("refs/tags/v"):
                raise ValueError("Protected main/version refs cannot be deleted")
        elif ref == "refs/heads/main":
            profile, reason, eligible = "FULL", "every main push", True
        elif ref.startswith("refs/tags/v"):
            if not re.fullmatch(r"refs/tags/v\d+\.\d+\.\d+", ref) or payload.get("forced"):
                raise ValueError("Invalid or moved release tag")
            profile, reason = "FULL", "verify exact release tag without publishing"
        elif ref.startswith("refs/heads/"):
            profile = "NONE" if open_pr is True else "FAST"
            reason = "open PR owns verification" if open_pr is True else "branch feedback (including lookup failure)"
    elif event == "pull_request":
        action = payload.get("action")
        pr = payload.get("pull_request", {})
        if action == "edited" and "base" not in payload.get("changes", {}):
            pass
        elif action in {"opened", "synchronize", "reopened", "ready_for_review",
                        "converted_to_draft", "edited"}:
            eligible = not pr.get("draft", True) and pr.get("base", {}).get("ref") == "main"
            profile = "FULL" if eligible else "FAST"
            reason = "ready PR merge result" if eligible else "draft/non-main PR feedback"
        elif action != "closed":
            raise ValueError("Unsupported PR action")
    elif event == "merge_group":
        if payload.get("action") != "checks_requested":
            raise ValueError("Unsupported merge-group action")
        if payload.get("merge_group", {}).get("base_ref") != "refs/heads/main":
            raise ValueError("Unsupported merge-group base")
        profile, reason, eligible = "FULL", "exact merge-group result", True
    elif event == "schedule":
        if ref != "refs/heads/main" or payload.get("schedule") not in {NIGHTLY, WEEKLY}:
            raise ValueError("Unexpected scheduled request")
        profile, reason = "FULL", "scheduled main drift verification"
        cold = payload["schedule"] == WEEKLY
    elif event == "workflow_dispatch":
        if requested not in {"FULL", "FAST"}:
            raise ValueError("Choose FULL or FAST explicitly")
        profile, reason = requested, "explicit pinned manual verification"
    else:
        raise ValueError("Unsupported verification event")
    return {"profile": profile, "reason": reason, "merge_eligible": str(eligible).lower(),
            "cold": str(cold).lower()}


def open_pull_request(repository: str, branch: str, token: str) -> bool | None:
    """A failed read conservatively retains branch feedback; never writes GitHub."""
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository):
        raise ValueError("Invalid repository identity")
    query = urllib.parse.urlencode({"state": "open", "head": repository.split('/')[0] + ':' + branch,
                                    "per_page": 100})
    request = urllib.request.Request(f"https://api.github.com/repos/{repository}/pulls?{query}",
                                     headers={"Accept": "application/vnd.github+json",
                                              "Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            rows = json.load(response)
        if not isinstance(rows, list):
            return None
        return any(row.get("state") == "open"
                   and row.get("head", {}).get("ref") == branch
                   and (row.get("head", {}).get("repo") or {}).get("full_name") == repository
                   for row in rows)
    except (urllib.error.URLError, ValueError, TimeoutError):
        return None


def git(root: Path, *args) -> str:
    return subprocess.check_output(["git", *args], cwd=root, text=True).strip()


def changes_for(root: Path, base: str, source: str) -> list[dict]:
    if not SHA.fullmatch(base) or not SHA.fullmatch(source):
        raise ValueError("Diff requires full base/source identities")
    raw = subprocess.check_output(["git", "diff", "--no-renames", "--name-status", "-z",
                                   base, source, "--"], cwd=root).split(b"\0")
    rows = []
    for index in range(0, len(raw) - 1, 2):
        status, path = raw[index].decode(), raw[index + 1].decode()
        patch = ""
        if path in PROSE and status == "M":
            # Inspect only changed lines; unchanged support claims do not disqualify prose edits.
            diff = git(root, "diff", "--no-ext-diff", "--no-textconv", "--unified=0", base, source, "--", path)
            patch = '\n'.join(line for line in diff.splitlines()
                              if line[:1] in {"+", "-"} and not line.startswith(("+++", "---")))
        rows.append({"status": status, "path": path, "patch": patch})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    env = os.environ
    payload = json.loads(Path(env["GITHUB_EVENT_PATH"]).read_text())
    event, ref = env["GITHUB_EVENT_NAME"], env["GITHUB_REF"]
    source = env["SOURCE_SHA"]
    actual = git(root, "rev-parse", "HEAD")
    validate_requested_source(source, source, actual)
    if git(root, "status", "--porcelain"):
        raise ValueError("Verification source must be clean")
    reusable = env.get("REUSABLE", "false") == "true"
    if event == "workflow_dispatch" and not reusable:
        validate_requested_source(source, env.get("EXPECTED_SHA", ""), actual)
    lookup = None
    if event == "push" and ref.startswith("refs/heads/") and ref != "refs/heads/main":
        lookup = open_pull_request(env["GITHUB_REPOSITORY"], ref.removeprefix("refs/heads/"), env.get("GH_TOKEN", ""))
    routed = event_profile(event, ref, payload, requested=env.get("PROFILE", "FULL") or "FULL",
                           open_pr=lookup, reusable=reusable)
    if env.get("COLD") == "true" and (reusable or event == "workflow_dispatch"):
        routed["cold"] = "true"
    changes = []
    base = payload.get("pull_request", {}).get("base", {}).get("sha", "")
    if event == "pull_request" and routed["profile"] == "FULL":
        changes = changes_for(root, base, source)
        if docs_only(changes):
            routed.update(profile="DOCS", reason="allowlisted prose-only merge result")
    if ref.startswith("refs/tags/v") and routed["profile"] != "NONE":
        if git(root, "rev-parse", "origin/main") == source:
            pass
        else:
            subprocess.run(["git", "merge-base", "--is-ancestor", source, "origin/main"], cwd=root, check=True)
        version = source_version((root / "CMakeLists.txt").read_text(), (root / "snap/snapcraft.yaml").read_text())
        if ref != f"refs/tags/v{version}":
            raise ValueError("Release tag and source version differ")
    profile = routed["profile"]
    plan = make_plan(event, profile == "FULL", [r["path"] for r in changes], profile=profile)
    retention = 90 if profile == "FULL" and event != "pull_request" else 30 if routed["merge_eligible"] == "true" else 14
    policy = env["POLICY_SHA"]
    if not SHA.fullmatch(policy):
        raise ValueError("Expected an exact reviewed policy revision")
    manifest = load_manifest(root / "packaging/support-matrix.json")
    if event == "pull_request":
        enforce_coverage(load_manifest(), manifest)
    result = {**plan, **routed, "sha": source, "policy_sha": policy, "base_sha": base,
              "version": source_version((root / "CMakeLists.txt").read_text(), (root / "snap/snapcraft.yaml").read_text()),
              "matrix": json.dumps(package_matrix(manifest)),
              "retention": str(retention), "event": event, "ref": ref,
              "cache_scope": (f"pr-{payload.get('number')}" if event == "pull_request" else
                              "trusted" if ref == "refs/heads/main" or ref.startswith("refs/tags/v")
                              else "branch-" + hashlib.sha256(ref.encode()).hexdigest()[:16]),
              "run_id": env["GITHUB_RUN_ID"], "run_attempt": env["GITHUB_RUN_ATTEMPT"]}
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps(result, indent=2) + '\n')
    with args.output.open('a') as output:
        output.writelines(f"{k}={v}\n" for k, v in result.items())
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
