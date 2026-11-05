#!/usr/bin/env python3
"""Validate the QML runtime closure used by native and portable packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "packaging/qt-runtime-modules.json"


def requirements() -> dict:
    data = json.loads(MANIFEST.read_text())
    if data.get("schema_version") != 1:
        raise RuntimeError("Unsupported Qt runtime manifest schema")
    uris = [module["uri"] for module in data["modules"]]
    if len(uris) != len(set(uris)) or any(
            not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*", uri)
            for uri in uris):
        raise RuntimeError("Invalid or duplicate QML runtime module")
    for module in data["modules"]:
        profiles = module.get("plugins", {})
        if set(profiles) != set(data["qt_versions"]):
            raise RuntimeError(f"QML module {module['uri']} needs explicit plugins for every Qt profile")
        for version, plugins in profiles.items():
            if (not plugins or len(plugins) != len(set(plugins))
                    or any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", plugin) for plugin in plugins)):
                raise RuntimeError(f"Invalid QML plugin contract for {module['uri']} on Qt {version}")
    return data


def runtime_packages() -> tuple[str, ...]:
    data = requirements()
    return tuple(dict.fromkeys([*data["additional_packages"],
                                *(module["package"] for module in data["modules"])]))


def module_directory(qml_root: Path, uri: str) -> Path:
    return qml_root.joinpath(*uri.split("."))


def checked_file(path: Path, root: Path) -> str:
    if not path.is_file():
        raise RuntimeError(f"Missing QML runtime file: {path}")
    if not path.resolve().is_relative_to(root.resolve()):
        raise RuntimeError(f"QML runtime file escapes its runtime root: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_runtime(qml_root: Path, library_root: Path, qt_version: str) -> dict:
    data = requirements()
    if qt_version not in data["qt_versions"]:
        raise RuntimeError(f"Qt {qt_version} has no qualified runtime-module profile")
    modules = []
    for module in data["modules"]:
        directory = module_directory(qml_root, module["uri"])
        qmldir = directory / "qmldir"
        hashes = {"qmldir": checked_file(qmldir, qml_root)}
        metadata = qmldir.read_text()
        declared = re.findall(r"^\s*module\s+(\S+)\s*$", metadata, re.MULTILINE)
        if declared != [module["uri"]]:
            raise RuntimeError(f"QML module identity mismatch: expected {module['uri']} in {qmldir}")
        plugins = re.findall(r"^[ \t]*(?:optional[ \t]+)?plugin[ \t]+(\w+)(?:[ \t]+(\S+))?[ \t]*$",
                             metadata, re.MULTILINE)
        expected_plugins = module["plugins"][qt_version]
        declared_plugins = [plugin for plugin, _ in plugins]
        if sorted(declared_plugins) != sorted(expected_plugins):
            raise RuntimeError(f"QML plugin identity mismatch for {module['uri']}: "
                               f"expected {expected_plugins}, found {declared_plugins}")
        for plugin, relative in plugins:
            name = f"lib{plugin}.so"
            hashes[name] = checked_file(directory / relative / name, qml_root)
        for name in module.get("libraries", []):
            hashes[name] = checked_file(library_root / name, library_root)
        modules.append({"uri": module["uri"], "files": hashes})
    return {"schema_version": 1, "status": "passed", "qt_version": qt_version,
            "requirements_sha256": hashlib.sha256(MANIFEST.read_bytes()).hexdigest(),
            "modules": modules}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qml-root", type=Path, required=True)
    parser.add_argument("--library-root", type=Path, required=True)
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = validate_runtime(args.qml_root, args.library_root, args.qt_version)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Validated {len(report['modules'])} QML runtime modules for Qt {args.qt_version}")


if __name__ == "__main__":
    main()
