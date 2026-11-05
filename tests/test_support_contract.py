#!/usr/bin/env python3
"""Keep distro claims attached to actual ABI recipes and executable qualifiers."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import check_support_manifest as support
import qualification_evidence as evidence
from package_reader_checks import READER_SAMPLES
from verify_dialog_interactions import REQUIRED_SCENARIOS
from verify_native_dialog_driver import REQUIRED_SCENARIOS as NATIVE_SCENARIOS


class SupportContractTests(unittest.TestCase):
    def setUp(self):
        self.manifest = support.load_manifest()

    def test_actual_build_profiles_match_the_contract(self):
        support.check_recipes(self.manifest)

    def test_readme_support_table_is_generated_from_contract(self):
        readme = (ROOT / "README.md").read_text()
        expected = f"{support.BEGIN}\n{support.readme_table(self.manifest)}\n{support.END}"
        self.assertIn(expected, readme)

    def test_release_gate_cannot_drift_from_executed_dialog_and_reader_contracts(self):
        self.assertEqual(evidence.REQUIRED_DIALOG_SCENARIOS, REQUIRED_SCENARIOS | {"application_launch"})
        self.assertEqual(evidence.REQUIRED_NATIVE_DIALOG_SCENARIOS, NATIVE_SCENARIOS)
        self.assertEqual(evidence.REQUIRED_READER_FORMATS, set(READER_SAMPLES))

    def test_portable_targets_keep_ubuntu_qt_while_debian_native_is_separate(self):
        rows = {target["id"]: target for target in self.manifest["targets"]}
        self.assertEqual(rows["debian13-native"]["profile"], "debian-6.8.2")
        portable = self.manifest["profiles"]["appimage-6.4.2"]
        self.assertEqual(portable["runtime_profile"], "ubuntu-6.4.2")
        self.assertEqual(portable["glibc_minimum"], "2.35")
        for row in rows.values():
            if row["artifact"] in {"appimage", "rpm", "arch"}:
                self.assertEqual(row["profile"], "appimage-6.4.2")
        self.assertEqual(rows["ubuntu22-appimage"]["status"], "supported")
        self.assertEqual(rows["ubuntu22-native-rejection"]["profile"], "ubuntu-6.4.2")
        self.assertNotEqual(self.manifest["artifacts"]["ubuntu-deb"], self.manifest["artifacts"]["debian-deb"])

    def test_qt_lock_change_cannot_silently_change_support_claims(self):
        changed = copy.deepcopy(self.manifest)
        changed["profiles"]["ubuntu-6.4.2"]["qt"] = "6.8.2"
        with self.assertRaisesRegex(ValueError, "Qt disagrees"):
            support.check_recipes(changed)

    def test_distro_recipe_change_requires_contract_review(self):
        changed = copy.deepcopy(self.manifest)
        changed["profiles"]["ubuntu-6.4.2"]["build_image"] = "ubuntu:26.04"
        with self.assertRaisesRegex(ValueError, "actual build recipe"):
            support.check_recipes(changed)

    def test_rejects_ambiguous_or_unsafe_contracts(self):
        for mutate in (
            lambda data: data["targets"].append(data["targets"][0]),
            lambda data: data["targets"][0].update(status="probably-supported"),
            lambda data: data["targets"][0].update(profile="unimplemented"),
            lambda data: data["artifacts"].update(appimage="../{version}"),
            lambda data: data["targets"][0].update(desktop="gnome"),
        ):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as temporary:
                changed = copy.deepcopy(self.manifest)
                mutate(changed)
                path = Path(temporary) / "support.json"
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    support.load_manifest(path)


if __name__ == "__main__":
    unittest.main()
