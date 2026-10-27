#!/usr/bin/env python3
"""Keep nested local workflow permissions within the calling job's grant."""

import copy
from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[1]
LOCAL = "./.github/workflows/"
LEVELS = {"none": 0, "read": 1, "write": 2}


def permissions(value, inherited):
    if value is None:
        return dict(inherited)
    if value in ("read-all", "write-all"):
        return {"*": value.removesuffix("-all")}
    if not isinstance(value, dict) or any(level not in LEVELS for level in value.values()):
        raise ValueError(f"Unexpected workflow permission declaration: {value!r}")
    # An explicit mapping sets every unlisted permission to none.
    return dict(value)


def permission_level(grant, scope):
    return grant.get(scope, grant.get("*", "none"))


def check_local_calls(workflows, entry):
    """Inspect calls statically, including jobs whose runtime if is false."""
    calls = set()

    def visit(name, ceiling=None, trail=()):
        if name in trail:
            raise ValueError(f"Recursive local workflow call: {name}")
        workflow = workflows[name]
        for job_id, job in workflow.get("jobs", {}).items():
            declaration = job.get("permissions", workflow.get("permissions"))
            grant = permissions(declaration, ceiling or {})
            if ceiling is not None:
                scopes = grant.keys() | ceiling.keys()
                for scope in scopes:
                    requested, available = permission_level(grant, scope), permission_level(ceiling, scope)
                    if LEVELS[requested] > LEVELS[available]:
                        path = " -> ".join((*trail, f"{name}/{job_id}"))
                        raise ValueError(f"{path}: {scope}:{requested} exceeds caller {scope}:{available}")
            target = job.get("uses", "")
            if target.startswith(LOCAL):
                target = target.removeprefix(LOCAL)
                calls.add((name, job_id, target))
                visit(target, grant, (*trail, name))

    visit(entry)
    return calls


class WorkflowPermissionTests(unittest.TestCase):
    def setUp(self):
        self.workflows = {path.name: yaml.safe_load(path.read_text())
                          for path in (ROOT / ".github/workflows").glob("*.yml")}

    def test_release_uses_the_shared_full_graph_without_publication_permissions(self):
        calls = check_local_calls(self.workflows, "snapcraft-build.yml")
        self.assertIn(("snapcraft-build.yml", "verify_sanitizers", "ci.yml"), calls)
        self.assertIn(("ci.yml", "test", "verify-source.yml"), calls)
        self.assertIn(("ci.yml", "ubuntu", "build-native.yml"), calls)
        self.assertIn(("ci.yml", "debian", "build-native.yml"), calls)

    def broad_ci_caller(self):
        workflows = copy.deepcopy(self.workflows)
        caller = workflows["snapcraft-build.yml"]["jobs"]["verify_sanitizers"]
        caller["uses"] = LOCAL + "ci.yml"
        caller["permissions"] = {"actions": "read", "contents": "read"}
        return workflows

    def test_original_release_caller_without_actions_read_reproduces_failure(self):
        workflows = self.broad_ci_caller()
        workflows["snapcraft-build.yml"]["jobs"]["verify_sanitizers"].pop("permissions", None)
        workflows["ci.yml"]["jobs"]["retry"]["if"] = False
        with self.assertRaisesRegex(ValueError, r"ci.yml/retry: actions:read exceeds caller actions:none"):
            check_local_calls(workflows, "snapcraft-build.yml")

    def test_nested_retry_cannot_escalate_to_actions_write(self):
        workflows = self.broad_ci_caller()
        workflows["retry-verification.yml"]["permissions"]["actions"] = "write"
        with self.assertRaisesRegex(ValueError, r"actions:write exceeds caller actions:read"):
            check_local_calls(workflows, "snapcraft-build.yml")

    def test_release_source_verification_requires_only_contents_read(self):
        release = self.workflows["snapcraft-build.yml"]
        self.assertEqual(release["permissions"], {"contents": "read"})
        self.assertEqual(release["jobs"]["verify_sanitizers"].get("permissions", release["permissions"]),
                         {"contents": "read", "actions": "read", "pull-requests": "read"})
        self.assertEqual(self.workflows["verify-source.yml"]["permissions"], {"contents": "read"})
        check_local_calls(self.workflows, "ci.yml")

    def test_undeclared_child_permissions_inherit_the_calling_job(self):
        workflows = {
            "parent.yml": {"permissions": {"contents": "read", "actions": "read"},
                           "jobs": {"child": {"uses": LOCAL + "child.yml"}}},
            "child.yml": {"jobs": {"worker": {}}},
        }
        check_local_calls(workflows, "parent.yml")
        workflows["child.yml"]["jobs"]["worker"]["permissions"] = {"contents": "write"}
        with self.assertRaisesRegex(ValueError, r"contents:write exceeds caller contents:read"):
            check_local_calls(workflows, "parent.yml")

    def test_explicit_job_mapping_does_not_inherit_unlisted_permissions(self):
        workflows = {
            "parent.yml": {"permissions": {"contents": "read", "actions": "read"}, "jobs": {
                "child": {"permissions": {"contents": "read"}, "uses": LOCAL + "child.yml"}}},
            "child.yml": {"permissions": {"actions": "read"}, "jobs": {"worker": {}}},
        }
        with self.assertRaisesRegex(ValueError, r"actions:read exceeds caller actions:none"):
            check_local_calls(workflows, "parent.yml")

    def test_shorthand_permissions_keep_the_same_ceiling(self):
        self.assertEqual(permissions("read-all", {}), {"*": "read"})
        self.assertEqual(permission_level({"*": "read"}, "actions"), "read")
        self.assertEqual(permissions({}, {"actions": "read"}), {})


if __name__ == "__main__":
    unittest.main()
