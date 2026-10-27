#!/usr/bin/env python3
"""Guard exact source selection and prevent verification from publishing."""

from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from hosted_candidate import source_version, validate_requested_source


class HostedCandidateTests(unittest.TestCase):
    def test_requires_the_explicit_full_commit_and_matching_checkout(self):
        commit = "a" * 40
        validate_requested_source(commit, commit, commit)
        for selected, expected, actual in (
            (commit, "", commit), (commit, "main", commit),
            (commit, commit[:12], commit), (commit, "b" * 40, commit),
            (commit, commit, "b" * 40), ("main", commit, commit),
        ):
            with self.subTest(selected=selected, expected=expected, actual=actual), self.assertRaises(ValueError):
                validate_requested_source(selected, expected, actual)

    def test_candidate_version_must_match_every_built_package(self):
        cmake = "project(Licasa VERSION 0.8.0 LANGUAGES CXX)"
        self.assertEqual(source_version(cmake, "version: '0.8.0'\n"), "0.8.0")
        for snap in ("version: '0.8.1'\n", "version: latest\n", "", "version: '0.8.0'\nversion: '0.8.0'\n"):
            with self.subTest(snap=snap), self.assertRaises(ValueError):
                source_version(cmake, snap)

    def test_verification_has_no_publication_permissions_credentials_or_steps(self):
        workflow = "\n".join((ROOT / ".github/workflows" / name).read_text()
                             for name in ("ci.yml", "build-native.yml", "verify-source.yml"))
        for forbidden in (
            r"\b(?:contents|packages|deployments|id-token):\s*write\b",
            r"\bsecrets\b", r"^\s+environment:", r"\bsnapcraft\s+(?:upload|release)\b",
            r"\bgh\s+release\b", r"\bgit\s+(?:push|tag)\b", r"^\s+push:\s*true\b",
            r"publish-release\.yml", r"snapcraft-build\.yml",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertIsNone(re.search(forbidden, workflow, re.M))
        self.assertIn("permissions:\n  contents: read", workflow)
        self.assertIn("push: false", workflow)

    def test_complete_contract_is_retained_and_cannot_authorize_promotion(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        result = workflow.split("  result:\n", 1)[1]
        self.assertIn("always()", result)
        self.assertIn("CI_RESULTS: ${{ toJSON(needs) }}", result)
        self.assertIn("python3 tools/ci_plan.py check", result)
        self.assertIn("SOURCE_SHA: ${{ inputs.source_ref || (github.event_name == 'workflow_dispatch' && inputs.expected_sha) || github.sha }}", workflow)
        self.assertIn("EXPECTED_SHA: ${{ inputs.expected_sha }}", workflow)
        self.assertIn("${{ fromJSON(needs.plan.outputs.matrix) }}", workflow)
        self.assertIn("desktop: [gnome, kde]", workflow)
        self.assertIn("tools/qualification_evidence.py run-target", workflow)
        self.assertIn("tools/qualification_evidence.py assemble", workflow)
        self.assertIn("--build-run-id \"$BUILD_RUN_ID\"", workflow)
        self.assertIn("--source-sha \"$SOURCE_SHA\"", workflow)
        self.assertIn("name: licasa-verification-evidence", workflow)
        self.assertNotIn("name: licasa-candidate-qualification", workflow)
        promotion = (ROOT / ".github/workflows/publish-release.yml").read_text()
        self.assertIn("'.github/workflows/qualify-candidate.yml'", promotion)
        self.assertNotIn("'.github/workflows/ci.yml'", promotion)


if __name__ == "__main__":
    unittest.main()
