#!/usr/bin/env python3
"""Validate the support contract against build recipes and render its README table."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

from runtime_profiles import PROFILES

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "packaging/support-matrix.json"
BEGIN = "<!-- support-matrix:start -->"
END = "<!-- support-matrix:end -->"


def load_manifest(path: Path = MANIFEST) -> dict:
    manifest = json.loads(path.read_text())
    if manifest.get("schema_version") != 1:
        raise ValueError("Unsupported support manifest version")
    if manifest.get("architecture") != "x86_64" or manifest.get("libc") != "glibc":
        raise ValueError("Only the x86_64/glibc runtime is implemented")
    ids = [target["id"] for target in manifest["targets"]]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate qualification targets")
    for target in manifest["targets"]:
        if not re.fullmatch(r"[a-z0-9-]+", target["id"]):
            raise ValueError("Invalid target identifier")
        if target["status"] not in {"supported", "in-development", "unsupported"}:
            raise ValueError(f"Unknown support status for {target['id']}")
        if target.get("contract", "dialogs") not in {"dialogs", "accessibility", "native-dialogs"}:
            raise ValueError(f"Unknown qualification contract for {target['id']}")
        if target["artifact"] not in manifest["artifacts"] or target["profile"] not in manifest["profiles"]:
            raise ValueError(f"Undefined artifact or Qt profile for {target['id']}")
        if bool(target.get("qualifier")) == bool(target.get("desktop")):
            raise ValueError(f"Expected exactly one qualifier for {target['id']}")
    for artifact in manifest["artifacts"].values():
        if Path(artifact).is_absolute() or ".." in Path(artifact).parts or "{version}" not in artifact:
            raise ValueError("Invalid artifact path template")
    return manifest


def check_recipes(manifest: dict, root: Path = ROOT) -> None:
    for name, profile in manifest["profiles"].items():
        runtime = PROFILES.get(profile.get("runtime_profile", name))
        if (runtime is None or profile["qt"] != runtime["qt_version"]
                or profile.get("multimedia_lock") != runtime["ffmpeg_lock"]
                or profile.get("quick_lock") != runtime["quick_lock"]):
            raise ValueError(f"{name} Qt disagrees with its implemented runtime profile")
        recipe = (root / profile["build_recipe"]).read_text()
        if not re.search(rf"^FROM\s+{re.escape(profile['build_image'])}(?:\s|$)", recipe, re.MULTILINE):
            raise ValueError(f"{name} image disagrees with its actual build recipe")
        for lock_key in ("quick_lock", "multimedia_lock"):
            if lock_key in profile:
                lock = json.loads((root / profile[lock_key]).read_text())
                if lock["version"] != profile["qt"]:
                    raise ValueError(f"{name} Qt disagrees with {profile[lock_key]}")
        if "portable_lock" in profile:
            portable = json.loads((root / profile["portable_lock"]).read_text())
            quick = json.loads((root / profile["quick_lock"]).read_text())
            if (portable["qt_version"] != profile["qt"]
                    or portable["qt_modules"]["qtdeclarative"] != quick["sha256"]):
                raise ValueError(f"{name} Qt disagrees with its locked portable SDK")
    module_manifest = json.loads((root / "packaging/qt-runtime-modules.json").read_text())
    if set(module_manifest["qt_versions"]) != {profile["qt"] for profile in manifest["profiles"].values()}:
        raise ValueError("The Qt module inventory and native runtime profiles disagree")
    # Check implementation selectors rather than merely repeating a list of versions.
    package_source = (root / "tools/package_deb.py").read_text()
    for token in ("debian-13", "+deb13", "ubuntu-24.04"):
        if token not in package_source:
            raise ValueError(f"Native package implementation does not expose {token}")
    native_source = (root / "tools/build_debian_deb.py").read_text()
    if manifest["profiles"]["debian-6.8.2"]["qt"] not in native_source:
        raise ValueError("Debian build does not enforce the declared Qt version")
    for target in manifest["targets"]:
        if target.get("qualifier") and not (root / target["qualifier"][0]).is_file():
            raise ValueError(f"Missing qualifier for {target['id']}")
    if "base: core24" not in (root / "snap/snapcraft.yaml").read_text():
        raise ValueError("Snap base changed; review the Qt/libc support profile")


def readme_table(manifest: dict) -> str:
    rows = ["| Package / target | Architecture / libc baseline | Qt runtime | Candidate qualification |",
            "| --- | --- | --- | --- |"]
    for target in manifest["targets"]:
        if target["status"] == "unsupported":
            continue
        profile = manifest["profiles"][target["profile"]]
        status = "Required before stable" if target["status"] == "supported" else "In development; not a published support claim"
        rows.append(f"| {target['artifact']} / {target['os']} | x86-64 / glibc ≥ {profile['glibc_minimum']} | {profile['qt']} | {status} |")
    return "\n".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-readme", action="store_true")
    parser.add_argument("--matrix", action="store_true", help="Print candidate qualification target IDs as JSON")
    args = parser.parse_args()
    manifest = load_manifest()
    check_recipes(manifest)
    readme = ROOT / "README.md"
    content = readme.read_text()
    replacement = f"{BEGIN}\n{readme_table(manifest)}\n{END}"
    updated, count = re.subn(re.escape(BEGIN) + r".*?" + re.escape(END), replacement, content, flags=re.DOTALL)
    if count != 1:
        raise ValueError("README must contain one generated support table")
    if args.write_readme:
        readme.write_text(updated)
    elif content != updated:
        raise ValueError("README support guidance is stale; run tools/check_support_manifest.py --write-readme")
    if args.matrix:
        print(json.dumps([target["id"] for target in manifest["targets"] if target.get("qualifier")]))
    else:
        print("Support recipes and README agree")


if __name__ == "__main__":
    main()
