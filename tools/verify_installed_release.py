#!/usr/bin/env python3
"""Smoke the installed release with samples shipped in this repository."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SAMPLES = {
    "jpeg": "formats/core-matrix/jpeg/micro-1x1.jpg",
    "png": "formats/core-matrix/png/micro-1x1.png",
    "avif": "formats/core-matrix/avif/medium-640x360.avif",
    "jxl": "modern/jxl/full-hd.jxl",
    "heic": "modern/heif/with-alpha-512x512.heic",
    "dng": "modern/raw/small.dng",
    "apng": "animated/apng/standard-transparent-320x240.apng",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    prefix = args.prefix.resolve(strict=True)
    diagnostic = prefix / "bin/licasa_diagnostics"
    if not diagnostic.is_file():
        parser.error(f"Missing installed diagnostics: {diagnostic}")
    paths = [ROOT / "tests/test-assets" / sample for sample in SAMPLES.values()]
    for path in paths:
        if not path.is_file():
            parser.error(f"Missing repository sample: {path}")

    with tempfile.TemporaryDirectory(prefix="licasa-installed-release-") as temporary:
        env = dict(os.environ)
        env.pop("LD_LIBRARY_PATH", None)
        env.pop("QT_PLUGIN_PATH", None)
        env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                   XDG_CONFIG_HOME=temporary, XDG_CACHE_HOME=temporary)
        completed = subprocess.run(
            [str(diagnostic), "--formats", "--megapixels", "25", *map(str, paths)],
            env=env, capture_output=True, text=True, check=True, timeout=60,
        )
    report = json.loads(completed.stdout)
    available = set(report["read_formats"])
    missing = set(SAMPLES) - available
    if missing:
        raise RuntimeError(f"Installed readers missing: {sorted(missing)}")

    if report["before_decode"]["mapped_optional_libraries"]:
        raise RuntimeError("Native codecs loaded before the first decode")
    quick = [Path(path) for path in report["before_decode"]["mapped_qt_libraries"]
             if "libQt6Quick.so." in path]
    if len(quick) != 1 or not quick[0].is_relative_to(prefix):
        raise RuntimeError(f"Installed Qt Quick backport was not loaded: {quick}")

    for expected_format, sample in zip(SAMPLES, report["samples"], strict=True):
        if not sample["decoded"] or sample["format"] != expected_format:
            raise RuntimeError(f"Installed {expected_format} decode failed: {sample}")
        if expected_format == "dng" and sample["raw_development"]:
            raise RuntimeError("RAW sample used full development instead of its preview")
        for library in sample["process"]["mapped_optional_libraries"]:
            if not Path(library).is_relative_to(prefix):
                raise RuntimeError(f"Codec library escaped the installation: {library}")

    desktop = (prefix / "share/applications/licasa.desktop").read_text()
    advertised = next(line.removeprefix("MimeType=").split(";")
                      for line in desktop.splitlines() if line.startswith("MimeType="))
    missing_mime = set(advertised) - {""} - set(report["read_mime_types"])
    if missing_mime:
        raise RuntimeError(f"Desktop file advertises missing readers: {sorted(missing_mime)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Installed release decoded {len(SAMPLES)} bundled samples: {args.output}")


if __name__ == "__main__":
    main()
