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

    def test_publication_still_requires_every_original_platform_gate(self):
        common = {"preflight", "verify_sanitizers", "build_ubuntu", "portable_packages", "build_snap"}
        for publication, required in (("upload_snap_edge", common),
                                      ("github_prerelease", common | {"build_debian", "upload_snap_edge"})):
            with self.subTest(publication=publication):
                self.assertTrue(required <= ancestors(self.jobs, publication))
        # Success-only dependency semantics must remain in force at every release gate.
        for job in self.jobs.values():
            self.assertNotIn("if", job)
            self.assertFalse(job.get("continue-on-error", False))
        self.assertEqual(self.jobs["upload_snap_edge"]["environment"], "snap-beta")

    def test_snap_and_portable_qualification_can_start_from_the_same_checked_build(self):
        for name in ("build_snap", "portable_packages"):
            self.assertEqual(dependencies(self.jobs[name]), {"preflight", "build_ubuntu"})
        self.assertNotIn("portable_packages", ancestors(self.jobs, "build_snap"))
        self.assertNotIn("build_snap", ancestors(self.jobs, "portable_packages"))
        downloads = [step["with"]["name"] for step in self.jobs["build_snap"]["steps"]
                     if step.get("uses", "").startswith("actions/download-artifact@")]
        self.assertEqual(downloads, ["licasa-ubuntu-deb"])

    def test_output_references_have_direct_dependencies_in_every_workflow(self):
        for workflow, contents in self.workflows.items():
            for name, job in contents["jobs"].items():
                references = set(re.findall(r"\bneeds\.([a-zA-Z_][a-zA-Z_0-9]*)\.", str(job)))
                with self.subTest(workflow=workflow, job=name):
                    self.assertTrue(references <= dependencies(job), references - dependencies(job))

    def test_all_package_verifiers_and_failure_evidence_survive_the_split(self):
        required = {
            "build_ubuntu": {"verify-fresh-build.sh", "check_hardening.py", "verify_deb_container.py"},
            "build_debian": {"build_debian_deb.py", "verify_deb_container.py"},
            "portable_packages": {"verify_appimage_container.py", "verify_appimage_fedora.py",
                                  "verify_appimage_desktops.py", "package_rpm.py", "verify_rpm_container.py",
                                  "package_arch.py", "verify_arch_package.py"},
            "build_snap": {"verify_snap_install.py"},
            "upload_snap_edge": {"verify_snap_install.py"},
            "preflight": {"verify_native_lifecycle.py"},
        }
        for job, tools in required.items():
            scripts = "\n".join(step.get("run", "") for step in self.jobs[job]["steps"])
            for tool in tools:
                with self.subTest(job=job, tool=tool):
                    self.assertIn(tool, scripts)
        failure_upload = next(step for step in self.jobs["build_snap"]["steps"]
                              if step.get("with", {}).get("name") == "failed-snap-verification")
        self.assertIn("failure()", failure_upload["if"])
        self.assertIn("snap-local-smoke*", failure_upload["with"]["path"])

    def test_complete_public_artifact_contract_is_retained(self):
        exports = {step["with"]["name"]: (job, step["with"]["path"])
                   for job, value in self.jobs.items() for step in value.get("steps", [])
                   if step.get("uses", "").startswith("actions/upload-artifact@")}
        self.assertEqual(exports["licasa-linux-amd64"], ("portable_packages", "dist/"))
        self.assertEqual(exports["licasa-debian13-amd64"][0], "build_debian")
        self.assertEqual(exports["licasa-snap-amd64"][0], "build_snap")
        scripts = "\n".join(step.get("run", "") for step in self.jobs["portable_packages"]["steps"])
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
        for command in ("unittest discover -s tests -p 'test_*.py'", "tools/check_code_style.py",
                        "tools/check_support_manifest.py"):
            self.assertEqual(combined.count(command), 1, command)
        for script in ("packaging/install-licasa-appimage.sh", "tools/check_appimage_install.sh"):
            self.assertIn(f"bash -n {script}", combined)


class ReleaseArtifactTransferTests(unittest.TestCase):
    """Run the actual workflow transfer scripts against tiny, real Debian archives."""

    @classmethod
    def setUpClass(cls):
        cls.jobs = yaml.safe_load((ROOT / ".github/workflows/snapcraft-build.yml").read_text())["jobs"]
        if not shutil.which("dpkg-deb"):
            raise RuntimeError("Release artifact tests require dpkg-deb")

    def write_checksum(self, path):
        path.with_name(path.name + ".sha256").write_text(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")

    def create_inputs(self, root, job, package_version):
        deb_dir = root / ("dist/deb" if job == "portable_packages" else "build/verified-linux/deb")
        deb_dir.mkdir(parents=True)
        package = root / "package"
        (package / "DEBIAN").mkdir(parents=True)
        (package / "DEBIAN/control").write_text(
            f"Package: licasa\nVersion: {package_version}\nArchitecture: amd64\n"
            "Maintainer: Fixture <fixture@example.invalid>\nDescription: Artifact transfer fixture\n")
        deb = deb_dir / f"licasa_{VERSION}-1_amd64.deb"
        subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(package), str(deb)],
                       capture_output=True, check=True, timeout=15)
        (deb_dir / "source-commit.txt").write_text(SOURCE_SHA + "\n")
        self.write_checksum(deb)
        appimage_dir = root / "build/qualification/appimage"
        if job == "portable_packages":
            appimage_dir.mkdir(parents=True)
            (appimage_dir / "source-commit.txt").write_text(SOURCE_SHA + "\n")
            for name in (f"Licasa-{VERSION}-x86_64.AppImage", "install-licasa-appimage.sh"):
                path = appimage_dir / name
                path.write_bytes(b"fixture payload\n")
                path.chmod(0o644)  # Artifact download strips executable modes.
                self.write_checksum(path)
        return deb, appimage_dir

    def run_transfer(self, root, job):
        step = next(step for step in self.jobs[job]["steps"] if step.get("id") == "verify_inputs")
        return subprocess.run(["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c", step["run"]],
                              cwd=root, env={**os.environ, "GITHUB_SHA": SOURCE_SHA, "VERSION": VERSION},
                              capture_output=True, text=True, timeout=15)

    def test_valid_transfers_preserve_payload_and_restore_required_executables(self):
        for job in ("build_snap", "portable_packages"):
            with self.subTest(job=job), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                deb, appimage = self.create_inputs(root, job, VERSION + "-1")
                result = self.run_transfer(root, job)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                if job == "build_snap":
                    self.assertEqual((root / "snap-input" / deb.name).read_bytes(), deb.read_bytes())
                    self.assertEqual((root / "snap-input/source-commit.txt").read_text().strip(), SOURCE_SHA)
                else:
                    for name in (f"Licasa-{VERSION}-x86_64.AppImage", "install-licasa-appimage.sh"):
                        self.assertEqual((appimage / name).stat().st_mode & 0o777, 0o755)

    def test_damaged_mismatched_and_missing_inputs_fail_before_packaging(self):
        for job in ("build_snap", "portable_packages"):
            cases = ["deb checksum", "deb source", "deb version", "missing checksum"]
            if job == "portable_packages":
                cases += ["appimage checksum", "appimage source", "installer checksum"]
            for case in cases:
                with self.subTest(job=job, case=case), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    deb, appimage = self.create_inputs(root, job, "0.8.1-1" if case == "deb version"
                                                      else VERSION + "-1")
                    if case == "deb checksum":
                        deb.write_bytes(deb.read_bytes() + b"corrupted")
                    elif case == "deb source":
                        (deb.parent / "source-commit.txt").write_text("b" * 40 + "\n")
                    elif case == "missing checksum":
                        deb.with_name(deb.name + ".sha256").unlink()
                    elif case == "appimage checksum":
                        (appimage / f"Licasa-{VERSION}-x86_64.AppImage").write_bytes(b"corrupted")
                    elif case == "appimage source":
                        (appimage / "source-commit.txt").write_text("b" * 40 + "\n")
                    elif case == "installer checksum":
                        (appimage / "install-licasa-appimage.sh").write_bytes(b"corrupted")
                    result = self.run_transfer(root, job)
                    self.assertNotEqual(result.returncode, 0, case)
                    if job == "build_snap":
                        self.assertFalse((root / "snap-input").exists())


if __name__ == "__main__":
    unittest.main()
