#!/usr/bin/env python3
"""Preserve CI coverage while scheduling shared builds and independent package checks."""

import copy
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from build_ci_dependencies import command_for
from check_support_manifest import load_manifest
from ci_plan import check_results, make_plan, package_matrix

JOBS = {"plan", "public_files", "rpm_preflight", "package_contract", "test", "ubuntu", "debian", "wrappers",
        "snap", "packages", "snap_desktops", "attest"}


def results(plan):
    rows = {name: {"result": "success"} for name in JOBS}
    rows["plan"]["outputs"] = plan
    return rows


class CiPlanTests(unittest.TestCase):
    def test_full_manual_request_preserves_every_source_and_platform_check(self):
        plan = make_plan("workflow_dispatch", True, [])
        self.assertEqual(json.loads(plan["presets"]), ["release", "asan", "ubsan"])
        self.assertEqual(plan["packages"], "true")
        self.assertEqual(set(check_results(results(plan))), JOBS)
        for preset in ('["asan","ubsan"]', '["release"]', '["release","asan"]'):
            with self.subTest(preset=preset), self.assertRaises(ValueError):
                make_plan("workflow_dispatch", True, [], preset)

    def test_docs_pr_keeps_contracts_without_unneeded_builds(self):
        plan = make_plan("pull_request", False, ["README.md", "LICENSE"])
        self.assertEqual(plan["build"], "false")
        self.assertEqual(plan["packages"], "false")
        rows = results(plan)
        for name in JOBS - {"plan", "public_files", "package_contract"}:
            rows[name]["result"] = "skipped"
        self.assertEqual(set(check_results(rows)), {"plan", "public_files", "package_contract"})

    def test_runtime_pr_requires_the_full_source_and_platform_contract(self):
        for path in ("src/main.cpp", "qml/OpenDialog.qml", "packaging/build-deb.Dockerfile",
                     ".github/workflows/ci.yml", "tools/ci_plan.py", "tests/test_ci_plan.py"):
            with self.subTest(path=path):
                plan = make_plan("pull_request", False, [path])
                self.assertEqual(plan["complete"], "true")
                self.assertEqual(json.loads(plan["presets"]), ["release", "asan", "ubsan"])
                self.assertEqual(set(check_results(results(plan))), JOBS)

    def test_explicit_feedback_cannot_select_only_sanitizers(self):
        plan = make_plan("workflow_dispatch", False, [], profile="FAST")
        self.assertEqual(json.loads(plan["presets"]), ["release"])
        self.assertEqual(plan["packages"], "false")
        with self.assertRaises(ValueError):
            make_plan("workflow_dispatch", False, [], '["asan","ubsan"]', profile="FAST")

    def test_invalid_requests_fail_before_building(self):
        for selected in ('[]', '["release","release"]', '["unknown"]', '"release"', '[null]'):
            with self.subTest(selected=selected), self.assertRaises(ValueError):
                make_plan("workflow_dispatch", False, [], selected)
        with self.assertRaises(ValueError):
            make_plan("pull_request", True, ["src/main.cpp"], profile="unknown")

    def test_every_missing_failed_cancelled_or_skipped_full_gate_fails_completion(self):
        good = results(make_plan("workflow_dispatch", True, []))
        for name in JOBS:
            for status in ("failure", "cancelled", "skipped", None):
                with self.subTest(job=name, status=status), self.assertRaises(ValueError):
                    changed = copy.deepcopy(good)
                    if status is None:
                        changed.pop(name)
                    else:
                        changed[name]["result"] = status
                    check_results(changed)

    def test_missing_or_contradictory_plan_cannot_turn_skips_into_success(self):
        good = results(make_plan("workflow_dispatch", True, []))
        for key in ("build", "packages", "complete"):
            changed = copy.deepcopy(good)
            del changed["plan"]["outputs"][key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                check_results(changed)
        for key in ("build", "packages"):
            changed = copy.deepcopy(good)
            changed["plan"]["outputs"][key] = "false"
            with self.subTest(key=key), self.assertRaises(ValueError):
                check_results(changed)

    def test_only_needed_artifact_is_downloaded_without_dropping_targets(self):
        manifest = load_manifest()
        selected = package_matrix(manifest)
        self.assertEqual({row["target"] for row in selected},
                         {row["id"] for row in manifest["targets"] if row.get("qualifier")})
        self.assertEqual(len(selected) + sum(bool(row.get("desktop")) for row in manifest["targets"]),
                         len(manifest["targets"]))
        for target in manifest["targets"]:
            if not target.get("qualifier"):
                continue
            actual = next(row["artifact"] for row in selected if row["target"] == target["id"])
            expected = ("verification-debian-packages" if target["artifact"] == "debian-deb"
                        else "verification-portable-packages" if target["artifact"] in {"rpm", "arch"}
                        else "verification-ubuntu-packages")
            self.assertEqual(actual, expected)

    def test_shared_quick_is_identical_but_codec_sanitizers_stay_distinct(self):
        self.assertEqual(command_for("quick", "release", 3), command_for("quick", "asan", 3))
        self.assertEqual(command_for("quick", "release", 3), command_for("quick", "ubsan", 3))
        release = command_for("codecs", "release", 3)
        self.assertNotIn("--sanitizer", release)
        for preset, sanitizer in (("asan", "address"), ("ubsan", "undefined")):
            command = command_for("codecs", preset, 3)
            self.assertEqual(command[command.index("--sanitizer") + 1], sanitizer)
        with self.assertRaises(ValueError):
            command_for("codecs", "asan", 0)

    def test_graph_retains_all_final_dependencies_and_parallel_native_builds(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        final = workflow.split("  result:\n", 1)[1]
        dependency_names = set(re.search(r"needs: \[([^]]+)\]", final)[1].replace(" ", "").split(","))
        self.assertEqual(dependency_names, JOBS | {"retry", "reproducibility"})
        for name in ("ubuntu", "debian"):
            block = workflow.split(f"  {name}:\n", 1)[1].split("\n  ", 1)[0]
            self.assertIn("needs: [plan, package_contract, rpm_preflight]", block)
            self.assertNotIn("test", re.search(r"needs: (.*)", block)[1])
        for removed in ("candidate_request", "candidate_verification", "candidate_result", "scope"):
            self.assertNotIn(f"\n  {removed}:\n", workflow)
        source = workflow.split("  test:\n", 1)[1].split("\n  ubuntu:\n", 1)[0]
        self.assertIn("uses: ./.github/workflows/verify-source.yml", source)
        self.assertIn("source_ref: ${{ needs.plan.outputs.sha }}", source)
        self.assertIn("presets: ${{ needs.plan.outputs.presets }}", source)
        self.assertNotIn("\n  quick:\n", workflow)
        self.assertFalse((ROOT / ".github/workflows/verify-candidate.yml").exists())
        self.assertFalse((ROOT / ".github/workflows/package-interactions.yml").exists())


if __name__ == "__main__":
    unittest.main()
