#!/usr/bin/env python3
"""Validate shipped Qt catalogs and optionally compare real lupdate extraction."""

import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
PLACEHOLDER = re.compile(r"%(?:L?\d+|n)")


def messages(path):
    root = ET.parse(path).getroot()
    if root.tag != "TS" or not root.get("language"):
        raise ValueError("Expected a Qt TS catalog with a target language")
    found = {}
    finished = 0
    for context in root.findall("context"):
        name = context.findtext("name")
        if not name:
            raise ValueError("Translation context has no name")
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is None:
                raise ValueError("Translation message has no translation element")
            if translation.get("type") in {"obsolete", "vanished"}:
                continue
            source = message.findtext("source", "")
            key = (name, source, message.findtext("comment", ""), message.get("numerus", ""))
            if key in found:
                raise ValueError(f"Duplicate translation message: {key}")
            found[key] = message
            if translation.get("type") == "unfinished":
                continue
            variants = translation.findall("numerusform") if message.get("numerus") == "yes" else [translation]
            if not variants:
                raise ValueError(f"Missing plural translations: {key}")
            if message.get("numerus") == "yes" and root.get("language").startswith("sv") and len(variants) != 2:
                raise ValueError(f"Swedish plurals require two forms: {key}")
            for variant in variants:
                text = "".join(variant.itertext())
                if not text.strip():
                    raise ValueError(f"Empty finished translation: {key}")
                if Counter(PLACEHOLDER.findall(text)) != Counter(PLACEHOLDER.findall(source)):
                    raise ValueError(f"Translation placeholders changed: {key}")
            finished += 1
    return found, finished


def check_sources(catalog, lupdate, root=ROOT):
    target_language = ET.parse(catalog).getroot().get("language", "")
    with tempfile.TemporaryDirectory(prefix="licasa-translations-") as temporary:
        extracted = Path(temporary) / catalog.name
        subprocess.run([str(lupdate), str(root / "src"), str(root / "qml"),
                        "-no-obsolete", "-locations", "none", "-target-language", target_language,
                        "-ts", str(extracted)], check=True, capture_output=True, text=True, timeout=60)
        expected, _ = messages(extracted)
    actual, _ = messages(catalog)
    if actual.keys() != expected.keys():
        raise ValueError(f"Catalog differs from current sources: {len(expected.keys() - actual.keys())} missing, "
                         f"{len(actual.keys() - expected.keys())} obsolete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "translations/licasa_sv.ts")
    parser.add_argument("--lupdate", type=Path)
    args = parser.parse_args()
    catalog, finished = messages(args.catalog)
    if args.lupdate:
        check_sources(args.catalog, args.lupdate)
    print(f"Validated {len(catalog)} catalog messages; {finished} finished translations")


if __name__ == "__main__":
    main()
