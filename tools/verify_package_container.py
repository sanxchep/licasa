#!/usr/bin/env python3
"""Build a clean Ubuntu runtime and run the installed-package decoder checks.

Only the package and named fixtures enter the container. The test itself runs
without network access, as the invoking user, with a read-only root filesystem.
Docker must be available; vendor fixtures must already be in the supplied cache.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

# AVIF_PACKAGE_AUTOMATION_V1
# APNG_STAGE3_PACKAGE_AUTOMATION_V1
# HEIF_SEQUENCE_STAGE3_PACKAGE_AUTOMATION_V1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--vendor-cache", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build-container-results")
    parser.add_argument("--jxl", action="store_true")
    parser.add_argument(
        "--jp2", action="store_true", help="run installed JP2 and raw J2K checks"
    )
    parser.add_argument("--raw", action="store_true")
    parser.add_argument(
        "--avif",
        action="store_true",
        help="run the installed AVIF package/corpus qualification in the clean runtime",
    )
    # AVIF_STAGE4_PACKAGE_ANIMATION_V1
    parser.add_argument(
        "--avif-animation",
        action="store_true",
        help="run the installed 4K animated-AVIF qualification in the clean runtime",
    )
    parser.add_argument(
        "--apng",
        action="store_true",
        help="run installed APNG animation and PNG-fallback qualification",
    )
    parser.add_argument(
        "--heif-sequence",
        type=Path,
        help="run installed HEIF image-sequence qualification with a pinned fixture",
    )
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)
    vendor = args.vendor_cache.resolve(strict=True)
    avif_corpus = None
    heif_sequence = (
        args.heif_sequence.resolve(strict=True) if args.heif_sequence else None
    )
    if args.avif_animation and not args.avif:
        parser.error("--avif-animation requires --avif")

    avif_animation_dir = None
    if args.avif:
        avif_corpus = (ROOT / "build/deps/avif-samples").resolve(strict=True)
        if not (avif_corpus / "sources.json").is_file():
            parser.error(
                "AVIF corpus is missing sources.json; run "
                "tools/fetch_avif_corpus.py first"
            )
        if args.avif_animation:
            avif_animation_dir = (
                ROOT / "build/deps/avif-animation-large"
            ).resolve(strict=True)
            if not (avif_animation_dir / "fixture.json").is_file():
                parser.error("AVIF animation fixture.json is missing")
            if not (avif_animation_dir / "avif-4k-8f-10fps.avif").is_file():
                parser.error("AVIF 4K animation fixture is missing")

    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tag = "licasa-runtime-verification:local"
    subprocess.run(["docker", "build", "--tag", tag, "--file",
                    str(ROOT / "packaging/verify-runtime.Dockerfile"), str(prefix)], check=True)
    mounts = [(ROOT / "tools/smoke_package.py", "/checks/tools/smoke_package.py", True),
              (ROOT / "tools/fetch_raw_vendor_samples.py",
               "/checks/tools/fetch_raw_vendor_samples.py", True),
              (ROOT / "tests/test-assets/modern/heif", "/checks/tests/test-assets/modern/heif", True),
              (vendor, "/vendor-samples", True), (output, "/reports", False)]
    if heif_sequence:
        mounts.append((heif_sequence, "/heif-sequence.heics", True))
    if args.jxl:
        mounts.append((ROOT / "tests/test-assets/modern/jxl", "/checks/tests/test-assets/modern/jxl", True))
    if args.jp2:
        mounts.append((
            ROOT / "tests/test-assets/formats/additional-probes",
            "/checks/tests/test-assets/formats/additional-probes",
            True,
        ))
    if args.raw:
        mounts.append((ROOT / "tests/test-assets/modern/raw", "/checks/tests/test-assets/modern/raw", True))
    if args.apng:
        mounts.extend([
            (
                ROOT / "tests/test-assets/animated/apng",
                "/checks/tests/test-assets/animated/apng",
                True,
            ),
            (
                ROOT / "tests/test-assets/formats/core-matrix/png",
                "/checks/tests/test-assets/formats/core-matrix/png",
                True,
            ),
        ])
    if args.avif:
        mounts.extend([
            (
                ROOT / "tools/fetch_avif_corpus.py",
                "/checks/tools/fetch_avif_corpus.py",
                True,
            ),
            (
                ROOT / "tests/test-assets/modern/avif",
                "/checks/tests/test-assets/modern/avif",
                True,
            ),
            (
                avif_corpus,
                "/checks/build/deps/avif-samples",
                True,
            ),
        ])
        if args.avif_animation:
            mounts.append(
                (
                    avif_animation_dir,
                    "/checks/build/deps/avif-animation-large",
                    True,
                )
            )

    command = ["docker", "run", "--rm", "--network=none", "--read-only",
               "--cap-drop=ALL", "--security-opt=no-new-privileges",
               "--user", f"{os.getuid()}:{os.getgid()}",
               "--tmpfs", "/tmp:rw,nosuid,nodev,size=64m"]
    for source, target, read_only in mounts:
        # Docker's structured mount syntax has no escaping for commas.
        if "," in str(source):
            parser.error("Container bind paths cannot contain commas")
        command.extend(["--mount", f"type=bind,source={source},target={target}" + (",readonly" if read_only else "")])
    command += [tag, "python3", "/checks/tools/smoke_package.py", "/opt/licasa",
                "--vendor-cache", "/vendor-samples", "--output", "/reports/package-smoke.json"]
    if args.jxl:
        command.append("--jxl")
    if args.jp2:
        command.append("--jp2")
    if args.raw:
        command.append("--raw")
    if args.avif:
        command.append("--avif")
    if args.avif_animation:
        command.append("--avif-animation")
    if args.apng:
        command.append("--apng")
    if heif_sequence:
        command.extend(["--heif-sequence", "/heif-sequence.heics"])

    subprocess.run(command, check=True)
    report_path = output / "package-smoke.json"
    report = json.loads(report_path.read_text())
    image = json.loads(subprocess.check_output(["docker", "image", "inspect", tag], text=True))[0]
    report["container"] = {"image_id": image["Id"], "os": "Ubuntu 24.04",
                            "network": "disabled during tests", "root_filesystem": "read-only"}
    report["qualification"] = "Clean Ubuntu runtime container; private installed codecs and Qt Quick; no developer libraries or system HEIF decoder packages."
    if args.avif:
        report["avif_clean_runtime"] = {
            "enabled": True,
            "corpus_mount": "read-only",
            "network": "disabled",
            "package_prefix": "/opt/licasa",
            "qualification": (
                "image/avif registration, lazy native loading, representative "
                "still/clap/progressive decode and package-local "
                "libavif/dav1d/libyuv linkage verified"
            ),
        }
        if args.avif_animation:
            report["avif_clean_runtime"]["animation"] = {
                "enabled": True,
                "fixture_mount": "read-only",
                "fixture": "3840x2160, 8 frames, 10 fps",
                "qualification": (
                    "installed animation probe verified frame count, timing, "
                    "0/middle/final decode, backward seek, native raster "
                    "accounting and package-local native libraries"
                ),
            }
    if args.jp2:
        report["jp2_clean_runtime"] = {
            "enabled": True,
            "fixtures_mount": "read-only",
            "network": "disabled",
            "package_prefix": "/opt/licasa",
            "qualification": "installed JP2 and raw J2K decoding verified",
        }
    if heif_sequence:
        report["heif_sequence_clean_runtime"] = {
            "enabled": True,
            "fixture_mount": "read-only",
            "network": "disabled",
            "root_filesystem": "read-only",
            "package_prefix": "/opt/licasa",
            "desktop_mime_advertised": True,
            "qualification": (
                "installed HEIF sequence reader discovered the streaming frame "
                "count at EOS, decoded frame 0/middle/final, sought backward, "
                "preserved native-raster accounting and used only package-local "
                "libheif 1.23.4/libde265 1.1.2"
            ),
        }
    if args.apng:
        report["apng_clean_runtime"] = {
            "enabled": True,
            "fixture_mount": "read-only",
            "network": "disabled",
            "package_prefix": "/opt/licasa",
            "desktop_mime_advertised": True,
            "qualification": (
                "installed image/apng registration, representative APNG decode, "
                "animation frame seeking, static PNG fallthrough, desktop MIME "
                "advertisement and package-local APNG backend verified in a clean "
                "Ubuntu 24.04 runtime"
            ),
        }
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Clean-runtime evidence: {report_path}")


if __name__ == "__main__":
    main()
