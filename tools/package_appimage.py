#!/usr/bin/env python3
"""Assemble a checked amd64 Licasa payload as a portable Qt AppImage."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
from urllib.request import Request, urlopen

from qt_runtime import module_directory, requirements, validate_runtime
from lifecycle_update import install_appimage_launcher


ROOT = Path(__file__).resolve().parents[1]
# linuxdeploy excludes these by default, but minimal desktops need our copies
# for Qt's X11 plugins, libSM's UUID dependency, bundled FFmpeg/Pango,
# and the crypto/Kerberos dependency closure on minimal Debian desktops.
PORTABLE_RUNTIME_LIBRARIES = ("libSM.so.6", "libICE.so.6", "libuuid.so.1", "libfribidi.so.0",
                              "libgpg-error.so.0", "libcom_err.so.2")
GTK_PLATFORM_THEME = "platformthemes/libqgtk3.so"
GTK_SETTINGS_MODULE = "gio/modules/libdconfsettings.so"
JAMMY_LIBRARY_DIR = Path("/usr/lib/x86_64-linux-gnu")
JAMMY_ABI = {"GLIBC": (2, 35), "GLIBCXX": (3, 4, 30), "CXXABI": (1, 3, 13)}
TOOLS = {
    "linuxdeploy-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/"
        "1-alpha-20251107-1/linuxdeploy-x86_64.AppImage",
        "c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d",
    ),
    "linuxdeploy-plugin-qt-x86_64.AppImage": (
        "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/"
        "1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage",
        "15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724",
    ),
    "appimagetool-x86_64.AppImage": (
        "https://github.com/AppImage/appimagetool/releases/download/"
        "1.9.1/appimagetool-x86_64.AppImage",
        "ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0",
    ),
    "runtime-x86_64": (
        "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64",
        "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d",
    ),
}


def fetch_tools(directory: Path, *, offline: bool = False) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in TOOLS.items():
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            if offline:
                raise RuntimeError(f"Offline AppImage tool missing or mismatched: {path}")
            with tempfile.NamedTemporaryFile(dir=directory, delete=False) as target:
                temporary = Path(target.name)
            try:
                request = Request(url, headers={"User-Agent": "Licasa-packager"})
                with urlopen(request, timeout=120) as source, temporary.open("wb") as target:
                    shutil.copyfileobj(source, target)
                if hashlib.sha256(temporary.read_bytes()).hexdigest() != expected:
                    raise RuntimeError(f"Checksum mismatch for {name}")
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)
        if name.endswith(".AppImage") and not os.access(path, os.X_OK):
            path.chmod(0o755)


def run(command: list[str], *, env: dict[str, str] | None = None, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, env=env, cwd=cwd)


def set_wayland_plugin_runpaths(appdir: Path, groups: tuple[str, ...]) -> None:
    # These helpers are copied before linuxdeploy scans their dependencies, but
    # linuxdeploy does not rewrite the copied files' loader search paths. Qt
    # dlopens them after loading the generic Wayland platform, so they must find
    # their own bundled dependencies without relying on another plugin having
    # already loaded the EGL integration library.
    for group in groups:
        for plugin in sorted((appdir / "usr/plugins" / group).glob("*.so")):
            run(["patchelf", "--set-rpath", "$ORIGIN/../../lib:$ORIGIN", str(plugin)])


def stage_qml_modules(source: Path, appdir: Path) -> None:
    """Retain runtime-only imports before linuxdeploy scans ELF dependencies."""
    for module in requirements()["modules"]:
        if not module.get("deploy_explicitly"):
            continue
        directory = module_directory(source, module["uri"])
        if not (directory / "qmldir").is_file():
            raise RuntimeError(f"Missing QML runtime module {module['uri']}; install {module['package']}")
        destination = module_directory(appdir / "usr/qml", module["uri"])
        shutil.copytree(directory, destination, symlinks=True, dirs_exist_ok=True)


def set_qml_plugin_runpaths(appdir: Path) -> None:
    for module in requirements()["modules"]:
        if not module.get("deploy_explicitly"):
            continue
        directory = module_directory(appdir / "usr/qml", module["uri"])
        relative = os.path.relpath(appdir / "usr/lib", directory)
        for plugin in sorted(directory.glob("*.so")):
            run(["patchelf", "--set-rpath", f"$ORIGIN/{relative}:$ORIGIN", str(plugin)])


def preserve_desktop_theme_selection(appdir: Path) -> None:
    # The pinned deployer's hook unconditionally sets gtk3 on GNOME/XFCE,
    # overriding an explicit user setting and preventing a deliberate fallback.
    # The hook contains only that theme override; Qt already detects GTK
    # desktops (including MATE) and respects QT_QPA_PLATFORMTHEME itself.
    hook = appdir / "apprun-hooks/linuxdeploy-plugin-qt-hook.sh"
    if not hook.is_file():
        raise RuntimeError("AppDir is missing its Qt runtime hook")
    # GIO modules are an extension of our bundled GLib ABI. Newer host GVFS
    # modules can fail to resolve symbols against it; retain the matching dconf
    # settings backend and do not import unrelated host GIO modules.
    hook.write_text("# Licasa: Qt selects its desktop theme; use matching bundled GIO modules.\n"
                    'export GIO_MODULE_DIR="$this_dir/usr/lib/licasa/gio/modules"\n'
                    "unset GIO_EXTRA_MODULES\n")


def pin_multimedia_backend(appdir: Path) -> dict:
    """Every Qt plugin search order must select the qualified private backend."""
    private = appdir / "usr/lib/licasa/plugins/multimedia/libffmpegmediaplugin.so"
    canonical = appdir / "usr/plugins/multimedia/libffmpegmediaplugin.so"
    if not private.is_file():
        raise RuntimeError("AppDir is missing its qualified private FFmpeg plugin")
    # linuxdeploy also copies the distro multimedia backend into Qt's canonical
    # plugin directory. Its different search priority must not undo our fixes.
    run(["patchelf", "--set-rpath", "$ORIGIN/../../lib:$ORIGIN/../../..:$ORIGIN", str(private)])
    canonical.parent.mkdir(parents=True, exist_ok=True)
    canonical.unlink(missing_ok=True)
    canonical.symlink_to(os.path.relpath(private, canonical.parent))
    return {"private_path": str(private.relative_to(appdir)),
            "canonical_path": str(canonical.relative_to(appdir)),
            "sha256": hashlib.sha256(private.read_bytes()).hexdigest()}


def check_jammy_abi(appdir: Path) -> dict:
    """Enforce the host ABI on all bundled ELFs, including lazy plugins."""
    files = 0
    highest = {}
    system_only = {"libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1"}
    for path in sorted(appdir.rglob("*")):
        if path.name in system_only or path.name.startswith("ld-linux-"):
            raise RuntimeError(f"The AppImage must use the host runtime for {path.name}")
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as content:
            if content.read(4) != b"\x7fELF":
                continue
        files += 1
        versions = subprocess.check_output(["readelf", "--version-info", "--wide", str(path)],
                                           text=True, env=dict(os.environ, LC_ALL="C"))
        needs = versions.partition("Version needs section")[2]
        for family, version in re.findall(r"\bName: (GLIBCXX|GLIBC|CXXABI)_(\S+)", needs):
            if family == "CXXABI" and version in {"FLOAT128", "TM_1"}:
                continue
            if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+", version):
                raise RuntimeError(f"{path.relative_to(appdir)} requires unsupported {family}_{version}")
            parts = tuple(map(int, version.split(".")))
            if parts > JAMMY_ABI[family]:
                raise RuntimeError(f"{path.relative_to(appdir)} requires {family}_{version} above the Ubuntu 22.04 baseline")
            highest[family] = max(highest.get(family, ()), parts)
    if not files:
        raise RuntimeError("The AppImage payload contains no ELF files")
    return {"baseline": "ubuntu-22.04", "elf_files": files,
            "maximum_required": {family: ".".join(map(str, version)) for family, version in highest.items()}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, nargs="?", help="Legacy Ubuntu 24.04 amd64 .deb")
    parser.add_argument("--payload", type=Path, help="Staged Ubuntu 22.04 application from build_appimage.py")
    parser.add_argument("--version", help="Project version for --payload")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/qualification/appimage")
    parser.add_argument("--tools-dir", type=Path, default=ROOT / "build/packaging-tools")
    parser.add_argument("--offline", action="store_true", help="Require the pinned packaging tools in --tools-dir")
    args = parser.parse_args()
    if bool(args.package) == bool(args.payload) or bool(args.version) != bool(args.payload):
        parser.error("Supply either a Debian package or --payload with --version")
    qmake = "qmake6" if shutil.which("qmake6") else "qmake"
    payload_tools = ("readelf",) if args.payload else ("dpkg-deb",)
    missing_tools = [name for name in (*payload_tools, qmake, "file", "patchelf")
                     if shutil.which(name) is None]
    if missing_tools:
        parser.error("Missing AppImage build tools: " + ", ".join(missing_tools))
    if args.payload:
        if platform.machine() != "x86_64" or os.confstr("CS_GNU_LIBC_VERSION") != "glibc 2.35":
            parser.error("The portable payload requires the Ubuntu 22.04 amd64 builder")
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
            parser.error("Expected the Licasa project version")
        version = args.version
    else:
        package = args.package.resolve(strict=True)
        if subprocess.check_output(["dpkg-deb", "--field", str(package), "Package"], text=True).strip() != "licasa":
            parser.error("Expected a Licasa Debian package")
        architecture = subprocess.check_output(["dpkg-deb", "--field", str(package), "Architecture"], text=True).strip()
        if architecture != "amd64":
            parser.error("Only amd64 is qualified for the AppImage")
        version = subprocess.check_output(["dpkg-deb", "--field", str(package), "Version"], text=True).strip().split("-", 1)[0]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tools_dir = args.tools_dir.resolve()
    fetch_tools(tools_dir, offline=args.offline)
    qt_plugins = Path(subprocess.check_output(
        [qmake, "-query", "QT_INSTALL_PLUGINS"], text=True).strip())
    qt_libraries = Path(subprocess.check_output(
        [qmake, "-query", "QT_INSTALL_LIBS"], text=True).strip())
    qt_qml = Path(subprocess.check_output(
        [qmake, "-query", "QT_INSTALL_QML"], text=True).strip())
    qt_version = subprocess.check_output([qmake, "-query", "QT_VERSION"], text=True).strip()
    if qt_version != "6.4.2":
        raise RuntimeError("The portable Ubuntu payload must be deployed with matching Qt 6.4.2")
    gtk_theme = qt_plugins / GTK_PLATFORM_THEME
    if not gtk_theme.is_file():
        raise RuntimeError("Missing matching Qt GTK platform theme; install qt6-gtk-platformtheme")
    system_libraries = JAMMY_LIBRARY_DIR if args.payload else qt_libraries
    gtk_settings = system_libraries / GTK_SETTINGS_MODULE
    if not gtk_settings.is_file():
        raise RuntimeError("Missing matching GTK settings backend; install dconf-gsettings-backend")
    # This also rejects a partially installed build environment before any bundle is made.
    validate_runtime(qt_qml, qt_libraries, qt_version)
    wayland_platforms = ("libqwayland-egl.so", "libqwayland-generic.so")
    for plugin in wayland_platforms:
        if not (qt_plugins / "platforms" / plugin).is_file():
            raise RuntimeError(f"Missing Qt Wayland platform plugin: {plugin}; install qt6-wayland")
    wayland_plugin_groups = (
        "wayland-decoration-client",
        "wayland-graphics-integration-client",
        "wayland-shell-integration",
    )
    for group in wayland_plugin_groups:
        if not (qt_plugins / group).is_dir():
            raise RuntimeError(f"Missing Qt Wayland plugin group: {group}; install qt6-wayland")
    wl_shell_library = qt_libraries / "libQt6WlShellIntegration.so.6"
    if not wl_shell_library.is_file():
        raise RuntimeError("Missing Qt Wayland shell integration library; install qt6-wayland")
    wayland_egl_library = qt_libraries / "libQt6WaylandEglClientHwIntegration.so.6"
    qml_libraries = tuple(qt_libraries / name for module in requirements()["modules"]
                          if module.get("deploy_explicitly") for name in module.get("libraries", []))
    forced_libraries = (wl_shell_library, wayland_egl_library,
                        *(system_libraries / name for name in PORTABLE_RUNTIME_LIBRARIES), *qml_libraries)
    for library in forced_libraries:
        if not library.is_file():
            raise RuntimeError(f"Missing AppImage runtime library: {library}")
    with tempfile.TemporaryDirectory(prefix="licasa-appimage-", dir=output) as temporary:
        work = Path(temporary)
        appdir = work / "Licasa.AppDir"
        if args.payload:
            shutil.copytree(args.payload.resolve(strict=True), appdir, symlinks=True)
        else:
            appdir.mkdir()
            run(["dpkg-deb", "--extract", str(package), str(appdir)])
        # A native Debian transaction uses a root-owned launcher before its
        # ELF. Portable packages have their own managed/portable launch gate,
        # so restore the ordinary payload layout before linuxdeploy scans it.
        native_binary = appdir / "usr/bin/licasa.bin"
        if native_binary.is_file():
            (appdir / "usr/bin/licasa").unlink(missing_ok=True)
            native_binary.rename(appdir / "usr/bin/licasa")
        for native_only in ("usr/lib/licasa/native_lifecycle.py", "usr/share/licasa/native-runtime.json"):
            (appdir / native_only).unlink(missing_ok=True)
        desktop = appdir / "usr/share/applications/licasa.desktop"
        icon = appdir / "usr/share/icons/hicolor/512x512/apps/licasa.png"
        for path in (appdir / "usr/bin/licasa", desktop, icon):
            if not path.is_file():
                raise RuntimeError(f"Incomplete AppImage payload: {path}")
        # The Qt deploy plugin only copies platform plugins. Wayland also loads
        # shell and buffer integrations at runtime, so stage those before the
        # dependency scanner runs.
        for group in wayland_plugin_groups:
            shutil.copytree(qt_plugins / group, appdir / "usr/plugins" / group)
        # A QML FileDialog selects native GTK dialogs through this plugin, not
        # through its QML imports. Copy the matching build-runtime plugin before
        # linuxdeploy scans ELF dependencies, including GTK on minimal systems.
        # Leave theme selection to Qt/the desktop; never load host Qt plugins.
        bundled_gtk_theme = appdir / "usr/plugins" / GTK_PLATFORM_THEME
        bundled_gtk_theme.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(gtk_theme, bundled_gtk_theme)
        bundled_gtk_settings = appdir / "usr/lib/licasa" / GTK_SETTINGS_MODULE
        bundled_gtk_settings.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(gtk_settings, bundled_gtk_settings)
        stage_qml_modules(qt_qml, appdir)
        env = os.environ.copy()
        env.update(APPIMAGE_EXTRACT_AND_RUN="1", QML_SOURCES_PATHS=str(ROOT / "qml"),
                   VERSION=version, ARCH="x86_64",
                   EXTRA_PLATFORM_PLUGINS=";".join(
                       ("libqoffscreen.so", *wayland_platforms)))
        if args.payload:
            env["QMAKE"] = shutil.which(qmake)
        linuxdeploy = str(tools_dir / "linuxdeploy-x86_64.AppImage")
        run([linuxdeploy, "--appdir", str(appdir), "--desktop-file", str(desktop),
             "--icon-file", str(icon),
             "--deploy-deps-only", str(bundled_gtk_theme),
             "--deploy-deps-only", str(bundled_gtk_settings),
             *(argument for library in forced_libraries for argument in ("--library", str(library))),
             "--plugin", "qt"], env=env, cwd=work)
        preserve_desktop_theme_selection(appdir)
        set_wayland_plugin_runpaths(appdir, wayland_plugin_groups)
        set_wayland_plugin_runpaths(appdir, ("platformthemes",))
        run(["patchelf", "--set-rpath", "$ORIGIN/../../..:$ORIGIN", str(bundled_gtk_settings)])
        set_qml_plugin_runpaths(appdir)
        multimedia = pin_multimedia_backend(appdir)
        # Explicit --library requests override linuxdeploy's default exclusion
        # list and retain its dependency and copyright-file deployment.
        for library in forced_libraries:
            if not (appdir / "usr/lib" / library.name).is_file():
                raise RuntimeError(f"AppDir is missing the bundled runtime library: {library.name}")
        # Qt's QML plugins resolve Qt Quick through the top-level library path.
        # Point that name at our tested backport, rather than a second distro
        # Qt Quick copied by linuxdeploy.
        private_quick = appdir / "usr/lib/licasa/lib/libQt6Quick.so.6"
        bundled_quick = appdir / "usr/lib/libQt6Quick.so.6"
        if not private_quick.is_file() or not bundled_quick.is_file():
            raise RuntimeError("AppDir is missing Qt Quick or its private backport")
        bundled_quick.unlink()
        bundled_quick.symlink_to("licasa/lib/libQt6Quick.so.6")
        # FFmpeg's dependency scan can find the distro dav1d before AVIF's
        # pinned private copy, depending on traversal order. Both have the same
        # SONAME. Resolve all consumers to the published private runtime so a
        # rebuild cannot silently select a different decoder implementation.
        private_dav1d = appdir / "usr/lib/licasa/lib/libdav1d.so.7"
        bundled_dav1d = appdir / "usr/lib/libdav1d.so.7"
        if not private_dav1d.is_file() or not bundled_dav1d.is_file():
            raise RuntimeError("AppDir is missing dav1d or its pinned private runtime")
        bundled_dav1d.unlink()
        bundled_dav1d.symlink_to("licasa/lib/libdav1d.so.7")
        for plugin in ("libqxcb.so", "libqoffscreen.so", *wayland_platforms):
            if not (appdir / "usr/plugins/platforms" / plugin).is_file():
                raise RuntimeError(f"AppDir is missing Qt platform plugin: {plugin}")
        for group, plugin in (("wayland-shell-integration", "libxdg-shell.so"),
                              ("wayland-graphics-integration-client", "libqt-plugin-wayland-egl.so")):
            if not (appdir / "usr/plugins" / group / plugin).is_file():
                raise RuntimeError(f"AppDir is missing Qt Wayland helper: {plugin}")
        if not (appdir / "usr/lib/libQt6WlShellIntegration.so.6").is_file():
            raise RuntimeError("AppDir is missing the Wayland shell compatibility library")
        # Licasa's image plugins live outside linuxdeploy's normal Qt plugin
        # directory. Keep the JP2 decoder alongside its plugin so the plugin's
        # $ORIGIN search path finds it even on distros without OpenJPEG.
        jp2_library = appdir / "usr/lib/libopenjp2.so.7"
        jp2_plugin = appdir / "usr/lib/licasa/plugins/imageformats/liblicasa_jp2_plugin.so"
        if not jp2_library.is_file() or not jp2_plugin.is_file():
            raise RuntimeError("AppDir is missing the bundled JPEG 2000 reader")
        shutil.copy2(jp2_library, jp2_plugin.parent / jp2_library.name)
        runtime_report = validate_runtime(appdir / "usr/qml", appdir / "usr/lib", qt_version)
        if not bundled_gtk_theme.is_file():
            raise RuntimeError("AppDir is missing its matching Qt GTK platform theme")
        for library in ("libgtk-3.so.0", "libgdk-3.so.0", "libpangoft2-1.0.so.0"):
            if not (appdir / "usr/lib" / library).is_file():
                raise RuntimeError(f"AppDir is missing a GTK runtime library: {library}")
        runtime_report["native_file_dialog"] = {
            "provider": "gtk3", "qt_version": qt_version,
            "plugin": str(bundled_gtk_theme.relative_to(appdir)),
            "sha256": hashlib.sha256(bundled_gtk_theme.read_bytes()).hexdigest(),
            "selection": "desktop default; Qt Quick fallback remains available",
            "settings_module": str(bundled_gtk_settings.relative_to(appdir)),
            "settings_sha256": hashlib.sha256(bundled_gtk_settings.read_bytes()).hexdigest(),
        }
        runtime_report["multimedia_backend"] = multimedia
        if args.payload:
            runtime_report["abi"] = check_jammy_abi(appdir)
        report_path = appdir / "usr/share/licasa/qt-runtime.json"
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(runtime_report, indent=2) + "\n")
        built = work / f"Licasa-{version}-x86_64.AppImage"
        install_appimage_launcher(appdir)
        run([str(tools_dir / "appimagetool-x86_64.AppImage"), "--runtime-file",
             str(tools_dir / "runtime-x86_64"), str(appdir), str(built)],
            env=env, cwd=work)
        destination = output / f"Licasa-{version}-x86_64.AppImage"
        shutil.move(built, destination)
        destination.chmod(0o755)
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_name(destination.name + ".sha256").write_text(
        f"{digest}  {destination.name}\n")
    print(f"AppImage: {destination}\nSHA-256: {digest}")


if __name__ == "__main__":
    main()
