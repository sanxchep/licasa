#!/usr/bin/env python3
"""Guard the shared source gate used by CI and release sanitizer checks."""

import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ci_plan import check_source_results, source_plan, source_presets


class SourceWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load((ROOT / ".github/workflows/verify-source.yml").read_text())
        self.jobs = self.workflow["jobs"]

    def test_request_requires_exact_clean_source_on_hosted_runner(self):
        sha = "a" * 40
        result = source_plan(sha, sha, "", '["asan","ubsan"]', "github-hosted")
        self.assertEqual(result, {"sha": sha, "presets": '["asan", "ubsan"]'})
        for source, actual, dirty, runner in (
                ("main", sha, "", "github-hosted"),
                (sha[:12], sha, "", "github-hosted"),
                (sha, "b" * 40, "", "github-hosted"),
                (sha, sha, " M src/main.cpp", "github-hosted"),
                (sha, sha, "?? extra-file", "github-hosted"),
                (sha, sha, "", "self-hosted"),
                (sha, sha, "", "")):
            with self.subTest(source=source, actual=actual, dirty=dirty, runner=runner):
                with self.assertRaises(ValueError):
                    source_plan(source, actual, dirty, '["asan","ubsan"]', runner)

    def test_only_nonempty_distinct_supported_presets_reach_matrix(self):
        for value in ('', '[]', '{}', 'null', '"asan"', '[null]', '[{}]',
                      '["asan","asan"]', '["unknown"]', '["asan; false"]'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                source_presets(value)
        for selected in (["release"], ["asan", "ubsan"], ["release", "asan", "ubsan"]):
            self.assertEqual(source_presets(json.dumps(selected)), selected)

    def test_completion_rejects_each_missing_failed_cancelled_or_skipped_gate(self):
        good = {name: {"result": "success"} for name in ("plan", "quick", "test")}
        self.assertEqual(set(check_source_results(good)), set(good))
        for name in good:
            for status in ("failure", "cancelled", "skipped", "", None):
                changed = copy.deepcopy(good)
                if status is None:
                    changed.pop(name)
                else:
                    changed[name]["result"] = status
                with self.subTest(name=name, status=status), self.assertRaises(ValueError):
                    check_source_results(changed)

    def test_completion_command_fails_closed_on_serialized_needs(self):
        needs = {name: {"result": "success"} for name in ("plan", "quick", "test")}
        for status, expected in (("success", 0), ("skipped", 1), ("failure", 1), ("cancelled", 1)):
            needs["test"]["result"] = status
            with self.subTest(status=status):
                result = subprocess.run([sys.executable, "-B", str(ROOT / "tools/ci_plan.py"),
                                         "source-check"], text=True, capture_output=True,
                                        env=dict(os.environ, CI_RESULTS=json.dumps(needs)), timeout=10)
                self.assertEqual(result.returncode, expected, result.stderr)

    def test_validation_precedes_expensive_work_and_completion_always_runs(self):
        self.assertEqual(set(self.jobs), {"plan", "quick", "test", "result"})
        self.assertEqual(self.jobs["quick"]["needs"], "plan")
        self.assertEqual(set(self.jobs["test"]["needs"]), {"plan", "quick"})
        self.assertEqual(set(self.jobs["result"]["needs"]), {"plan", "quick", "test"})
        self.assertEqual(self.jobs["result"]["if"], "${{ always() }}")
        for job in self.jobs.values():
            checkout = job["steps"][0]
            self.assertEqual(checkout["uses"], "actions/checkout@v7")
            self.assertFalse(checkout["with"]["persist-credentials"])
        plan = self.jobs["plan"]["steps"][-1]
        self.assertIn("tools/ci_plan.py source-plan", plan["run"])
        self.assertEqual(plan["env"]["SOURCE_SHA"], "${{ inputs.source_ref }}")
        self.assertEqual(plan["env"]["PRESETS"], "${{ inputs.presets }}")

    def test_both_callers_share_source_steps_and_the_release_keeps_both_sanitizers(self):
        for name, job_id in (("ci.yml", "test"), ("snapcraft-build.yml", "verify_sanitizers")):
            workflow = yaml.safe_load((ROOT / ".github/workflows" / name).read_text())
            call = workflow["jobs"][job_id]
            with self.subTest(workflow=name):
                self.assertEqual(call["uses"], "./.github/workflows/verify-source.yml")
                self.assertNotIn("steps", call)
            if name == "snapcraft-build.yml":
                self.assertEqual(json.loads(call["with"]["presets"]), ["asan", "ubsan"])
        matrix = self.jobs["test"]["strategy"]
        self.assertFalse(matrix["fail-fast"])
        self.assertEqual(matrix["matrix"]["preset"], "${{ fromJSON(needs.plan.outputs.presets) }}")

    def test_shared_runtime_transfer_preserves_executable_and_symlink(self):
        quick_steps = self.jobs["quick"]["steps"]
        archive = next(step["run"] for step in quick_steps
                       if step.get("name") == "Preserve runtime permissions, links and provenance")
        restore = next(step["run"] for step in self.jobs["test"]["steps"]
                       if step.get("name") == "Restore the same verified Qt runtime")
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            runtime = work / "build/deps/qt-backport-runtime"
            runtime.mkdir(parents=True)
            payload = runtime / "libQt6Quick.so.6.4.2"
            payload.write_bytes(b"exact runtime fixture")
            payload.chmod(0o755)
            (runtime / "libQt6Quick.so.6").symlink_to(payload.name)
            subprocess.run(["bash", "-euc", archive], cwd=work, check=True, timeout=10)
            shared = work / "build/shared-runtime"
            shared.mkdir()
            (work / "build/quick-runtime.tar.gz").rename(shared / "quick-runtime.tar.gz")
            payload.unlink()
            (runtime / "libQt6Quick.so.6").unlink()
            subprocess.run(["bash", "-euc", restore], cwd=work, check=True, timeout=10)
            self.assertEqual(payload.read_bytes(), b"exact runtime fixture")
            self.assertEqual(payload.stat().st_mode & 0o777, 0o755)
            self.assertEqual(os.readlink(runtime / "libQt6Quick.so.6"), payload.name)

    def test_exact_dependency_caches_and_toolchain_selection_remain_enforced(self):
        steps = self.jobs["test"]["steps"]
        private = next(step["with"] for step in steps
                       if step.get("name") == "Restore exact private dependencies")
        self.assertIn("${{ matrix.preset }}", private["key"])
        self.assertIn("${{ steps.toolchain.outputs.digest }}", private["key"])
        self.assertIn("packaging/*.lock.json", private["key"])
        self.assertNotIn("restore-keys", private)
        fingerprint = next(step for step in steps
                           if step.get("name") == "Fingerprint the installed dependency toolchain")
        self.assertEqual(fingerprint["env"]["EXPECTED_TOOLCHAIN"], "${{ needs.quick.outputs.toolchain }}")
        shared_guard = "steps.toolchain.outputs.shared_runtime == 'true'"
        for step in steps:
            if (step.get("uses") == "actions/download-artifact@v8"
                    or step.get("name") == "Restore the same verified Qt runtime"):
                self.assertEqual(step["if"], shared_guard)
                self.assertLess(steps.index(fingerprint), steps.index(step))
        local = next(step for step in steps if step.get("id") == "quick_dependencies")
        producer = next(step for step in self.jobs["quick"]["steps"]
                        if step.get("id") == "dependencies")
        self.assertEqual(local["if"], "steps.toolchain.outputs.shared_runtime == 'false'")
        self.assertEqual(local["with"], producer["with"])
        self.assertNotIn("restore-keys", local["with"])
        builder = next(step for step in steps
                       if step.get("name") == "Build Qt for the current runner toolchain")
        self.assertEqual(builder["if"], "steps.toolchain.outputs.shared_runtime == 'false' "
                         "&& steps.quick_dependencies.outputs.cache-hit != 'true'")
        producer_builder = next(step for step in self.jobs["quick"]["steps"]
                                if step.get("name") == "Build the identical Release Qt runtime once")
        self.assertEqual(builder["run"], producer_builder["run"])

    def test_full_ctest_release_installation_and_failure_logs_remain(self):
        steps = self.jobs["test"]["steps"]
        commands = "\n".join(step.get("run", "") for step in steps)
        for command in ("cmake --preset", "cmake --build --preset", "ctest --preset",
                        "tools/check_hardening.py", "tools/verify_installed_release.py",
                        "licasa_diagnostics"):
            with self.subTest(command=command):
                self.assertIn(command, commands)
        installed = next(step for step in steps if step.get("name") == "Verify installed release")
        self.assertEqual(installed["if"], "matrix.preset == 'release'")
        logs = next(step for step in steps if step.get("uses") == "actions/upload-artifact@v7")
        self.assertEqual(logs["if"], "always()")
        for report in ("LastTest.log", "runtime-formats.json", "package-smoke.json",
                       "raw-viewer-behavior.json", "viewer-lifecycle.json"):
            with self.subTest(report=report):
                self.assertIn(report, logs["with"]["path"])


class ToolchainRuntimeWorkflowTests(unittest.TestCase):
    """Execute the workflow's shell against runner inventory/cache fixtures."""

    PACKAGES = ("git=1:2.56.0-0ppa1~ubuntu24.04.2\n"
                "g++=4:13.2.0-7ubuntu1\nqt6-base-dev=6.4.2+dfsg-21.1build5\n")

    def setUp(self):
        self.jobs = yaml.safe_load((ROOT / ".github/workflows/verify-source.yml").read_text())["jobs"]

    def step(self, name, job="test"):
        return next(step for step in self.jobs[job]["steps"] if step.get("name") == name)

    @staticmethod
    def digest(packages):
        manifest = "".join(line + "\n" for line in sorted(packages.splitlines()))
        return hashlib.sha256(manifest.encode()).hexdigest()

    @staticmethod
    def shell(step, work, **env):
        # Match GitHub's default bash -e shell; fingerprint blocks must enable
        # pipefail themselves so a failed query cannot hide behind sort.
        return subprocess.run(["bash", "--noprofile", "--norc", "-e", "-c", step["run"]],
                              cwd=work, env={**os.environ, **env}, text=True,
                              capture_output=True, timeout=10)

    def fingerprint(self, work, packages, expected=None, status=0, job="test"):
        binaries = work / "bin"
        binaries.mkdir(exist_ok=True)
        fixture = work / "inventory.txt"
        fixture.write_text(packages)
        query = binaries / "dpkg-query"
        query.write_text('#!/bin/sh\ncat "$PACKAGE_FIXTURE"\nexit "$QUERY_STATUS"\n')
        query.chmod(0o755)
        output = work / "github-output"
        result = self.shell(self.step("Fingerprint the installed dependency toolchain", job), work,
                            PATH=str(binaries) + os.pathsep + os.environ["PATH"],
                            PACKAGE_FIXTURE=str(fixture), QUERY_STATUS=str(status),
                            EXPECTED_TOOLCHAIN=self.digest(self.PACKAGES) if expected is None else expected,
                            GITHUB_OUTPUT=str(output))
        outputs = dict(line.split("=", 1) for line in output.read_text().splitlines()) if output.exists() else {}
        return result, outputs

    def test_same_inventory_uses_shared_runtime_regardless_of_query_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            packages = "\n".join(reversed(self.PACKAGES.splitlines())) + "\n"
            result, outputs = self.fingerprint(work, packages)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(outputs, {"digest": self.digest(self.PACKAGES), "shared_runtime": "true"})
            manifest = (work / "build/toolchain/packages.txt").read_text()
            self.assertEqual(manifest.splitlines(), sorted(self.PACKAGES.splitlines()))
            self.assertEqual((work / "build/toolchain/runtime-source.txt").read_text(), "shared\n")

    def test_git_or_dependency_version_drift_selects_only_current_toolchain(self):
        # The Git delta reproduces the completed release's unrelated runner-image drift.
        for old, new in (("git=1:2.56.0", "git=1:2.55.0"), ("g++=4:13.2.0", "g++=4:13.3.0")):
            with self.subTest(package=old), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                packages = self.PACKAGES.replace(old, new)
                result, outputs = self.fingerprint(work, packages)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(outputs, {"digest": self.digest(packages), "shared_runtime": "false"})
                self.assertNotEqual(outputs["digest"], self.digest(self.PACKAGES))
                self.assertIn(self.digest(self.PACKAGES), result.stdout)
                self.assertIn(outputs["digest"], result.stdout)
                self.assertEqual((work / "build/toolchain/runtime-source.txt").read_text(), "local\n")
                self.assertEqual((work / "build/toolchain/producer-digest.txt").read_text().strip(),
                                 self.digest(self.PACKAGES))

    def test_producer_records_the_same_complete_sorted_inventory(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            result, outputs = self.fingerprint(work, self.PACKAGES, job="quick")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(outputs, {"digest": self.digest(self.PACKAGES)})
            self.assertEqual((work / "build/toolchain/packages.txt").read_text().splitlines(),
                             sorted(self.PACKAGES.splitlines()))

    def test_failed_empty_or_invalid_fingerprints_fail_before_runtime_selection(self):
        for packages, status, expected in ((self.PACKAGES, 7, None), ("", 0, None), (self.PACKAGES, 0, ""),
                                           (self.PACKAGES, 0, "invalid-producer-digest")):
            for job in ("test", "quick") if expected is None else ("test",):
                with self.subTest(job=job, status=status, packages=packages, expected=expected):
                    with tempfile.TemporaryDirectory() as temporary:
                        result, outputs = self.fingerprint(Path(temporary), packages, expected, status, job)
                        self.assertNotEqual(result.returncode, 0)
                        self.assertEqual(outputs, {})

    def condition_applies(self, step, outputs, cache_hit):
        """Resolve only the explicit output comparisons used by these runtime steps."""
        values = {"toolchain.shared_runtime": outputs["shared_runtime"],
                  "quick_dependencies.cache-hit": cache_hit}
        for term in step.get("if", "").split(" && "):
            if not term:
                continue
            match = re.fullmatch(r"steps\.([a-z_]+)\.outputs\.([a-z_-]+) (==|!=) '(true|false)'", term)
            self.assertIsNotNone(match, term)
            job, key, operator, expected = match.groups()
            equal = values[f"{job}.{key}"] == expected
            if equal != (operator == "=="):
                return False
        return True

    @staticmethod
    def make_runtime(work, payload=b"current toolchain runtime"):
        runtime = work / "build/deps/qt-backport-runtime"
        runtime.mkdir(parents=True, exist_ok=True)
        (runtime / "libQt6Quick.so.6.4.2").write_bytes(payload)

    def runtime_selection(self, work, outputs, cache_hit="false", build_mode="good"):
        # External cache/download actions are fixtures; every selected run block is real workflow shell.
        builder = work / "bin/python3"
        builder.write_text(f"#!{sys.executable}\n" + '''import json
import os
from pathlib import Path
import sys
Path('builder-arguments.json').write_text(json.dumps(sys.argv[1:]))
mode = os.environ['BUILD_MODE']
if mode == 'failure':
    raise SystemExit(37)
runtime = Path('build/deps/qt-backport-runtime')
runtime.mkdir(parents=True, exist_ok=True)
library = runtime / 'libQt6Quick.so.6.4.2'
if mode == 'good':
    library.write_bytes(b'locally built Qt runtime')
elif mode == 'empty':
    library.write_bytes(b'')
elif mode == 'broken-link':
    library.symlink_to('missing-library')
''')
        builder.chmod(0o755)
        selected = []
        active = False
        for step in self.jobs["test"]["steps"]:
            if step.get("id") == "toolchain":
                active = True
                continue
            if not active:
                continue
            if step.get("name") == "Restore exact private dependencies":
                break
            if not self.condition_applies(step, outputs, cache_hit):
                continue
            selected.append(step.get("name", step.get("uses")))
            if "run" in step:
                result = self.shell(step, work, BUILD_MODE=build_mode,
                                    PATH=str(work / "bin") + os.pathsep + os.environ["PATH"])
                if result.returncode:
                    return result, selected
        (work / "configuration-reached").touch()
        return result, selected

    def test_matching_inventory_restores_shared_runtime_and_skips_local_cache_and_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            _, outputs = self.fingerprint(work, self.PACKAGES)
            self.make_runtime(work, b"shared producer runtime")
            runtime = work / "build/deps/qt-backport-runtime"
            archive = self.step("Preserve runtime permissions, links and provenance", "quick")
            self.assertEqual(self.shell(archive, work).returncode, 0)
            shared = work / "build/shared-runtime"
            shared.mkdir()
            (work / "build/quick-runtime.tar.gz").rename(shared / "quick-runtime.tar.gz")
            (runtime / "libQt6Quick.so.6.4.2").unlink()
            result, selected = self.runtime_selection(work, outputs)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("actions/download-artifact@v8", selected)
            self.assertIn("Restore the same verified Qt runtime", selected)
            self.assertNotIn("Restore exact runner-local Qt runtime", selected)
            self.assertFalse((work / "builder-arguments.json").exists())
            self.assertEqual((runtime / "libQt6Quick.so.6.4.2").read_bytes(), b"shared producer runtime")

    def test_mismatched_inventory_builds_locally_without_downloading_or_extracting_shared_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            result, outputs = self.fingerprint(work, self.PACKAGES.replace("2.56.0", "2.55.0"))
            self.assertEqual(result.returncode, 0, result.stderr)
            shared = work / "build/shared-runtime"
            shared.mkdir()
            # Any attempt to extract this artifact would fail; this runner must never select it.
            (shared / "quick-runtime.tar.gz").write_bytes(b"incompatible shared runtime")
            result, selected = self.runtime_selection(work, outputs)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("actions/download-artifact@v8", selected)
            self.assertNotIn("Restore the same verified Qt runtime", selected)
            self.assertIn("Restore exact runner-local Qt runtime", selected)
            self.assertTrue((work / "configuration-reached").exists())
            self.assertEqual(json.loads((work / "builder-arguments.json").read_text())[:3],
                             ["tools/build_ci_dependencies.py", "quick", "--jobs"])
            self.assertEqual((work / "build/deps/qt-backport-runtime/libQt6Quick.so.6.4.2").read_bytes(),
                             b"locally built Qt runtime")

    def test_exact_current_toolchain_cache_hit_skips_build_and_shared_transfer(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            _, outputs = self.fingerprint(work, self.PACKAGES.replace("2.56.0", "2.55.0"))
            self.make_runtime(work)
            result, selected = self.runtime_selection(work, outputs, cache_hit="true")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("Build Qt for the current runner toolchain", selected)
            self.assertNotIn("actions/download-artifact@v8", selected)
            self.assertFalse((work / "builder-arguments.json").exists())

    def test_failed_or_missing_local_runtime_stops_before_configuration(self):
        for mode in ("failure", "missing", "empty", "broken-link"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                _, outputs = self.fingerprint(work, self.PACKAGES.replace("2.56.0", "2.55.0"))
                result, _ = self.runtime_selection(work, outputs, build_mode=mode)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((work / "configuration-reached").exists())

    def test_missing_runtime_on_exact_cache_hit_fails_before_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            _, outputs = self.fingerprint(work, self.PACKAGES.replace("2.56.0", "2.55.0"))
            result, _ = self.runtime_selection(work, outputs, cache_hit="true")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((work / "configuration-reached").exists())
            self.assertFalse((work / "builder-arguments.json").exists())

    def test_matching_inventory_does_not_silently_fallback_after_bad_shared_transfer(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            _, outputs = self.fingerprint(work, self.PACKAGES)
            result, selected = self.runtime_selection(work, outputs)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("actions/download-artifact@v8", selected)
            self.assertNotIn("Restore exact runner-local Qt runtime", selected)
            self.assertFalse((work / "configuration-reached").exists())

    def test_toolchain_manifests_survive_producer_and_matrix_failure(self):
        producer = next(step for step in self.jobs["quick"]["steps"]
                        if step.get("with", {}).get("name") == "verification-toolchain-quick")
        self.assertEqual(producer["if"], "always()")
        self.assertEqual(producer["with"]["path"], "build/toolchain/")
        matrix = next(step for step in self.jobs["test"]["steps"]
                      if step.get("uses") == "actions/upload-artifact@v7")
        self.assertEqual(matrix["if"], "always()")
        self.assertIn("build/toolchain/", matrix["with"]["path"])


if __name__ == "__main__":
    unittest.main()
