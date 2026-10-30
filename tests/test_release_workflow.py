#!/usr/bin/env python3
"""Guard release publication gates and execute artifact-transfer checks locally."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA = "a" * 40
VERSION = "0.9.0"


def dependencies(job):
    value = job.get("needs", [])
    return {value} if isinstance(value, str) else set(value)


def ancestors(jobs, name):
    found = dependencies(jobs[name])
    return found | set().union(*(ancestors(jobs, parent) for parent in found))


class ReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflows = {path.name: yaml.safe_load(path.read_text())
                         for path in (ROOT / ".github/workflows").glob("*.yml")}
        cls.jobs = cls.workflows["snapcraft-build.yml"]["jobs"]

    def test_publication_requires_complete_qualified_candidate_and_explicit_intent(self):
        for publication in ("upload_snap_edge", "github_prerelease"):
            self.assertTrue({"preflight", "verify_sanitizers", "stage_candidate"} <= ancestors(self.jobs, publication))
        self.assertIn("upload_snap_edge", ancestors(self.jobs, "github_prerelease"))
        for job in self.jobs.values():
            self.assertNotIn("if", job)
            self.assertFalse(job.get("continue-on-error", False))
        for job in ("upload_snap_edge", "github_prerelease"):
            self.assertEqual(self.jobs[job]["environment"], "snap-beta")
        call = self.jobs["verify_sanitizers"]
        self.assertEqual(call["uses"], "./.github/workflows/ci.yml")
        self.assertEqual(call["with"]["profile"], "FULL")
        scripts = "\n".join(step.get("run", "") for step in self.jobs["preflight"]["steps"])
        self.assertIn('test "$APPROVED" = true', scripts)
        self.assertIn('test "$GITHUB_SHA" = "$EXPECTED_SHA"', scripts)

    def test_release_consumes_the_same_run_packages_without_rebuilding(self):
        stage = self.jobs["stage_candidate"]
        self.assertEqual(dependencies(stage), {"preflight", "verify_sanitizers"})
        downloads = {step.get("with", {}).get("name") for step in stage["steps"]
                     if step.get("uses", "").startswith("actions/download-artifact@")}
        self.assertTrue({"verification-ubuntu-packages", "verification-debian-packages",
                         "verification-portable-packages", "verification-snap",
                         "licasa-verification-evidence", "verification-result"} <= downloads)
        scripts = "\n".join(step.get("run", "") for step in stage["steps"])
        self.assertIn("tools/ci_receipt.py check", scripts)
        self.assertIn("tools/qualification_evidence.py check", scripts)
        self.assertNotIn("package_rpm.py", scripts)
        self.assertNotIn("snapcraft upload", scripts)

    def test_output_references_have_direct_dependencies_in_every_workflow(self):
        for workflow, contents in self.workflows.items():
            for name, job in contents["jobs"].items():
                references = set(re.findall(r"\bneeds\.([a-zA-Z_][a-zA-Z_0-9]*)\.", str(job)))
                with self.subTest(workflow=workflow, job=name):
                    self.assertTrue(references <= dependencies(job), references - dependencies(job))

    def test_shared_graph_retains_native_hardening_rpm_and_all_qualifiers(self):
        ci = self.workflows["ci.yml"]["jobs"]
        self.assertIn("verify_native_lifecycle.py", str(ci["rpm_preflight"]))
        self.assertIn("qualification_evidence.py run-target", str(ci["packages"]))
        self.assertIn("qualification_evidence.py run-target", str(ci["snap_desktops"]))
        self.assertIn("verify_snap_install.py", str(ci["snap"]))
        native = self.workflows["build-native.yml"]["jobs"]["build"]
        self.assertIn("check_hardening.py /tmp/licasa-package/usr/bin/licasa.bin", str(native))
        for job in ("snap", "packages", "snap_desktops", "attest", "rpm_preflight"):
            self.assertTrue(any(step.get("if") == "always()" for step in ci[job]["steps"]))

    def test_complete_public_artifact_contract_is_retained(self):
        exports = {step["with"]["name"]: (job, step["with"]["path"])
                   for job, value in self.jobs.items() for step in value.get("steps", [])
                   if step.get("uses", "").startswith("actions/upload-artifact@")}
        self.assertEqual(exports["licasa-linux-amd64"][0], "stage_candidate")
        self.assertEqual(exports["licasa-debian13-amd64"][0], "stage_candidate")
        self.assertEqual(exports["licasa-snap-amd64"][0], "stage_candidate")
        scripts = exports["licasa-linux-amd64"][1]
        for directory in ("deb", "appimage", "rpm", "arch"):
            self.assertIn(f"dist/{directory}", scripts)
        for name in ("qualify-candidate.yml", "publish-release.yml"):
            downloads = {step.get("with", {}).get("name")
                         for job in self.workflows[name]["jobs"].values()
                         for step in job.get("steps", [])
                         if step.get("uses", "").startswith("actions/download-artifact@")}
            self.assertTrue({"licasa-linux-amd64", "licasa-debian13-amd64", "licasa-snap-amd64"} <= downloads)

    def test_full_source_regression_suite_runs_once_on_the_release_call_graph(self):
        scripts = []

        def visit(name):
            for job in self.workflows[name]["jobs"].values():
                scripts.extend(step.get("run", "") for step in job.get("steps", []))
                if job.get("uses", "").startswith("./.github/workflows/"):
                    visit(job["uses"].rsplit("/", 1)[-1])

        visit("snapcraft-build.yml")
        combined = "\n".join(scripts)
        for command in ("tools/run_python_tests.py", "tools/check_code_style.py",
                        "tools/check_support_manifest.py"):
            self.assertEqual(combined.count(command), 1, command)
        for script in ("packaging/install-licasa-appimage.sh", "tools/check_appimage_install.sh"):
            self.assertIn("tools/check_ci_inputs.py", combined)
            self.assertTrue((ROOT / script).is_file())


class ReleaseArtifactTransferTests(unittest.TestCase):
    """The new transfer boundary joins source gates with real package/report bytes."""

    def test_exact_package_evidence_and_full_receipt_must_both_pass_before_transfer(self):
        # Reuse the existing report-contract fixture; no package managers or
        # publishing commands run. The source gate is independently exercised
        # through real serialized receipts and file hashes.
        from test_qualification_evidence import QualificationEvidenceTests
        from ci_receipt import create, validate
        from ci_plan import make_plan
        from unittest.mock import patch
        import qualification_evidence as evidence
        fixture = QualificationEvidenceTests()
        fixture.setUp()
        try:
            plan = {**make_plan("push", True, []), "sha": "a" * 40, "policy_sha": "b" * 40,
                    "run_id": "123", "run_attempt": "1", "version": "1.2.3"}
            names = {"plan", "public_files", "package_contract", "rpm_preflight", "test",
                     "ubuntu", "debian", "wrappers", "snap", "packages", "snap_desktops", "attest"}
            needs = {name: {"result": "success"} for name in names}
            needs["plan"]["outputs"] = plan
            receipt = create(plan, needs, "owner/repo", "123", "1")
            validate(receipt, repository="owner/repo", source="a" * 40, version="1.2.3", run_id="123", attempt="1")
            arguments = dict(source_sha="a" * 40, build_run_id="123", version="1.2.3",
                             packages=fixture.packages, evidence=fixture.reports, manifest_path=fixture.manifest_path)
            with patch.object(evidence, "MANIFEST", fixture.manifest_path):
                evidence.validate(fixture.attestation, fixture.manifest, **arguments)
                for artifact in fixture.attestation["packages"].values():
                    path = fixture.packages / artifact["path"]
                    saved = path.read_bytes()
                    path.write_bytes(saved + b"changed after qualification")
                    with self.assertRaises(ValueError):
                        evidence.validate(fixture.attestation, fixture.manifest, **arguments)
                    path.write_bytes(saved)
                incomplete = dict(receipt, profile="FAST")
                with self.assertRaises(ValueError):
                    validate(incomplete, repository="owner/repo", source="a" * 40,
                             version="1.2.3", run_id="123", attempt="1")
        finally:
            fixture.doCleanups()


if __name__ == "__main__":
    unittest.main()
