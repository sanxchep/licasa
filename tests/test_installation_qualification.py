#!/usr/bin/env python3
"""Guard the minimal-runtime qualification boundary before desktop tools are added."""

import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def generated_recipe(name):
    tree = ast.parse((ROOT / "tools" / name).read_text())
    names = {"args": SimpleNamespace(ubuntu_version="24.04", target="ubuntu-24.04")}
    for node in tree.body:
        if (isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name)
                        and target.id in {"HOST_RUNTIME_PACKAGES", "INSTALLER_PACKAGES"}
                        for target in node.targets)):
            names[node.targets[0].id] = ast.literal_eval(node.value)
    calls = [node for node in ast.walk(tree)
             if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
             and node.func.attr == "write_text" and isinstance(node.func.value, ast.BinOp)
             and isinstance(node.func.value.right, ast.Constant)
             and node.func.value.right.value == "Dockerfile"]
    if len(calls) != 1:
        raise AssertionError(f"Expected one Dockerfile recipe in {name}")
    return eval(compile(ast.Expression(calls[0].args[0]), name, "eval"),
                {"__builtins__": {}}, names)


class DependencyRecipeTests(unittest.TestCase):
    def test_rpm_lifecycle_guard_runs_after_its_python_dependency_before_payload_install(self):
        sys.path.insert(0, str(ROOT / "tools"))
        try:
            import package_rpm
            spec = package_rpm.render_spec()
        finally:
            sys.path.remove(str(ROOT / "tools"))
        self.assertIsNone(re.search(r"(?m)^Requires\(pretrans\):", spec))
        self.assertIsNone(re.search(r"(?m)^%pretrans(?:\s|$)", spec))
        self.assertRegex(spec, r"(?m)^Requires\(pre\):\s+/usr/bin/python3$")
        preinstall = re.search(r"(?ms)^%pre\n(.*?)(?=^%post\n)", spec)
        self.assertIsNotNone(preinstall)
        self.assertIn("--installation appimage-native prepare --action upgrade", preinstall[1])
        self.assertNotIn("@NATIVE_", spec)

    def installer_recipes(self):
        return {
            "fedora": generated_recipe("verify_appimage_fedora.py"),
            "arch": re.sub(r"\\\n\s*", " ",
                           (ROOT / "packaging/verify-appimage-arch.Dockerfile").read_text()),
        }

    def assert_installer_prerequisites(self, distribution, recipe, commands):
        # These are distro package providers, not a second list of required tools.
        # Read required commands from the real installer so a new prerequisite
        # must be covered here before the release's expensive build can begin.
        providers = dict.fromkeys(
            "uname cp chmod mkdir mktemp mv rm rmdir sha256sum dirname basename cat".split(),
            "coreutils")
        providers.update(find="findutils", sed="sed", bash="bash",
                         getconf="glibc-common" if distribution == "fedora" else "glibc",
                         ldd="glibc-common" if distribution == "fedora" else "glibc",
                         flock="util-linux-core" if distribution == "fedora" else "util-linux",
                         python3="python3" if distribution == "fedora" else "python")
        lines = recipe.splitlines()
        lifecycle = next(i for i, line in enumerate(lines)
                         if line.startswith("RUN ") and "check_appimage_install.sh" in line)
        linkage = next(i for i, line in enumerate(lines)
                       if line.startswith("RUN ") and "licasa_check_runtime_dependencies" in line)
        packages = set()
        installed_at = {}
        for index, line in enumerate(lines[:lifecycle]):
            if not line.startswith("RUN "):
                continue
            for command in line[4:].split("&&"):
                tokens = shlex.split(command)
                if (tokens[:2] == ["dnf", "install"]
                        or (len(tokens) > 1 and tokens[0] == "pacman"
                            and tokens[1] in {"-S", "-Sy", "-Syu"})):
                    for token in tokens[2:]:
                        if not token.startswith("-"):
                            packages.add(token)
                            installed_at.setdefault(token, index)
        for command in ["bash", *commands]:
            self.assertIn(command, providers, f"Map the new installer tool {command!r} to its distro package")
            self.assertIn(providers[command], packages,
                          f"{distribution}: install {providers[command]} for {command} before the lifecycle gate")
        self.assertLess(linkage, installed_at[providers["python3"]],
                        "Keep Python's dependencies out of the raw bundle's loader check")

    def installer_commands(self):
        installer = (ROOT / "packaging/install-licasa-appimage.sh").read_text()
        assignments = re.findall(r"^\s*commands=\(.*\)$", installer, re.MULTILINE)
        self.assertEqual(len(assignments), 2, "Update the contract reader if installer tool declarations change")
        result = subprocess.run(["bash", "-ec", "\n".join(assignments) + '\nprintf "%s\\n" "${commands[@]}"'],
                                capture_output=True, text=True, check=True, timeout=5)
        return result.stdout.splitlines()

    def test_managed_install_tools_are_declared_before_minimal_lifecycle_gate(self):
        for distribution, recipe in self.installer_recipes().items():
            with self.subTest(distribution=distribution):
                self.assert_installer_prerequisites(distribution, recipe, self.installer_commands())

    def test_installer_prerequisite_guard_rejects_missing_late_and_new_requirements(self):
        commands = self.installer_commands()
        for distribution, recipe in self.installer_recipes().items():
            python = "python3" if distribution == "fedora" else "python"
            without_python = re.sub(r"\b" + python + r"\b", "", recipe)
            for scenario, candidate, required in (
                    ("missing", without_python, commands),
                    ("too late", without_python + f"\nRUN {'dnf install -y' if distribution == 'fedora' else 'pacman -S'} {python}\n", commands),
                    ("new tool", recipe, commands + ["unmapped-new-installer-tool"])):
                with self.subTest(distribution=distribution, scenario=scenario):
                    with self.assertRaises(AssertionError):
                        self.assert_installer_prerequisites(distribution, candidate, required)

    def test_release_hardening_inspects_the_packaged_elf_behind_its_launcher(self):
        workflow = (ROOT / ".github/workflows/build-native.yml").read_text()
        targets = re.findall(r"python3 /source/tools/check_hardening.py ([^\n]+)", workflow)
        self.assertEqual(len(targets), 1)
        target = shlex.split(targets[0].removesuffix("'"))[0]
        self.assertTrue(target.startswith("/tmp/licasa-package/"))
        with tempfile.TemporaryDirectory(prefix="licasa release hardening ") as temporary:
            root = Path(temporary)
            binary = root / "usr/bin/licasa.bin"
            binary.parent.mkdir(parents=True)
            compiler = shutil.which("cc")
            self.assertIsNotNone(compiler, "The package-contract toolchain requires a C compiler")
            subprocess.run([compiler, "-x", "c", "-", "-o", str(binary), "-fPIE", "-pie",
                            "-Wl,-z,relro", "-Wl,-z,now"], input="int main(void) { return 0; }\n",
                           text=True, capture_output=True, check=True, timeout=20)
            launcher = binary.with_name("licasa")
            launcher.write_text('#!/bin/sh\nexec "$(dirname "$0")/licasa.bin" "$@"\n')
            selected = root / target.removeprefix("/tmp/licasa-package/")
            result = subprocess.run([sys.executable, str(ROOT / "tools/check_hardening.py"), str(selected)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertTrue(all(report["checks"].values()))
            self.assertEqual(report["sha256"], hashlib.sha256(binary.read_bytes()).hexdigest())

    def test_wrapper_packages_cover_normal_launchers_and_recover_from_the_copied_apprun(self):
        for name in ("verify_rpm_container.py", "verify_arch_package.py"):
            with self.subTest(tool=name), tempfile.TemporaryDirectory() as temporary:
                sys.path.insert(0, str(ROOT / "tools"))
                try:
                    spec = importlib.util.spec_from_file_location("wrapper_qualification", ROOT / "tools" / name)
                    module = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(module)
                finally:
                    sys.path.remove(str(ROOT / "tools"))
                package = Path(temporary) / "candidate-package"
                package.write_bytes(b"fixture, no external package manager is executed")
                output = Path(temporary) / "report.json"
                arguments = [name, str(package), "--output", str(output)]
                result = SimpleNamespace(returncode=0, stdout="display evidence", stderr="")
                with patch.object(sys, "argv", arguments), \
                        patch.object(module.subprocess, "run", return_value=result) as run, \
                        patch.object(module, "parse_display_graphics", return_value={"status": "passed"}), \
                        patch.object(module, "run_reader_checks", return_value={"status": "passed"}), \
                        patch.object(module, "run_motion_checks", return_value={"status": "passed"}), \
                        patch.object(module, "run_dialog_checks", return_value=[]) as dialogs:
                    module.main()
                sessions = [call.args[0] for call in run.call_args_list if call.args[0][:2] == ["docker", "run"]]
                self.assertEqual(len(sessions), 4)
                for command in sessions:
                    self.assertIn("sh /opt/check_display_session.sh /usr/bin/licasa ", command[-1])
                keywords = dialogs.call_args.kwargs
                self.assertEqual(keywords["app"], "/opt/licasa/AppRun")
                copied_root = Path(keywords["qml_root"]).parents[1]
                self.assertEqual(Path(keywords["app"]), copied_root / "AppRun")
                self.assertEqual(keywords["diagnostic"], str(copied_root / "usr/bin/licasa_diagnostics"))

    def test_library_checks_precede_desktop_tool_installation(self):
        for name in ("verify_deb_container.py", "verify_appimage_container.py",
                     "verify_appimage_fedora.py", "verify_rpm_container.py", "verify_arch_package.py"):
            with self.subTest(tool=name):
                recipe = generated_recipe(name)
                lines = recipe.splitlines()
                check = next(index for index, line in enumerate(lines)
                             if line.startswith("RUN ") and "licasa_check_runtime_dependencies" in line)
                tools = next(index for index, line in enumerate(lines)
                             if line.startswith("RUN ") and "xvfb" in line.lower())
                self.assertLess(check, tools)
                self.assertFalse(any("weston" in line.lower() or "xvfb" in line.lower() or "desktop-file-utils" in line
                                     for line in lines[:check]))
                for line in lines:
                    if line.startswith("RUN "):
                        subprocess.run(["bash", "-n"], input=line[4:], text=True, check=True)

    def test_arch_appimage_dependency_and_lifecycle_gates_precede_display_tools(self):
        recipe = re.sub(r"\\\n\s*", " ", (ROOT / "packaging/verify-appimage-arch.Dockerfile").read_text())
        lines = recipe.splitlines()
        linkage = next(i for i, line in enumerate(lines) if line.startswith("RUN ") and "licasa_check_runtime_dependencies" in line)
        lifecycle = next(i for i, line in enumerate(lines) if line.startswith("RUN ") and "check_appimage_install.sh" in line)
        display = next(i for i, line in enumerate(lines) if line.startswith("RUN ") and "xorg-server-xvfb" in line)
        self.assertLess(linkage, lifecycle)
        self.assertLess(lifecycle, display)
        for line in lines:
            if line.startswith("RUN "):
                subprocess.run(["bash", "-n"], input=line[4:], text=True, check=True)


class LifecycleExecutionGuardTests(unittest.TestCase):
    def test_lifecycle_check_refuses_an_ordinary_home(self):
        with tempfile.TemporaryDirectory(prefix="licasa-lifecycle-guard-") as temporary:
            result = subprocess.run(
                ["bash", str(ROOT / "tools/check_appimage_install.sh"), "unused", "unused"],
                env=dict(os.environ, HOME=temporary), capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(result.returncode, 2)
            self.assertFalse(list(Path(temporary).iterdir()))

    def test_session_worker_refuses_a_host_before_touching_home(self):
        with tempfile.TemporaryDirectory(prefix="licasa-session-guard-") as temporary:
            result = subprocess.run(
                ["python3", str(ROOT / "tools/check_linux_session.py"), "--desktop", "gnome",
                 "--appimage", "unused", "status"],
                env=dict(os.environ, HOME=temporary), capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("marked disposable QEMU/KVM guest", result.stderr)
            self.assertFalse(list(Path(temporary).iterdir()))


class DesktopImageAssertionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        cls.addClassCleanup(sys.path.remove, str(ROOT / "tools"))
        import verify_snap_desktop
        cls.pattern_present = staticmethod(verify_snap_desktop.pattern_present)
        cls.colors = verify_snap_desktop.PHOTO_COLORS

    def test_black_frame_and_welcome_screen_cannot_pass_as_a_visible_photo(self):
        from PIL import Image, ImageDraw
        image = Image.new("RGB", (1280, 800), "black")
        self.assertFalse(self.pattern_present(image))
        draw = ImageDraw.Draw(image)
        draw.text((300, 300), "Drop an image here", fill="white")
        self.assertFalse(self.pattern_present(image))
        for index, color in enumerate(self.colors):
            x, y = index % 2 * 240 + 100, index // 2 * 180 + 100
            draw.rectangle((x, y, x + 239, y + 179), fill=color)
        self.assertTrue(self.pattern_present(image))

    def test_failed_vm_activation_retains_report_and_marks_failure(self):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        try:
            spec = importlib.util.spec_from_file_location("vm_qualification", ROOT / "tools/verify_linux_vm_desktop.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
        finally:
            sys.path.remove(str(ROOT / "tools"))
        with tempfile.TemporaryDirectory(prefix="licasa-failed-session-") as temporary:
            args = ['verify_linux_vm_desktop.py', '--port', '1234', '--identity', temporary + '/key',
                    '--known-hosts', temporary + '/hosts', '--qmp', temporary + '/qmp',
                    '--desktop', 'gnome', '--guest-appimage', '/guest/Licasa.AppImage', '--output', temporary]
            failed = subprocess.CompletedProcess([], 1, '', 'No logged-in compositor')
            with patch('sys.argv', args), patch.object(module.subprocess, 'run', return_value=failed):
                with self.assertRaises(RuntimeError):
                    module.main()
            import json
            report = json.loads((Path(temporary) / 'desktop-report.json').read_text())
            self.assertEqual(report['status'], 'failed')
            self.assertEqual(report['routes'], {})
            self.assertIn('No logged-in compositor', (Path(temporary) / 'prepare.stderr').read_text())


if __name__ == "__main__":
    unittest.main()
