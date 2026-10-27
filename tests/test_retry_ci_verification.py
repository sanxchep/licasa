#!/usr/bin/env python3
"""Reuse successful candidate checks only when package inputs remain identical."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import retry_ci_verification as retry


class RetryVerificationTests(unittest.TestCase):
    def current_build_jobs(self):
        """Build API job names from the workflows, independently of retry's sets."""
        workflows = ROOT / ".github/workflows"
        ci = yaml.safe_load((workflows / "ci.yml").read_text())
        source = yaml.safe_load((workflows / "verify-source.yml").read_text())
        call = ci["jobs"]["test"]
        self.assertEqual(call["uses"], "./.github/workflows/verify-source.yml")
        self.assertEqual(call["with"]["presets"], "${{ needs.plan.outputs.presets }}")
        # PyYAML treats the YAML 1.1 key "on" as True.
        inputs = ci.get("on", ci.get(True))["workflow_call"]["inputs"]
        presets = json.loads(inputs["presets"]["default"])
        self.assertEqual(set(presets), {"release", "asan", "ubsan"})
        names = set()
        for job_id in ("plan", "package_contract", "ubuntu", "debian", "wrappers", "snap"):
            job = ci["jobs"][job_id]
            name = job.get("name", job_id)
            if "uses" in job:
                child = yaml.safe_load((workflows / job["uses"].rsplit("/", 1)[-1]).read_text())
                names.update(f"{name} / {value.get('name', child_id)}"
                             for child_id, value in child["jobs"].items())
            else:
                names.add(name)
        prefix = call.get("name", "test")
        for job_id, job in source["jobs"].items():
            name = f"{prefix} / {job.get('name', job_id)}"
            if "strategy" in job:
                self.assertEqual(job["strategy"]["matrix"],
                                 {"preset": "${{ fromJSON(needs.plan.outputs.presets) }}"})
                names.update(f"{name} ({preset})" for preset in presets)
            else:
                names.add(name)
        return names

    def setUp(self):
        self.run = {"repository": {"full_name": "owner/repo"}, "path": ".github/workflows/ci.yml",
                    "event": "workflow_dispatch", "status": "completed", "conclusion": "failure",
                    "head_branch": "candidate", "head_sha": "a" * 40}
        packages = retry.package_matrix(retry.load_manifest())
        self.failed = {"debian13-mate-accessibility", "ubuntu24-snap-gnome", "ubuntu24-snap-kde"}
        names = self.current_build_jobs() | {f"packages ({row['target']}, {row['artifact']})" for row in packages}
        names |= {"snap_desktops (gnome)", "snap_desktops (kde)", "attest", "Required verification complete"}
        self.jobs = [{"name": name, "status": "completed", "conclusion": "failure" if (
            "mate-accessibility" in name or name.startswith("snap_desktops")
            or name in {"attest", "Required verification complete"}) else "success"} for name in sorted(names)]
        self.jobs.append({"name": "retry", "status": "completed", "conclusion": "skipped"})
        self.artifacts = [{"name": name, "expired": False}
                          for name in retry.PACKAGE_ARTIFACTS | {"licasa-verification-evidence"}]

    def plan(self):
        return retry.retry_plan(self.run, self.jobs, self.artifacts, repository="owner/repo", branch="candidate")

    def test_selects_only_failed_platforms_and_preserves_original_candidate(self):
        plan = self.plan()
        self.assertEqual(plan["sha"], "a" * 40)
        self.assertEqual(plan["matrix"], [{"target": "debian13-mate-accessibility",
                                          "artifact": "verification-ubuntu-packages"}])
        self.assertEqual(set(plan["desktops"]), {"gnome", "kde"})
        self.assertEqual(set(plan["targets"]), self.failed)

    def test_current_workflow_names_include_every_nested_source_gate(self):
        self.assertEqual(self.current_build_jobs(), retry.BUILD_JOBS)

    def test_exact_legacy_graph_remains_eligible_and_rejects_incomplete_or_mixed_graphs(self):
        legacy = {"plan", "package_contract", "Shared Qt runtime", "ubuntu / build", "debian / build",
                  "test (release)", "test (asan)", "test (ubsan)", "wrappers", "snap"}
        self.assertEqual(legacy, retry.LEGACY_BUILD_JOBS)
        jobs = [row for row in self.jobs if row["name"] not in retry.BUILD_JOBS]
        jobs += [{"name": name, "status": "completed", "conclusion": "success"} for name in sorted(legacy)]
        plan = retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")
        self.assertEqual(set(plan["targets"]), self.failed)
        for name in legacy:
            for status in ("failure", "skipped", "cancelled", None):
                with self.subTest(name=name, status=status):
                    changed = copy.deepcopy(jobs)
                    if status is None:
                        changed = [row for row in changed if row["name"] != name]
                    else:
                        next(row for row in changed if row["name"] == name)["conclusion"] = status
                    with self.assertRaises(ValueError):
                        retry.retry_plan(self.run, changed, self.artifacts,
                                         repository="owner/repo", branch="candidate")
        mixed = jobs + [{"name": "test / Source verification complete", "status": "completed",
                         "conclusion": "success"}]
        with self.assertRaisesRegex(ValueError, "expected verification graph"):
            retry.retry_plan(self.run, mixed, self.artifacts, repository="owner/repo", branch="candidate")

    def test_failed_skipped_missing_or_duplicated_source_checks_prevent_reuse(self):
        for name in retry.BUILD_JOBS:
            for status in ("failure", "skipped", "cancelled", None):
                with self.subTest(name=name, status=status):
                    jobs = copy.deepcopy(self.jobs)
                    if status is None:
                        jobs = [row for row in jobs if row["name"] != name]
                    else:
                        next(row for row in jobs if row["name"] == name)["conclusion"] = status
                    with self.assertRaises(ValueError):
                        retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")
        for jobs in (self.jobs[1:], self.jobs + [self.jobs[0]]):
            with self.assertRaises(ValueError):
                retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")

    def test_only_the_completed_skipped_retry_router_is_accepted(self):
        self.plan()
        for conclusion in ("success", "failure", "cancelled", None):
            with self.subTest(conclusion=conclusion):
                jobs = copy.deepcopy(self.jobs)
                next(row for row in jobs if row["name"] == "retry")["conclusion"] = conclusion
                with self.assertRaisesRegex(ValueError, "retry routing"):
                    retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")
        jobs = [row for row in self.jobs if row["name"] != "retry"]
        with self.assertRaisesRegex(ValueError, "retry routing"):
            retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")
        jobs = copy.deepcopy(self.jobs)
        next(row for row in jobs if row["name"] == "retry")["status"] = "in_progress"
        with self.assertRaisesRegex(ValueError, "unfinished"):
            retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")

    def test_unknown_skipped_jobs_and_skipped_platform_checks_still_reject(self):
        jobs = self.jobs + [{"name": "unexpected", "status": "completed", "conclusion": "skipped"}]
        with self.assertRaisesRegex(ValueError, "expected verification graph"):
            retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")
        for row in self.jobs:
            if not row["name"].startswith(("packages (", "snap_desktops (")):
                continue
            with self.subTest(name=row["name"]):
                jobs = copy.deepcopy(self.jobs)
                next(job for job in jobs if job["name"] == row["name"])["conclusion"] = "skipped"
                with self.assertRaisesRegex(ValueError, "skipped or missing"):
                    retry.retry_plan(self.run, jobs, self.artifacts, repository="owner/repo", branch="candidate")

    def test_wrong_run_or_branch_and_expired_artifacts_are_rejected(self):
        for key, value in (("head_branch", "other"), ("path", ".github/workflows/other.yml"),
                           ("status", "in_progress"), ("conclusion", "success"), ("event", "pull_request")):
            with self.subTest(key=key):
                run = dict(self.run, **{key: value})
                with self.assertRaises(ValueError):
                    retry.retry_plan(run, self.jobs, self.artifacts, repository="owner/repo", branch="candidate")
        self.artifacts[0]["expired"] = True
        with self.assertRaises(ValueError):
            self.plan()

    def test_preparation_retains_passing_evidence_and_removes_exactly_failed_targets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for target in retry.load_manifest()["targets"]:
                directory = root / target["id"]
                directory.mkdir()
                row = {"source_sha": "a" * 40, "build_run_id": "123",
                       "status": "fail" if target["id"] in self.failed else "pass"}
                (directory / "outcome.json").write_text(json.dumps(row))
            before = {str(path.relative_to(root)): path.read_bytes() for path in root.rglob("*") if path.is_file()}
            with self.assertRaises(ValueError):
                retry.prepare_evidence(root, ["ubuntu24-native"], "a" * 40, "123")
            self.assertEqual(before, {str(path.relative_to(root)): path.read_bytes() for path in root.rglob("*") if path.is_file()})
            retry.prepare_evidence(root, sorted(self.failed), "a" * 40, "123")
            for path, content in before.items():
                if path.split("/")[0] in self.failed:
                    self.assertFalse((root / path).exists())
                else:
                    self.assertEqual((root / path).read_bytes(), content)

    def test_real_git_checkout_allows_only_harness_changes_and_rejects_dirty_or_runtime_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def git(*arguments):
                return subprocess.check_output(["git", *arguments], cwd=root, text=True).strip()
            git("init", "-q")
            git("config", "user.name", "Test")
            git("config", "user.email", "test@example.invalid")
            (root / "src").mkdir()
            (root / "src/main.cpp").write_text("original")
            git("add", "src/main.cpp")
            git("commit", "-qm", "candidate")
            candidate = git("rev-parse", "HEAD")
            (root / "tools").mkdir()
            (root / "tools/verify_snap_desktop.py").write_text("fixed")
            git("add", "tools/verify_snap_desktop.py")
            git("commit", "-qm", "harness")
            verifier = git("rev-parse", "HEAD")
            with patch.object(retry, "ROOT", root):
                self.assertEqual(retry.validate_verifier(candidate, verifier), ["tools/verify_snap_desktop.py"])
                (root / "untracked").touch()
                with self.assertRaisesRegex(ValueError, "clean"):
                    retry.validate_verifier(candidate, verifier)
                (root / "untracked").unlink()
                (root / "tools/retry_ci_verification.py").write_text("routing fix")
                (root / "tests").mkdir()
                (root / "tests/test_retry_ci_verification.py").write_text("routing regression")
                git("add", "tools/retry_ci_verification.py", "tests/test_retry_ci_verification.py")
                git("commit", "-qm", "retry planner")
                verifier = git("rev-parse", "HEAD")
                self.assertEqual(retry.validate_verifier(candidate, verifier), [
                    "tests/test_retry_ci_verification.py", "tools/retry_ci_verification.py",
                    "tools/verify_snap_desktop.py"])
                (root / "src/main.cpp").write_text("changed")
                git("add", "src/main.cpp")
                git("commit", "-qm", "runtime")
                with self.assertRaisesRegex(ValueError, "full build"):
                    retry.validate_verifier(candidate, git("rev-parse", "HEAD"))

    def test_retry_graph_has_no_build_jobs_and_never_publishes(self):
        workflow = (ROOT / ".github/workflows/retry-verification.yml").read_text()
        for forbidden in ("build-native.yml", "snapcore/action-build", "gh release", "snapcraft upload"):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('pattern: verification-*-packages', workflow)
        self.assertIn('--verification-sha "$GITHUB_SHA"', workflow)
        self.assertIn("fetch-depth: 0", workflow)


if __name__ == "__main__":
    unittest.main()
