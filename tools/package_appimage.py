#!/usr/bin/env python3
"""Turn a checked amd64 Debian payload into a portable Qt AppImage."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
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


def fetch_tools(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in TOOLS.items():
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
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
        if name.endswith(".AppImage"):
            path.chmod(0o755)


def run(command: list[str], *, env: dict[str, str] | None = None, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, env=env, cwd=cwd)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Qualified Ubuntu 24.04 amd64 .deb")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/qualification/appimage")
    parser.add_argument("--tools-dir", type=Path, default=ROOT / "build/packaging-tools")
    args = parser.parse_args()
    missing_tools = [name for name in ("dpkg-deb", "qmake6", "file")
                     if shutil.which(name) is None]
    if missing_tools:
        parser.error("Missing AppImage build tools: " + ", ".join(missing_tools))
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
    fetch_tools(tools_dir)
    qt_plugins = Path(subprocess.check_output(
        ["qmake6", "-query", "QT_INSTALL_PLUGINS"], text=True).strip())
    qt_libraries = Path(subprocess.check_output(
        ["qmake6", "-query", "QT_INSTALL_LIBS"], text=True).strip())
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
    with tempfile.TemporaryDirectory(prefix="licasa-appimage-", dir=output) as temporary:
        work = Path(temporary)
        appdir = work / "Licasa.AppDir"
        appdir.mkdir()
        run(["dpkg-deb", "--extract", str(package), str(appdir)])
        desktop = appdir / "usr/share/applications/licasa.desktop"
        icon = appdir / "usr/share/icons/hicolor/512x512/apps/licasa.png"
        for path in (appdir / "usr/bin/licasa", desktop, icon):
            if not path.is_file():
                raise RuntimeError(f"Incomplete Debian payload: {path}")
        # The Qt deploy plugin only copies platform plugins. Wayland also loads
        # shell and buffer integrations at runtime, so stage those before the
        # dependency scanner runs.
        for group in wayland_plugin_groups:
            shutil.copytree(qt_plugins / group, appdir / "usr/plugins" / group)
        env = os.environ.copy()
        env.update(APPIMAGE_EXTRACT_AND_RUN="1", QML_SOURCES_PATHS=str(ROOT / "qml"),
                   VERSION=version, ARCH="x86_64",
                   EXTRA_PLATFORM_PLUGINS=";".join(
                       ("libqoffscreen.so", *wayland_platforms)))
        linuxdeploy = str(tools_dir / "linuxdeploy-x86_64.AppImage")
        run([linuxdeploy, "--appdir", str(appdir), "--desktop-file", str(desktop),
             "--icon-file", str(icon), "--library", str(wl_shell_library),
             "--plugin", "qt"], env=env, cwd=work)
        # Qt's QML plugins resolve Qt Quick through the top-level library path.
        # Point that name at our tested backport, rather than a second distro
        # Qt Quick copied by linuxdeploy.
        private_quick = appdir / "usr/lib/licasa/lib/libQt6Quick.so.6"
        bundled_quick = appdir / "usr/lib/libQt6Quick.so.6"
        if not private_quick.is_file() or not bundled_quick.is_file():
            raise RuntimeError("AppDir is missing Qt Quick or its private backport")
        bundled_quick.unlink()
        bundled_quick.symlink_to("licasa/lib/libQt6Quick.so.6")
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
        built = work / f"Licasa-{version}-x86_64.AppImage"
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
