#!/usr/bin/env python3
"""Check the staged opt-in JPEG gain-map runtime with its matching Qt SDK."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


NOTICES = {
    "LICENSE": "c9247b5cb07866938643cc238292434e6f2af8642e1fce10771c0e4cc7909316",
    "LICENSE-image-io": "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30",
    "NOTICE-adobe-gain-map": "746d8c9c414b335bb7c9615918a15a6489a4dcc1c5c3aac2bda410d1072c332a",
}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def mapped(receipt, phase, stem):
    return {Path(path).resolve() for path in receipt[phase]["mapped_optional_libraries"]
            if Path(path).name.startswith(stem)}


def exercise(stage, name, fixture, capture, env):
    binary = stage / "bin/licasa_diagnostics"
    with (capture / f"{name}.json").open("w") as output, (capture / f"{name}.log").open("w") as log:
        subprocess.run([binary, "--gainmap-codec-check", fixture], cwd=capture, env=env,
                       stdout=output, stderr=log, timeout=20, check=True)
    result = json.loads((capture / f"{name}.json").read_text())
    require(result["sdr_decoded"], f"{name}: the base JPEG did not decode")
    require(not mapped(result, "before_decode", "libuhdr.so"), f"{name}: codec mapped at startup")
    jpeg = mapped(result, "after_decode", "libjpeg.so")
    require(len(jpeg) == 1, f"{name}: expected one shared JPEG runtime, observed {jpeg}")
    return result, next(iter(jpeg))


def check(args, result):
    stage = args.stage
    files = list(stage.glob("lib*/**/licasa/plugins/gainmap/libuhdr.so.2"))
    require(len(files) == 1 and files[0].is_file() and not files[0].is_symlink(),
            "Expected one private libuhdr.so.2 payload in the stage")
    codec = files[0]
    paths = [stage / "bin/licasa", stage / "bin/licasa_diagnostics", codec, args.jpeg, args.gainmap]
    for name, expected in NOTICES.items():
        notice = stage / "share/licenses/licasa/libultrahdr" / name
        require(digest(notice) == expected, f"Staged upstream notice mismatch: {name}")
        paths.append(notice)
    result["sha256"] = {str(path): digest(path) for path in paths}
    for binary in paths[:2]:
        elf = subprocess.check_output(["readelf", "-Wd", binary], text=True)
        (args.capture / f"{binary.name}.elf").write_text(elf)
        require("libuhdr" not in elf, f"{binary.name} has a startup dependency on libultrahdr")

    with tempfile.TemporaryDirectory(prefix="licasa-gainmap-package-") as scratch:
        scratch = Path(scratch)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="generic",
                   QT_QUICK_BACKEND="software")
        env.pop("LD_PRELOAD", None)
        if args.runtime_lib_dir:
            env["LD_LIBRARY_PATH"] = str(args.runtime_lib_dir)
        for part in ("CONFIG", "DATA", "CACHE", "STATE", "RUNTIME"):
            directory = scratch / part.lower()
            directory.mkdir(mode=0o700)
            env[f"XDG_{part}_DIR" if part == "RUNTIME" else f"XDG_{part}_HOME"] = str(directory)

        ordinary, jpeg = exercise(stage, "ordinary", args.jpeg, args.capture, env)
        require(not ordinary["hdr_decoded"] and not mapped(ordinary, "after_decode", "libuhdr.so"),
                "An ordinary JPEG mapped the gain-map codec")

        gainmap, gainmap_jpeg = exercise(stage, "gainmap", args.gainmap, args.capture, env)
        require(gainmap_jpeg == jpeg and mapped(gainmap, "after_decode", "libuhdr.so") == {codec},
                "The gain map must map only the stage codec and the existing JPEG runtime")
        expected = 4 * ((192 / 255 + 0.055) / 1.055) ** 2.4
        require(gainmap["hdr_decoded"] and gainmap["hdr_fp16"] and
                (gainmap["hdr_width"], gainmap["hdr_height"]) == (32, 32) and
                all(math.isclose(channel, expected, abs_tol=0.012) for channel in gainmap["hdr_center"][:3]) and
                gainmap["hdr_center"][3] == 1, "Unexpected FP16 pixels from the staged codec")

        incomplete = scratch / "stage"
        shutil.copytree(stage, incomplete, symlinks=True)
        replacement = incomplete / codec.relative_to(stage)
        replacement.unlink()
        # A discoverable copy on the general loader path must never substitute
        # for an absent app-relative payload.
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (str(codec.parent), env.get("LD_LIBRARY_PATH"))))
        for name in ("missing", "invalid"):
            fallback, fallback_jpeg = exercise(incomplete, name, args.gainmap, args.capture, env)
            require(fallback_jpeg == jpeg and not fallback["hdr_decoded"] and
                    fallback["hdr_error"] == "Private Ultra HDR codec is unavailable." and
                    not mapped(fallback, "after_decode", "libuhdr.so") and
                    (fallback["sdr_width"], fallback["sdr_height"]) ==
                    (gainmap["sdr_width"], gainmap["sdr_height"]),
                    f"{name}: the staged viewer must keep the SDR rendition")
            if name == "missing":
                # Use the full production QML path as an additional SDR control.
                with (args.capture / "missing-viewer.json").open("w") as output, \
                     (args.capture / "missing-viewer.log").open("w") as log:
                    subprocess.run([incomplete / "bin/licasa_diagnostics", "--viewer", "--cycles",
                                    "1", args.gainmap], cwd=args.capture, env=env, stdout=output,
                                   stderr=log, timeout=20, check=True)
                viewer = json.loads((args.capture / "missing-viewer.json").read_text())
                require(viewer["image_status_at_settle"] == 1 and viewer["assigned_image_at_settle"]
                        and len(viewer["memory_cycles"]) == 1,
                        "The staged viewer did not complete a fallback open and reopen")
                replacement.write_text("invalid ELF payload\n")
    result["jpeg_runtime"] = str(jpeg)
    result["qt"] = gainmap["qt"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", required=True, type=Path, help="CMake-staged prefix")
    parser.add_argument("--jpeg", required=True, type=Path, help="An ordinary JPEG control")
    parser.add_argument("--gainmap", required=True, type=Path, help="Licasa's jpeg-gainmap-dual.jpg fixture")
    parser.add_argument("--runtime-lib-dir", type=Path, help="Matching private Qt library directory")
    parser.add_argument("--capture", required=True, type=Path, help="New directory for the test receipts")
    args = parser.parse_args()
    for name in ("stage", "jpeg", "gainmap", "capture"):
        setattr(args, name, getattr(args, name).resolve())
    if args.runtime_lib_dir:
        args.runtime_lib_dir = args.runtime_lib_dir.resolve()
    args.capture.mkdir()
    result = {"status": "failed"}
    try:
        check(args, result)
        result["status"] = "passed"
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(error, file=sys.stderr)
    (args.capture / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
