#!/usr/bin/env python3
"""Unsupported installations require explicit, candidate-bound rejection proof."""
import copy
import hashlib
import json
import os
import re
import shlex
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from check_package_report import check_report
from qualification_evidence import validate_report
import verify_unsupported_ubuntu as rejection_probe


class RejectionReportTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((ROOT / "packaging/support-matrix.json").read_text())
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.package = Path(self.temporary.name) / "candidate.deb"
        self.package.write_bytes(b"immutable candidate package")
        self.package_hash = hashlib.sha256(self.package.read_bytes()).hexdigest()

    def target(self, version):
        return next(row for row in self.manifest["targets"]
                    if row["id"] == f"ubuntu{version}-native-rejection")

    def receipt(self, version):
        report = {"ubuntu_version": f"{version}.04",
                  "native_deb": "unsupported_dependency_rejection_confirmed",
                  "package_sha256": self.package_hash}
        if version == "22":
            report.update(appimage="unsupported_glibc_rejection_confirmed", appimage_sha256="a" * 64)
        return report

    def test_both_rejection_targets_use_the_authoritative_dispatch(self):
        for version in ("22", "26"):
            with self.subTest(version=version):
                report, target = self.receipt(version), self.target(version)
                validate_report(report, target, self.manifest)
                check_report(report, target, self.manifest, self.package)

    def test_another_candidate_cannot_supply_rejection_evidence(self):
        report = self.receipt("26")
        report["package_sha256"] = "b" * 64
        with self.assertRaisesRegex(ValueError, "different candidate"):
            check_report(report, self.target("26"), self.manifest, self.package)

    def test_native_rejection_marker_is_mandatory(self):
        for value in (None, "passed", "unsupported", "arbitrary dependency error"):
            report = self.receipt("26")
            report["native_deb"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                check_report(report, self.target("26"), self.manifest, self.package)

    def test_receipt_must_describe_the_requested_userspace(self):
        report = self.receipt("26")
        report["ubuntu_version"] = "22.04"
        with self.assertRaises(ValueError):
            check_report(report, self.target("26"), self.manifest, self.package)

    def test_ubuntu22_requires_explicit_appimage_baseline_rejection(self):
        for field in ("appimage", "appimage_sha256"):
            report = self.receipt("22")
            del report[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                check_report(report, self.target("22"), self.manifest, self.package)
        report = self.receipt("22")
        report["appimage_sha256"] = "invalid"
        with self.assertRaises(ValueError):
            check_report(report, self.target("22"), self.manifest, self.package)

    def test_nonpassing_report_cannot_override_explicit_receipt_fields(self):
        for status in ("failed", "skipped", "unsupported"):
            report = dict(self.receipt("26"), status=status)
            with self.subTest(status=status), self.assertRaises(ValueError):
                check_report(report, self.target("26"), self.manifest, self.package)

    def test_positive_dialog_report_cannot_substitute_for_rejection(self):
        report = {"status": "passed", "dialog_interactions": [], "package_sha256": self.package_hash}
        with self.assertRaises(ValueError):
            check_report(report, self.target("26"), self.manifest, self.package)

    def test_unknown_unsupported_contract_fails_closed(self):
        target = copy.deepcopy(self.target("26"))
        target["qualifier"][0] = "tools/other_verifier.py"
        with self.assertRaises(ValueError):
            check_report(self.receipt("26"), target, self.manifest, self.package)


class RejectionGuestTests(unittest.TestCase):
    def run_probe(self, directory, *, version="22.04", install_result=None):
        root=Path(directory);package=root/'candidate.deb';package.write_bytes(b'candidate deb')
        appimage=root/'candidate.AppImage';appimage.write_bytes(b'candidate appimage')
        output=root/'report.json';captured={}
        argv=['verify_unsupported_ubuntu.py',str(package),'--ubuntu-version',version,'--output',str(output)]
        if version=='22.04':argv+=['--appimage',str(appimage)]
        def run(command,**kwargs):
            if command[:2]==['docker','build']:
                captured['recipe']=(Path(command[-1])/'Dockerfile').read_text()
                return subprocess.CompletedProcess(command,0)
            if 'apt-get' in command:
                return subprocess.CompletedProcess(command,100,'qt6-base-abi (= 6.4.2) is not installable','')
            captured['installer']=command
            return subprocess.CompletedProcess(command,*(install_result or
                (1,"GLIBC_2.38: version not found\nInstallation stopped before replacing any existing Licasa application.",'')))
        with patch.object(sys,'argv',argv),patch.dict(os.environ,{'RUNNER_ENVIRONMENT':'github-hosted'}),patch.object(rejection_probe.subprocess,'run',side_effect=run):
            rejection_probe.main()
        return captured,json.loads(output.read_text()),package,appimage

    def test_real_ubuntu22_recipe_provides_every_installer_tool_before_abi_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            captured,report,package,appimage=self.run_probe(directory)
            providers=dict.fromkeys('uname cp chmod mkdir mktemp mv rm rmdir sha256sum dirname basename cat'.split(),'coreutils')
            providers.update(bash='bash',find='findutils',sed='sed',flock='util-linux',getconf='libc-bin',ldd='libc-bin',python3='python3')
            installer=(ROOT/'packaging/install-licasa-appimage.sh').read_text()
            required={'bash'}
            for declaration in re.findall(r'(?m)^\s*commands=\(([^\n]*)\)',installer):
                required.update(token for token in shlex.split(declaration) if not token.startswith('${'))
            self.assertFalse(required-providers.keys(),'Map any newly required installer tool to its Ubuntu package')
            command=next(line for line in captured['recipe'].splitlines() if line.startswith('RUN apt-get install'))
            installed=set(shlex.split(command)[2:])
            self.assertFalse({providers[name] for name in required}-installed,
                             'The clean rejection guest must reach the ABI check, not fail missing-tool preflight')
            self.assertNotIn('qt6-base-dev',installed)
            self.assertEqual(report['appimage'],'unsupported_glibc_rejection_confirmed')
            self.assertEqual(report['package_sha256'],hashlib.sha256(package.read_bytes()).hexdigest())
            self.assertEqual(report['appimage_sha256'],hashlib.sha256(appimage.read_bytes()).hexdigest())
            self.assertIn('--read-only',captured['installer']);self.assertIn('none',captured['installer'])

    def test_missing_tools_unrelated_errors_and_success_cannot_pass_rejection(self):
        for result in ((1,"Cannot install Licasa: the required system tool 'python3' is missing.",''),
                       (1,'GLIBC_2.38: version not found',''),
                       (1,'Installation stopped before replacing any existing Licasa application.',''),
                       (0,'GLIBC_2.38: version not found before replacing','')):
            with self.subTest(result=result),tempfile.TemporaryDirectory() as directory:
                with self.assertRaisesRegex(RuntimeError,'Expected an explicit glibc-baseline rejection'):
                    self.run_probe(directory,install_result=result)
                self.assertFalse((Path(directory)/'report.json').exists())

    def test_ubuntu26_native_only_probe_does_not_install_appimage_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            captured,report,_,_=self.run_probe(directory,version='26.04')
            self.assertNotIn('apt-get install',captured['recipe']);self.assertNotIn('installer',captured)
            self.assertNotIn('appimage',report)
            self.assertEqual(report['native_deb'],'unsupported_dependency_rejection_confirmed')


if __name__ == "__main__":
    unittest.main()
