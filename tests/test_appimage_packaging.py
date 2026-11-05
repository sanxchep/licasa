#!/usr/bin/env python3
"""Exercise AppImage assembly with fake deployment tools; no real package is built."""

import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("appimage_packager", ROOT / "tools/package_appimage.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class AppImagePackagingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="licasa-appimage-assembly-fixture-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.libraries = self.work / "qt/lib"
        self.plugins = self.work / "qt/plugins"
        self.qml = self.work / "qt/qml"
        self.write(self.libraries / packager.GTK_SETTINGS_MODULE)
        for name in ("libQt6WlShellIntegration.so.6", "libQt6WaylandEglClientHwIntegration.so.6",
                     "libSM.so.6", "libICE.so.6", "libuuid.so.1", "libfribidi.so.0",
                     "libgpg-error.so.0", "libcom_err.so.2", "libQt6LabsFolderListModel.so.6"):
            self.write(self.libraries / name, name.encode())
        for module in packager.requirements()["modules"]:
            directory = packager.module_directory(self.qml, module["uri"])
            plugins = module["plugins"]["6.4.2"]
            metadata = f"module {module['uri']}\n" + "".join(f"plugin {name}\n" for name in plugins)
            self.write(directory / "qmldir", metadata.encode())
            for name in plugins:
                self.write(directory / f"lib{name}.so")
        for name in ("platforms/libqwayland-egl.so", "platforms/libqwayland-generic.so",
                     "platformthemes/libqgtk3.so",
                     "wayland-decoration-client/decoration.so",
                     "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so",
                     "wayland-shell-integration/libxdg-shell.so"):
            self.write(self.plugins / name)
        self.payload = self.work / "payload"
        self.write(self.payload / "usr/libexec/licasa/lifecycle_update.py", b"# lifecycle fixture\n")
        for name in ("usr/bin/licasa", "usr/share/applications/licasa.desktop",
                     "usr/share/icons/hicolor/512x512/apps/licasa.png",
                     "usr/lib/licasa/lib/libQt6Quick.so.6", "usr/lib/licasa/lib/libdav1d.so.7",
                     "usr/lib/libopenjp2.so.7",
                     "usr/lib/licasa/plugins/multimedia/libffmpegmediaplugin.so",
                     "usr/lib/licasa/plugins/imageformats/liblicasa_jp2_plugin.so"):
            self.write(self.payload / name)
        self.package = self.work / "fixture.deb"
        self.write(self.package)
        self.output = self.work / "output"
        self.deployment_commands = []
        self.patched_helpers = {}
        self.image_created = False
        self.omit_library = None
        self.omit_dav1d = False
        self.omit_module = None
        self.skip_style_scan = False
        self.deployed_dav1d = b"distro dav1d selected by dependency scan"
        self.deployed_ffmpeg = b"unpatched distro multimedia backend"

    def write(self, path, content=b"synthetic fixture"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def metadata(self, command, **kwargs):
        if command[0] == "dpkg-deb":
            return {"Package": "licasa", "Architecture": "amd64", "Version": "0.7.1-1"}[command[-1]]
        return str({"QT_INSTALL_PLUGINS": self.plugins, "QT_INSTALL_LIBS": self.libraries,
                    "QT_INSTALL_QML": self.qml, "QT_VERSION": "6.4.2"}[command[-1]])

    def run_tool(self, command, **kwargs):
        name = Path(command[0]).name
        if name == "dpkg-deb":
            shutil.copytree(self.payload, Path(command[-1]), dirs_exist_ok=True)
        elif name == "linuxdeploy-x86_64.AppImage":
            self.deployment_commands.append(command)
            appdir = Path(command[command.index("--appdir") + 1])
            self.write(appdir / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh",
                       b"export QT_QPA_PLATFORMTHEME=gtk3\n")
            self.write(appdir / "AppRun", b'#!/bin/bash\nexec "$this_dir/AppRun.wrapped" "$@"\n')
            self.assertEqual(Path(command[command.index("--deploy-deps-only") + 1]),
                             appdir / "usr/plugins" / packager.GTK_PLATFORM_THEME)
            for library in ("libgtk-3.so.0", "libgdk-3.so.0", "libpangoft2-1.0.so.0"):
                if library != self.omit_library:
                    self.write(appdir / "usr/lib" / library)
            for plugin in ("libqxcb.so", "libqoffscreen.so", "libqwayland-egl.so", "libqwayland-generic.so"):
                self.write(appdir / "usr/plugins/platforms" / plugin)
            self.write(appdir / "usr/lib/libQt6Quick.so.6")
            self.write(appdir / "usr/plugins/multimedia/libffmpegmediaplugin.so", self.deployed_ffmpeg)
            # Runtime style selection is not a source import that the deployer
            # can always discover. Explicit staging must survive that case.
            def ignore_styles(directory, names):
                return ["Basic"] if self.skip_style_scan and Path(directory) == self.qml / "QtQuick/Controls" else []
            shutil.copytree(self.qml, appdir / "usr/qml", dirs_exist_ok=True, ignore=ignore_styles)
            if self.omit_module:
                shutil.rmtree(packager.module_directory(appdir / "usr/qml", self.omit_module))
            if not self.omit_dav1d:
                self.write(appdir / "usr/lib/libdav1d.so.7", self.deployed_dav1d)
            # Model the failure: excluded libraries are copied only when explicitly requested.
            for index, argument in enumerate(command):
                if argument == "--library":
                    source = Path(command[index + 1])
                    if source.name != self.omit_library:
                        shutil.copy2(source, appdir / "usr/lib" / source.name)
        elif name == "appimagetool-x86_64.AppImage":
            appdir = Path(command[-2])
            self.assertIn("--family appimage", (appdir / "AppRun").read_text())
            self.assertIn("AppRun.wrapped", (appdir / "AppRun.lifecycle-original").read_text())
            self.assertNotIn("QT_QPA_PLATFORMTHEME=", (appdir / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh").read_text())
            self.assertIn('GIO_MODULE_DIR="$this_dir/usr/lib/licasa/gio/modules"',
                          (appdir / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh").read_text())
            self.assertTrue((appdir / "usr/lib/licasa" / packager.GTK_SETTINGS_MODULE).is_file())
            for library in packager.PORTABLE_RUNTIME_LIBRARIES:
                self.assertEqual((appdir / "usr/lib" / library).read_bytes(), library.encode())
            quick = appdir / "usr/lib/libQt6Quick.so.6"
            self.assertTrue(quick.is_symlink())
            self.assertEqual(quick.readlink(), Path("licasa/lib/libQt6Quick.so.6"))
            dav1d = appdir / "usr/lib/libdav1d.so.7"
            self.assertTrue(dav1d.is_symlink())
            self.assertEqual(dav1d.readlink(), Path("licasa/lib/libdav1d.so.7"))
            self.assertEqual(dav1d.read_bytes(), (self.payload / "usr/lib/licasa/lib/libdav1d.so.7").read_bytes())
            ffmpeg = appdir / "usr/plugins/multimedia/libffmpegmediaplugin.so"
            private_ffmpeg = appdir / "usr/lib/licasa/plugins/multimedia/libffmpegmediaplugin.so"
            self.assertTrue(ffmpeg.is_symlink())
            self.assertEqual(ffmpeg.resolve(), private_ffmpeg.resolve())
            self.assertEqual(ffmpeg.read_bytes(), (self.payload / private_ffmpeg.relative_to(appdir)).read_bytes())
            self.assertEqual(set(self.patched_helpers), {
                "wayland-decoration-client/decoration.so",
                "wayland-graphics-integration-client/libqt-plugin-wayland-egl.so",
                "wayland-shell-integration/libxdg-shell.so",
                "platformthemes/libqgtk3.so",
            })
            self.assertEqual(set(self.patched_helpers.values()), {"$ORIGIN/../../lib:$ORIGIN"})
            self.assertTrue((appdir / "usr/lib/libQt6WaylandEglClientHwIntegration.so.6").is_file())
            self.write(Path(command[-1]), b"synthetic image output")
            self.image_created = True
        elif name == "patchelf":
            self.assertEqual(command[1], "--set-rpath")
            plugin = Path(command[-1])
            self.assertTrue(plugin.is_file())
            self.assertTrue(self.deployment_commands)
            self.assertFalse(self.image_created)
            if "wayland-" in str(plugin) or "platformthemes" in str(plugin):
                self.patched_helpers["/".join(plugin.parts[-2:])] = command[2]
        else:
            self.fail(f"Unexpected tool request: {command}")

    def assemble(self):
        with patch.object(sys, "argv", ["package_appimage.py", str(self.package),
                                        "--output-dir", str(self.output)]), \
                patch.object(packager.shutil, "which", return_value="fixture-tool"), \
                patch.object(packager.subprocess, "check_output", side_effect=self.metadata), \
                patch.object(packager, "fetch_tools"), \
                patch.object(packager, "run", side_effect=self.run_tool):
            packager.main()

    def test_image_contains_excluded_runtime_libraries_and_private_qt_quick(self):
        self.assemble()
        self.assertTrue(self.image_created)
        image = self.output / "Licasa-0.7.1-x86_64.AppImage"
        digest = hashlib.sha256(image.read_bytes()).hexdigest()
        self.assertEqual(image.with_name(image.name + ".sha256").read_text(), f"{digest}  {image.name}\n")

    def test_jammy_checks_version_needs_in_lazy_plugins(self):
        plugin = self.payload / "usr/lib/licasa/plugins/imageformats/lazy.so"
        self.write(plugin, b"\x7fELF fixture")
        versions = ("Version definition section '.gnu.version_d':\n  Name: GLIBC_2.38\n"
                    "Version needs section '.gnu.version_r':\n  Name: GLIBC_2.35\n"
                    "  Name: GLIBCXX_3.4.30\n  Name: CXXABI_1.3.13\n")
        with patch.object(packager.subprocess, "check_output", return_value=versions) as readelf:
            report = packager.check_jammy_abi(self.payload)
            self.assertEqual(report["maximum_required"],
                             {"GLIBC": "2.35", "GLIBCXX": "3.4.30", "CXXABI": "1.3.13"})
            self.assertEqual(report["elf_files"], 1)
            self.assertEqual(readelf.call_args.args[0][-1], str(plugin))
            for requirement in ("GLIBC_2.36", "GLIBCXX_3.4.31", "GLIBC_ABI_DT_RELR"):
                with self.subTest(requirement=requirement):
                    readelf.return_value = versions + f"  Name: {requirement}\n"
                    with self.assertRaisesRegex(RuntimeError, "lazy.so requires.*" + requirement):
                        packager.check_jammy_abi(self.payload)

    def test_jammy_rejects_a_bundled_system_cpp_runtime(self):
        (self.payload / "usr/lib/libstdc++.so.6").symlink_to("libstdc++.so.6.0.30")
        with self.assertRaisesRegex(RuntimeError, "host runtime for libstdc"):
            packager.check_jammy_abi(self.payload)

    def test_offline_packaging_rejects_a_missing_tool_without_fetching(self):
        with patch.object(packager, "urlopen") as download:
            with self.assertRaisesRegex(RuntimeError, "Offline AppImage tool missing"):
                packager.fetch_tools(self.work / "empty-tools", offline=True)
            download.assert_not_called()

    def test_native_debian_launcher_is_normalized_before_portable_assembly(self):
        self.write(self.payload / "usr/bin/licasa.bin", b"native actual ELF fixture")
        self.write(self.payload / "usr/bin/licasa", b"native root-gated wrapper")
        self.write(self.payload / "usr/lib/licasa/native_lifecycle.py", b"native only")
        self.write(self.payload / "usr/share/licasa/native-runtime.json", b"native only")
        original = self.run_tool
        def inspect(command, **kwargs):
            if Path(command[0]).name == "linuxdeploy-x86_64.AppImage":
                appdir = Path(command[command.index("--appdir") + 1])
                self.assertEqual((appdir / "usr/bin/licasa").read_bytes(), b"native actual ELF fixture")
                self.assertFalse((appdir / "usr/bin/licasa.bin").exists())
                self.assertFalse((appdir / "usr/lib/licasa/native_lifecycle.py").exists())
                self.assertFalse((appdir / "usr/share/licasa/native-runtime.json").exists())
            return original(command, **kwargs)
        with patch.object(self, "run_tool", side_effect=inspect):
            self.assemble()

    def test_missing_system_library_stops_before_deployment(self):
        (self.libraries / "libfribidi.so.0").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing AppImage runtime library: .*libfribidi"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_missing_matching_gtk_theme_stops_before_deployment(self):
        (self.plugins / packager.GTK_PLATFORM_THEME).unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing matching Qt GTK platform theme"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_deployer_dropping_gtk_dependency_cannot_produce_an_image(self):
        self.omit_library = "libpangoft2-1.0.so.0"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing a GTK runtime library"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_missing_gtk_settings_backend_stops_before_deployment(self):
        (self.libraries / packager.GTK_SETTINGS_MODULE).unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing matching GTK settings backend"):
            self.assemble()
        self.assertFalse(self.deployment_commands)

    def test_launcher_preserves_explicit_qt_theme_and_isolates_gio_modules(self):
        appdir = self.work / "AppDir with spaces"
        hook = appdir / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh"
        self.write(hook, b"export QT_QPA_PLATFORMTHEME=gtk3\n")
        packager.preserve_desktop_theme_selection(appdir)
        environment = dict(os.environ, QT_QPA_PLATFORMTHEME="deliberate-user-fallback",
                           GIO_MODULE_DIR="/foreign-host/modules", GIO_EXTRA_MODULES="/foreign-host/extra")
        command = ["bash", "-ec", '. "$1"; printf "%s\\n" "${QT_QPA_PLATFORMTHEME-unset}" "$GIO_MODULE_DIR" "${GIO_EXTRA_MODULES-unset}"',
                   "_", str(hook)]
        environment["this_dir"] = str(appdir)
        result = subprocess.check_output(command, env=environment, text=True)
        self.assertEqual(result.splitlines(), ["deliberate-user-fallback", str(appdir / "usr/lib/licasa/gio/modules"), "unset"])
        environment.pop("QT_QPA_PLATFORMTHEME")
        result = subprocess.check_output(command, env=environment, text=True)
        self.assertEqual(result.splitlines()[0], "unset")

    def test_missing_folderlistmodel_stops_before_deployment(self):
        shutil.rmtree(packager.module_directory(self.qml, "Qt.labs.folderlistmodel"))
        with self.assertRaisesRegex(RuntimeError, "Missing QML runtime file"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_deployer_dropping_dialog_dependency_cannot_produce_an_image(self):
        self.omit_module = "Qt.labs.folderlistmodel"
        with self.assertRaisesRegex(RuntimeError, "Missing QML runtime file"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_missing_basic_style_stops_before_deployment(self):
        shutil.rmtree(packager.module_directory(self.qml, "QtQuick.Controls.Basic"))
        with self.assertRaisesRegex(RuntimeError, "Missing QML runtime file"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_missing_basic_style_implementation_stops_before_deployment(self):
        shutil.rmtree(packager.module_directory(self.qml, "QtQuick.Controls.Basic.impl"))
        with self.assertRaisesRegex(RuntimeError, "Missing QML runtime file"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_deployer_dropping_fallback_style_cannot_produce_an_image(self):
        for uri in ("QtQuick.Controls.Basic", "QtQuick.Controls.Basic.impl"):
            with self.subTest(uri=uri):
                self.omit_module = uri
                self.image_created = False
                with self.assertRaisesRegex(RuntimeError, "Missing QML runtime file"):
                    self.assemble()
                self.assertFalse(self.image_created)

    def test_runtime_only_fallback_style_survives_missing_scanner_discovery(self):
        self.skip_style_scan = True
        self.assemble()
        self.assertTrue(self.image_created)

    def test_deployer_dropping_gpg_error_cannot_produce_an_image(self):
        self.omit_library = "libgpg-error.so.0"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing.*libgpg-error"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_deployer_dropping_com_err_cannot_produce_an_image(self):
        self.omit_library = "libcom_err.so.2"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing.*libcom_err"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_deployer_dropping_folderlistmodel_library_cannot_produce_an_image(self):
        self.omit_library = "libQt6LabsFolderListModel.so.6"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing.*libQt6LabsFolderListModel"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_incomplete_deployment_stops_before_image_creation(self):
        self.omit_library = "libICE.so.6"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing.*libICE.so.6"):
            self.assemble()
        self.assertFalse(self.image_created)


    def test_missing_wayland_egl_library_stops_before_deployment(self):
        (self.libraries / "libQt6WaylandEglClientHwIntegration.so.6").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing AppImage runtime library: .*WaylandEgl"):
            self.assemble()
        self.assertFalse(self.deployment_commands)
        self.assertFalse(self.image_created)

    def test_missing_deployed_wayland_egl_library_stops_before_image_creation(self):
        self.omit_library = "libQt6WaylandEglClientHwIntegration.so.6"
        with self.assertRaisesRegex(RuntimeError, "AppDir is missing.*WaylandEgl"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_missing_patchelf_fails_preflight(self):
        with patch.object(sys, "argv", ["package_appimage.py", str(self.package)]), \
                patch.object(packager.shutil, "which", side_effect=lambda name: None if name == "patchelf" else name), \
                patch.object(packager, "fetch_tools") as fetch:
            with self.assertRaises(SystemExit) as failure:
                packager.main()
        self.assertEqual(failure.exception.code, 2)
        fetch.assert_not_called()

    def test_dav1d_always_resolves_to_private_runtime_regardless_of_scan_selection(self):
        for deployed in (b"older distro dav1d", b"private dav1d found first"):
            with self.subTest(deployed=deployed):
                self.image_created = False
                self.deployed_dav1d = deployed
                self.assemble()
                self.assertTrue(self.image_created)

    def test_distro_multimedia_backend_cannot_override_qualified_private_plugin(self):
        for deployed in (b"unpatched distro plugin", b"different deployer plugin"):
            with self.subTest(deployed=deployed):
                self.image_created = False
                self.deployed_ffmpeg = deployed
                self.assemble()
                self.assertTrue(self.image_created)

    def test_missing_private_multimedia_backend_cannot_fall_back_to_distro_plugin(self):
        (self.payload / "usr/lib/licasa/plugins/multimedia/libffmpegmediaplugin.so").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing its qualified private FFmpeg plugin"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_missing_private_dav1d_stops_before_image_creation(self):
        (self.payload / "usr/lib/licasa/lib/libdav1d.so.7").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing dav1d or its pinned private runtime"):
            self.assemble()
        self.assertFalse(self.image_created)

    def test_missing_deployed_dav1d_stops_before_image_creation(self):
        self.omit_dav1d = True
        with self.assertRaisesRegex(RuntimeError, "missing dav1d or its pinned private runtime"):
            self.assemble()
        self.assertFalse(self.image_created)


@unittest.skipUnless(shutil.which("cc") and shutil.which("patchelf"), "requires a C compiler and patchelf")
class WaylandPluginLoaderTests(unittest.TestCase):
    def test_fallback_style_plugins_load_bundled_dependencies_without_overrides(self):
        with tempfile.TemporaryDirectory(prefix="licasa-qml-loader-fixture-") as temporary:
            work = Path(temporary)
            appdir = work / "AppDir with spaces"
            libraries = appdir / "usr/lib"
            libraries.mkdir(parents=True)
            dependency = "liblicasa_qml_loader_fixture.so.1"
            source = work / "dependency.c"
            source.write_text("int licasa_qml_fixture_value(void) { return 42; }\n")
            subprocess.run(["cc", "-shared", "-fPIC", f"-Wl,-soname,{dependency}", str(source),
                            "-o", str(libraries / dependency)], check=True, capture_output=True)
            source = work / "plugin.c"
            source.write_text("extern int licasa_qml_fixture_value(void);\n"
                              "int licasa_qml_fixture_result(void) { return licasa_qml_fixture_value(); }\n")
            plugins = []
            env = {key: value for key, value in os.environ.items()
                   if key not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")}
            probe = [sys.executable, "-c", "import ctypes, sys; p = ctypes.CDLL(sys.argv[1]); "
                     "assert p.licasa_qml_fixture_result() == 42"]
            for uri in ("QtQuick.Controls.Basic", "QtQuick.Controls.Basic.impl"):
                directory = packager.module_directory(appdir / "usr/qml", uri)
                directory.mkdir(parents=True, exist_ok=True)
                plugin = directory / "libfixture.so"
                subprocess.run(["cc", "-shared", "-fPIC", str(source), f"-L{libraries}",
                                f"-l:{dependency}", "-o", str(plugin)], check=True, capture_output=True)
                baseline = subprocess.run([*probe, str(plugin)], env=env, text=True, capture_output=True)
                self.assertNotEqual(baseline.returncode, 0)
                self.assertIn(dependency, baseline.stderr)
                plugins.append(plugin)
            packager.set_qml_plugin_runpaths(appdir)
            for plugin in plugins:
                loaded = subprocess.run([*probe, str(plugin)], env=env, text=True, capture_output=True)
                self.assertEqual(loaded.returncode, 0, loaded.stderr)

    def test_helper_loads_its_bundled_dependency_without_loader_overrides(self):
        with tempfile.TemporaryDirectory(prefix="licasa-wayland-loader-fixture-") as temporary:
            work = Path(temporary)
            appdir = work / "AppDir with spaces"
            library_dir = appdir / "usr/lib"
            plugin_dir = appdir / "usr/plugins/wayland-graphics-integration-client"
            library_dir.mkdir(parents=True)
            plugin_dir.mkdir(parents=True)
            dependency_name = "liblicasa_wayland_loader_fixture.so.1"
            dependency_source = work / "dependency.c"
            dependency_source.write_text("int licasa_loader_fixture_value(void) { return 42; }\n")
            plugin_source = work / "plugin.c"
            plugin_source.write_text("extern int licasa_loader_fixture_value(void);\n"
                                     "int licasa_loader_fixture_result(void) { return licasa_loader_fixture_value(); }\n")
            subprocess.run(["cc", "-shared", "-fPIC", f"-Wl,-soname,{dependency_name}",
                            str(dependency_source), "-o", str(library_dir / dependency_name)],
                           check=True, capture_output=True)
            plugin = plugin_dir / "libfixture.so"
            subprocess.run(["cc", "-shared", "-fPIC", str(plugin_source), f"-L{library_dir}",
                            f"-l:{dependency_name}", "-o", str(plugin)], check=True, capture_output=True)
            env = {key: value for key, value in os.environ.items()
                   if key not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT")}
            command = [sys.executable, "-c", "import ctypes, sys; "
                       "plugin = ctypes.CDLL(sys.argv[1]); "
                       "assert plugin.licasa_loader_fixture_result() == 42", str(plugin)]
            baseline = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertNotEqual(baseline.returncode, 0)
            self.assertIn(dependency_name, baseline.stderr)
            packager.set_wayland_plugin_runpaths(appdir, ("wayland-graphics-integration-client",))
            fixed = subprocess.run(command, env=env, text=True, capture_output=True)
            self.assertEqual(fixed.returncode, 0, fixed.stderr)
            runpath = subprocess.check_output(["patchelf", "--print-rpath", str(plugin)], text=True).strip()
            self.assertEqual(runpath, "$ORIGIN/../../lib:$ORIGIN")


if __name__ == "__main__":
    unittest.main()
