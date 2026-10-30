"""Guard isolation of timing-sensitive probes in the generated CTest graph."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


def require_isolated_tests(manifest, names):
    tests = {test["name"]: test for test in manifest["tests"]}
    for name in names:
        if name not in tests:
            raise ValueError(f"Required timing-sensitive test is missing: {name}")
        properties = {item["name"]: item["value"] for item in tests[name].get("properties", [])}
        if properties.get("RUN_SERIAL") is not True:
            raise ValueError(f"Timing-sensitive test can overlap competing CTest work: {name}")


class CTestSchedulingTests(unittest.TestCase):
    def manifest(self, properties):
        return {"tests": [{"name": "raw-heartbeat", "properties": properties},
                          {"name": "ordinary-test", "properties": []}]}

    def test_serial_probe_does_not_require_serializing_ordinary_tests(self):
        require_isolated_tests(self.manifest([{"name": "RUN_SERIAL", "value": True}]),
                               ["raw-heartbeat"])

    def test_absent_false_and_misspelled_properties_cannot_establish_isolation(self):
        for properties in ([], [{"name": "RUN_SERIAL", "value": False}],
                           [{"name": "RUN_SERIAL", "value": "TRUE"}],
                           [{"name": "RUN_SERIALLY", "value": True}]):
            with self.subTest(properties=properties), self.assertRaisesRegex(ValueError, "overlap"):
                require_isolated_tests(self.manifest(properties), ["raw-heartbeat"])

    def test_missing_required_test_fails_instead_of_vacuously_passing(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            require_isolated_tests({"tests": []}, ["raw-heartbeat"])

    def test_graph_inspection_preserves_the_active_ctest_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CTestTestfile.cmake").write_text("# generated graph\n")
            log = build / "Testing/Temporary/LastTest.log"
            log.parent.mkdir(parents=True)
            log.write_text("active outer CTest evidence\n")

            def inspect_copy(command, **kwargs):
                inspected = Path(command[2])
                self.assertNotEqual(inspected, build)
                self.assertEqual((inspected / "CTestTestfile.cmake").read_text(), "# generated graph\n")
                copied_log = inspected / "Testing/Temporary/LastTest.log"
                copied_log.parent.mkdir(parents=True)
                copied_log.write_text("nested CTest inspection\n")
                return SimpleNamespace(stdout=json.dumps(self.manifest([
                    {"name": "RUN_SERIAL", "value": True}])))

            with patch.object(sys, "argv", ["check", "--build-dir", str(build),
                    "--ctest", "/ctest", "--isolated-test", "raw-heartbeat"]), \
                    patch.object(subprocess, "run", side_effect=inspect_copy):
                check_build()
            self.assertEqual(log.read_text(), "active outer CTest evidence\n")


def check_build():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--isolated-test", action="append", required=True)
    args = parser.parse_args()
    # Even --show-only writes Testing/Temporary/LastTest.log. Inspect a private
    # copy of the generated graph so an in-flight outer CTest keeps its evidence.
    with tempfile.TemporaryDirectory(prefix="licasa-ctest-graph-") as temporary:
        graph = Path(temporary)
        for source in args.build_dir.rglob("CTestTestfile.cmake"):
            destination = graph / source.relative_to(args.build_dir)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        result = subprocess.run([args.ctest, "--test-dir", str(graph),
                                 "--show-only=json-v1"], capture_output=True, text=True,
                                check=True, timeout=15)
    require_isolated_tests(json.loads(result.stdout), args.isolated_test)
    print("Generated CTest graph isolates: " + ", ".join(args.isolated_test))


if __name__ == "__main__":
    if "--build-dir" in sys.argv:
        check_build()
    else:
        unittest.main()
