#!/usr/bin/env python3
"""Verify installed image codecs, bounds, lazy loading, and package-local libraries."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
from urllib.request import urlopen

from fetch_raw_vendor_samples import fetch_samples as fetch_raw_samples

ROOT = Path(__file__).resolve().parents[1]

# AVIF_PACKAGE_AUTOMATION_V1
# APNG_STAGE3_PACKAGE_AUTOMATION_V1
# HEIF_SEQUENCE_STAGE3_PACKAGE_AUTOMATION_V1
# AVIF package qualification deliberately reuses the already-pinned public
# corpus. The source cache is copied to temporary storage before the offline
# verifier runs because fetch_avif_corpus.py republishes sources.json.
VENDOR_SAMPLES = {
    "iphone15pro.heic": (
        "https://dl.photoprism.app/samples/Brands/Apple/Apple%20iPhone%2015%20Pro/iphone_15_pro.heic",
        "c68e438f92341da271a731a059113db9395366f4c2f78ead5ded64deba7de463"),
    "xiaomi-mi10-ultra.heic": (
        "https://dl.photoprism.app/samples/Brands/Xiaomi/Xiaomi%20Mi%2010%20Ultra/IMG_20211010_145903.HEIC",
        "37e9ce9a3c6d9083322566a869e390d45df3d7aed991d77ddfe880d822326e37"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--vendor-cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jxl", action="store_true")
    parser.add_argument(
        "--jp2", action="store_true", help="qualify the installed JPEG 2000 reader"
    )
    parser.add_argument("--raw", action="store_true")
    parser.add_argument(
        "--avif",
        action="store_true",
        help="qualify the installed AVIF reader with the pinned public corpus",
    )
    # AVIF_STAGE4_PACKAGE_ANIMATION_V1
    parser.add_argument(
        "--avif-animation",
        action="store_true",
        help="qualify installed animated-AVIF frame count, timing and seeking",
    )
    parser.add_argument(
        "--apng",
        action="store_true",
        help="qualify the installed APNG reader, animation contract and PNG fallback",
    )
    parser.add_argument(
        "--heif-sequence",
        type=Path,
        help="qualify an installed HEIF image-sequence reader with a pinned external fixture",
    )
    args = parser.parse_args()
    # A failed rerun must not leave an earlier successful qualification receipt.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("{}\n")
    prefix = args.prefix.resolve(strict=True)
    cache = args.vendor_cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    heif_sequence_path = (
        args.heif_sequence.resolve(strict=True) if args.heif_sequence else None
    )
    sources = {}
    for name, (url, digest) in VENDOR_SAMPLES.items():
        path = cache / name
        if not path.exists():
            with urlopen(url, timeout=30) as response:
                data = response.read(32 * 1024 * 1024 + 1)
            if len(data) > 32 * 1024 * 1024 or hashlib.sha256(data).hexdigest() != digest:
                raise RuntimeError(f"Invalid sample download: {name}")
            path.write_bytes(data)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f"Sample checksum mismatch: {path}")
        sources[name] = {"url": url, "sha256": digest, "provenance": "PhotoPrism public camera-test samples"}
    samples = list((ROOT / "tests/test-assets/modern/heif").rglob("*.heic")) + [cache / n for n in VENDOR_SAMPLES]
    if args.jxl:
        samples += list((ROOT / "tests/test-assets/modern/jxl").rglob("*.jxl"))
    if args.jp2:
        samples += [
            ROOT / "tests/test-assets/formats/additional-probes/jpeg-2000.jp2",
            ROOT / "tests/test-assets/formats/additional-probes/jpeg-2000-codestream.j2k",
        ]
    if args.raw:
        raw_paths, raw_sources = fetch_raw_samples(cache / "raw", check_only=True, verbose=False)
        for source, path in zip(raw_sources, raw_paths, strict=True):
            sources[path.name] = source
            samples.append(path)
        samples += list((ROOT / "tests/test-assets/modern/raw").glob("*.dng"))

    apng_sample_paths = []
    apng_animation_path = None
    apng_static_png = None
    if args.apng:
        apng_dir = ROOT / "tests/test-assets/animated/apng"
        apng_sample_paths = sorted(apng_dir.glob("*.apng"))
        if not apng_sample_paths:
            raise RuntimeError(
                f"APNG package qualification has no fixtures under {apng_dir}"
            )
        apng_animation_path = apng_dir / "hd-transparent-1280x720.apng"
        if not apng_animation_path.is_file():
            raise RuntimeError(
                f"APNG package qualification requires {apng_animation_path}"
            )
        apng_static_png = (
            ROOT / "tests/test-assets/formats/core-matrix/png/full-hd-1920x1080.png"
        )
        if not apng_static_png.is_file():
            raise RuntimeError(
                f"APNG package qualification requires static PNG fixture {apng_static_png}"
            )
        samples += apng_sample_paths

    avif_corpus = ROOT / "build/deps/avif-samples"
    avif_sample_paths = []
    avif_reference_png = None
    avif_animation_path = None
    avif_animation_fixture = None
    if args.avif_animation and not args.avif:
        parser.error("--avif-animation requires --avif")
    if args.avif:
        avif_sample_paths = [
            avif_corpus / "link-u-avif-sample-images/kimono.avif",
            avif_corpus / "link-u-avif-sample-images/kimono.crop.avif",
            avif_corpus / "libavif-v1.4.2/tests/data/draw_points_idat_progressive.avif",
        ]
        avif_reference_png = (
            avif_corpus / "link-u-avif-sample-images/kimono.png"
        )

        required = [
            avif_corpus / "sources.json",
            ROOT / "tools/fetch_avif_corpus.py",
            ROOT / "tests/test-assets/modern/avif/external-sources.json",
            avif_reference_png,
            *avif_sample_paths,
        ]
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise RuntimeError(
                "AVIF package qualification requires the verified public corpus; "
                "missing: " + ", ".join(missing)
            )

        samples += avif_sample_paths

        if args.avif_animation:
            animation_dir = ROOT / "build/deps/avif-animation-large"
            avif_animation_path = animation_dir / "avif-4k-8f-10fps.avif"
            fixture_path = animation_dir / "fixture.json"
            if not avif_animation_path.is_file() or not fixture_path.is_file():
                raise RuntimeError(
                    "AVIF animation qualification requires the pinned 4K fixture "
                    "and fixture.json under build/deps/avif-animation-large"
                )
            avif_animation_fixture = json.loads(
                fixture_path.read_text(encoding="utf-8")
            )
            digest = hashlib.sha256(avif_animation_path.read_bytes()).hexdigest()
            if digest != avif_animation_fixture["sha256"]:
                raise RuntimeError(
                    f"Animated AVIF fixture checksum mismatch: {digest}"
                )
            if not (avif_animation_fixture["width"] == 3840):
                raise AssertionError
            if not (avif_animation_fixture["height"] == 2160):
                raise AssertionError
            if not (avif_animation_fixture["frames"] == 8):
                raise AssertionError

    avif_provenance = None
    avif_verifier_output = ""
    avif_lazy_png = None
    avif_animation_probe = None

    apng_animation_probe = None
    apng_static_png_probe = None
    apng_png_extension_probe = None

    heif_sequence_probe = None

    with tempfile.TemporaryDirectory(prefix="licasa-package-smoke-") as temporary:
        temporary_path = Path(temporary)

        if args.avif:
            # Preserve the qualification input as read-only in the container.
            # fetch_avif_corpus.py writes deterministic sources.json even in
            # --offline mode, so verify an exact temporary copy instead.
            verified_avif = temporary_path / "verified-avif-corpus"
            shutil.copytree(avif_corpus, verified_avif, symlinks=True)

            verified = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/fetch_avif_corpus.py"),
                    "--manifest",
                    str(ROOT / "tests/test-assets/modern/avif/external-sources.json"),
                    "--output",
                    str(verified_avif),
                    "--offline",
                ],
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            avif_verifier_output = verified.stdout
            avif_provenance = json.loads(
                (verified_avif / "sources.json").read_text(encoding="utf-8")
            )

        env = dict(os.environ)
        env.pop("QT_PLUGIN_PATH", None)
        env.pop("LD_LIBRARY_PATH", None)
        env.update(QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=temporary,
                   QT_IMAGEIO_MAXALLOC="0", LIBHEIF_SECURITY_LIMITS="off")

        if heif_sequence_path:
            probe_binary = prefix / "bin/licasa_animation_probe"
            if not probe_binary.is_file():
                raise RuntimeError(
                    f"installed animation probe is missing: {probe_binary}"
                )

            probe_completed = subprocess.run(
                [str(probe_binary), str(heif_sequence_path), "heics"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=120,
            )
            probe_text = (
                probe_completed.stdout.strip()
                or probe_completed.stderr.strip()
            )
            heif_sequence_probe = json.loads(probe_text.splitlines()[-1])

            if not (heif_sequence_probe["ok"]):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["forced_format"] == "heics"):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["format"] == "heics"):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["animation"] is True):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["frames_initial"] == 0):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["streaming_count_discovery"] is True):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["frames"] == 120):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["loop_count"] == 0):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["declared_width"] == 256):
                raise AssertionError(heif_sequence_probe)
            if not (heif_sequence_probe["declared_height"] == 144):
                raise AssertionError(heif_sequence_probe)
            if not ("heics" in heif_sequence_probe["read_formats"]):
                raise AssertionError(heif_sequence_probe)
            if not ("heifs" in heif_sequence_probe["read_formats"]):
                raise AssertionError(heif_sequence_probe)
            if not ("image/heic-sequence" in heif_sequence_probe["read_mime_types"]):
                raise AssertionError(heif_sequence_probe)
            if not ("image/heif-sequence" in heif_sequence_probe["read_mime_types"]):
                raise AssertionError(heif_sequence_probe)
            if not ("libheif 1.23.4" in heif_sequence_probe["decoder_backend"]):
                raise AssertionError(heif_sequence_probe)
            if not ("libde265" in heif_sequence_probe["decoder_backend"]):
                raise AssertionError(heif_sequence_probe)
            if not ("heif image sequence" in heif_sequence_probe["preview_path"].lower()):
                raise AssertionError(heif_sequence_probe)

            checks = heif_sequence_probe["frame_checks"]
            if not ([row["requested"] for row in checks] == [0, 60, 119]):
                raise AssertionError(checks)
            for row in checks:
                if not (row["jumped"] and row["decoded"]):
                    raise AssertionError(row)
                if not (row["current"] == row["requested"]):
                    raise AssertionError(row)
                if not (row["width"] == 256 and row["height"] == 144):
                    raise AssertionError(row)
                if not (10 <= row["delay_ms"] <= 60000):
                    raise AssertionError(row)

            back = heif_sequence_probe["seek_back_to_zero"]
            if not (back["jumped"] and back["decoded"] and back["current"] == 0):
                raise AssertionError(back)
            if not (int(heif_sequence_probe["native_raster_pixels"]) == 256 * 144):
                raise AssertionError(heif_sequence_probe)

            mapped = [Path(item) for item in heif_sequence_probe["mapped_codec_libraries"]]
            if not (any(item.name == "liblicasa_heif.so" for item in mapped)):
                raise AssertionError(mapped)
            if not (any(item.name == "libheif.so.1.23.4" for item in mapped)):
                raise AssertionError(mapped)
            if not (any(item.name.startswith("libde265.so.") for item in mapped)):
                raise AssertionError(mapped)
            for library in mapped:
                if not (library.resolve(strict=True).is_relative_to(prefix)):
                    raise AssertionError(f"HEIF sequence codec escaped installed package: {library}")

        if args.avif:
            # Isolated PNG decode proves discovering image/avif does not eagerly
            # load libavif/dav1d/libyuv.
            lazy_completed = subprocess.run(
                [
                    str(prefix / "bin/licasa_diagnostics"),
                    "--formats",
                    "--megapixels",
                    "25",
                    str(avif_reference_png),
                ],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            avif_lazy_png = json.loads(lazy_completed.stdout)

            if args.avif_animation:
                probe_binary = prefix / "bin/licasa_animation_probe"
                if not probe_binary.is_file():
                    raise RuntimeError(
                        f"installed animation probe is missing: {probe_binary}"
                    )
                probe_completed = subprocess.run(
                    [str(probe_binary), str(avif_animation_path)],
                    env=env,
                    capture_output=True,
                    text=True,
                    check=True,
                    timeout=90,
                )
                probe_text = probe_completed.stdout.strip() or probe_completed.stderr.strip()
                avif_animation_probe = json.loads(probe_text.splitlines()[-1])

                if not (avif_animation_probe["ok"]):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["format"] == "avif"):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["animation"] is True):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["frames"] == avif_animation_fixture["frames"]):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["declared_width"] == avif_animation_fixture["width"]):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["declared_height"] == avif_animation_fixture["height"]):
                    raise AssertionError(avif_animation_probe)
                if not ("avif" in avif_animation_probe["read_formats"]):
                    raise AssertionError(avif_animation_probe)
                if not ("image/avif" in avif_animation_probe["read_mime_types"]):
                    raise AssertionError(avif_animation_probe)
                if not (avif_animation_probe["can_read_after_last"] is False):
                    raise AssertionError(avif_animation_probe)

                checks = avif_animation_probe["frame_checks"]
                if not ([row["requested"] for row in checks] == [0, 4, 7]):
                    raise AssertionError(checks)
                for row in checks:
                    if not (row["jumped"] and row["decoded"]):
                        raise AssertionError(row)
                    if not (row["current"] == row["requested"]):
                        raise AssertionError(row)
                    if not (row["width"] == avif_animation_fixture["width"]):
                        raise AssertionError(row)
                    if not (row["height"] == avif_animation_fixture["height"]):
                        raise AssertionError(row)
                    if not (10 <= row["delay_ms"] <= 60000):
                        raise AssertionError(row)

                back = avif_animation_probe["seek_back_to_zero"]
                if not (back["jumped"] and back["decoded"] and back["current"] == 0):
                    raise AssertionError(back)

                native_pixels = int(avif_animation_probe["native_raster_pixels"])
                if not (native_pixels == (
                    avif_animation_fixture["width"] *
                    avif_animation_fixture["height"]
                )):
                    raise AssertionError(avif_animation_probe)

                mapped = [
                    Path(item)
                    for item in avif_animation_probe["mapped_codec_libraries"]
                ]
                if not (any("libavif" in item.name for item in mapped)):
                    raise AssertionError(mapped)
                if not (any("libdav1d" in item.name for item in mapped)):
                    raise AssertionError(mapped)
                if not (any("libyuv" in item.name for item in mapped)):
                    raise AssertionError(mapped)
                for library in mapped:
                    if not (library.resolve(strict=True).is_relative_to(prefix)):
                        raise AssertionError(f"animated AVIF escaped installed package: {library}")

            lazy_sample = avif_lazy_png["samples"][0]
            if not (lazy_sample["decoded"] and lazy_sample["format"] == "png"):
                raise AssertionError(lazy_sample)

            eager_avif_libraries = [
                library
                for library in lazy_sample["process"]["mapped_optional_libraries"]
                if any(
                    token in Path(library).name
                    for token in ("libavif", "libdav1d", "libyuv")
                )
            ]
            if not (not eager_avif_libraries):
                raise AssertionError("ordinary PNG eagerly loaded AVIF native libraries: "
                f"{eager_avif_libraries}")

        if args.apng:
            probe_binary = prefix / "bin/licasa_animation_probe"
            if not probe_binary.is_file():
                raise RuntimeError(
                    f"installed animation probe is missing: {probe_binary}"
                )

            probe_completed = subprocess.run(
                [str(probe_binary), str(apng_animation_path), "apng"],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            probe_text = (
                probe_completed.stdout.strip()
                or probe_completed.stderr.strip()
            )
            apng_animation_probe = json.loads(probe_text.splitlines()[-1])

            if not (apng_animation_probe["ok"]):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["forced_format"] == "apng"):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["format"] == "apng"):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["animation"] is True):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["frames"] >= 2):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["declared_width"] == 1280):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["declared_height"] == 720):
                raise AssertionError(apng_animation_probe)
            if not ("apng" in apng_animation_probe["read_formats"]):
                raise AssertionError(apng_animation_probe)
            if not ("image/apng" in apng_animation_probe["read_mime_types"]):
                raise AssertionError(apng_animation_probe)
            if not (apng_animation_probe["can_read_after_last"] is False):
                raise AssertionError(apng_animation_probe)

            checks = apng_animation_probe["frame_checks"]
            expected = [
                0,
                apng_animation_probe["frames"] // 2,
                apng_animation_probe["frames"] - 1,
            ]
            if not ([row["requested"] for row in checks] == expected):
                raise AssertionError(checks)
            for row in checks:
                if not (row["jumped"] and row["decoded"]):
                    raise AssertionError(row)
                if not (row["current"] == row["requested"]):
                    raise AssertionError(row)
                if not (0 < row["width"] <= 1280):
                    raise AssertionError(row)
                if not (0 < row["height"] <= 720):
                    raise AssertionError(row)
                if not (10 <= row["delay_ms"] <= 60000):
                    raise AssertionError(row)

            back = apng_animation_probe["seek_back_to_zero"]
            if not (back["jumped"] and back["decoded"] and back["current"] == 0):
                raise AssertionError(back)

            native_pixels = int(apng_animation_probe["native_raster_pixels"])
            if not (0 < native_pixels <= 1280 * 720):
                raise AssertionError(apng_animation_probe)

            mapped = [
                Path(item)
                for item in apng_animation_probe["mapped_codec_libraries"]
            ]
            apng_backends = [
                item for item in mapped if "licasa_apng" in item.name
            ]
            if not (apng_backends):
                raise AssertionError(mapped)
            for library in apng_backends:
                if not (library.resolve(strict=True).is_relative_to(prefix)):
                    raise AssertionError(f"APNG backend escaped installed package: {library}")

            static_completed = subprocess.run(
                [
                    str(prefix / "bin/licasa_diagnostics"),
                    "--formats",
                    "--megapixels",
                    "25",
                    str(apng_static_png),
                ],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            apng_static_png_probe = json.loads(static_completed.stdout)
            static_sample = apng_static_png_probe["samples"][0]
            if not (static_sample["decoded"]):
                raise AssertionError(static_sample)
            if not (static_sample["format"] == "png"):
                raise AssertionError(static_sample)
            if not (static_sample["animation"] is False):
                raise AssertionError(static_sample)

            apng_as_png = temporary_path / "apng-as-png.png"
            shutil.copyfile(apng_animation_path, apng_as_png)
            apng_png_completed = subprocess.run(
                [
                    str(prefix / "bin/licasa_diagnostics"),
                    "--formats",
                    "--megapixels",
                    "25",
                    str(apng_as_png),
                ],
                env=env,
                capture_output=True,
                text=True,
                check=True,
                timeout=30,
            )
            apng_png_extension_probe = json.loads(apng_png_completed.stdout)
            apng_png_sample = apng_png_extension_probe["samples"][0]
            if not (apng_png_sample["decoded"]):
                raise AssertionError(apng_png_sample)
            if not (apng_png_sample["format"] == "apng"):
                raise AssertionError(apng_png_sample)
            if not (apng_png_sample["animation"] is True):
                raise AssertionError(apng_png_sample)
            if not (apng_png_sample["frames"] >= 2):
                raise AssertionError(apng_png_sample)

        completed = subprocess.run([str(prefix / "bin/licasa_diagnostics"), "--formats", "--megapixels", "25",
                                    *map(str, sorted(samples))], env=env, capture_output=True, text=True,
                                   check=True, timeout=60)
        report = json.loads(completed.stdout)
        admitted = subprocess.run([str(prefix / "bin/licasa_diagnostics"), "--formats", "--megapixels", "100",
            str(ROOT / "tests/test-assets/modern/heif/single-48mp.heic")],
            env=env, capture_output=True, text=True, check=True, timeout=60)
        admitted = json.loads(admitted.stdout)
        admitted_jxl = None
        if args.jxl:
            admitted_jxl = subprocess.run([str(prefix / "bin/licasa_diagnostics"), "--formats", "--megapixels", "100",
                str(ROOT / "tests/test-assets/modern/jxl/large-48mp.jxl")],
                env=env, capture_output=True, text=True, check=True, timeout=60)
            admitted_jxl = json.loads(admitted_jxl.stdout)
        viewer_env = dict(env, QT_QUICK_BACKEND="software", QSG_RENDER_LOOP="basic")
        rejected_viewer = subprocess.run([str(prefix / "bin/licasa_diagnostics"), "--viewer", "--megapixels", "25",
            str(ROOT / "tests/test-assets/modern/heif/single-48mp.heic")],
            env=viewer_env, capture_output=True, text=True, check=True, timeout=40)
        rejected_viewer = json.loads(rejected_viewer.stdout)
    if not (not report["before_decode"]["mapped_optional_libraries"]):
        raise AssertionError("codec library eagerly loaded")
    if not (report["qt_allocation_mib"] == 191):
        raise AssertionError
    desktop = (prefix / "share/applications/licasa.desktop").read_text()
    advertised = next(line.removeprefix("MimeType=").split(";") for line in desktop.splitlines() if line.startswith("MimeType="))
    if not (set(advertised) - {""} <= set(report["read_mime_types"])):
        raise AssertionError("desktop advertises a missing reader")

    if args.avif:
        if not ("avif" in report["read_formats"]):
            raise AssertionError("installed package does not expose AVIF")
        if not ("image/avif" in report["read_mime_types"]):
            raise AssertionError("Qt AVIF plugin does not advertise image/avif")
        if not ("image/avif" in advertised):
            raise AssertionError("installed desktop file does not advertise image/avif")

    if args.jp2:
        if not ("jp2" in report["read_formats"]):
            raise AssertionError("installed package does not expose JP2")
        jp2_plugin = prefix / "lib/licasa/plugins/imageformats/liblicasa_jp2_plugin.so"
        if not (jp2_plugin.is_file()):
            raise AssertionError(jp2_plugin)

    if args.apng:
        if not ("apng" in report["read_formats"]):
            raise AssertionError("installed package does not expose APNG")
        if not ("image/apng" in report["read_mime_types"]):
            raise AssertionError("Qt APNG plugin does not advertise image/apng")
        if not ("image/apng" in advertised):
            raise AssertionError("installed desktop file does not advertise qualified image/apng support")

    if heif_sequence_path:
        if not ("heics" in report["read_formats"]):
            raise AssertionError("installed package does not expose HEIC sequences")
        if not ("heifs" in report["read_formats"]):
            raise AssertionError("installed package does not expose HEIF sequences")
        if not ("image/heic-sequence" in report["read_mime_types"]):
            raise AssertionError("Qt HEIF plugin does not advertise image/heic-sequence")
        if not ("image/heif-sequence" in report["read_mime_types"]):
            raise AssertionError("Qt HEIF plugin does not advertise image/heif-sequence")
        if not ("image/heic-sequence" in advertised):
            raise AssertionError("desktop file does not advertise qualified image/heic-sequence support")
        if not ("image/heif-sequence" in advertised):
            raise AssertionError("desktop file does not advertise qualified image/heif-sequence support")

    quick = [path for path in report["before_decode"]["mapped_qt_libraries"] if "libQt6Quick.so." in path]
    if not (len(quick) == 1 and Path(quick[0]).is_relative_to(prefix)):
        raise AssertionError("package is missing the Qt Quick cleanup fix")
    for sample in report["samples"]:
        if Path(sample["path"]).suffix.lower() == ".dng":
            if not (not sample["raw_development"]):
                raise AssertionError(sample)
            if Path(sample["path"]).name in ("broken-preview-offset.dng", "oversized-preview.dng", "truncated.dng", "no-preview.dng"):
                if not (not sample["decoded"] and sample["largest_native_raster_pixels"] == 0):
                    raise AssertionError(sample)
                continue
        if Path(sample["path"]).parent.name == "broken":
            if not (not sample["decoded"] and sample["largest_native_raster_pixels"] == 0):
                raise AssertionError(sample)
            continue
        if Path(sample["path"]).name in ("single-48mp.heic", "large-48mp.jxl"):
            if not (not sample["decoded"] and "limit" in sample["codec_error"]):
                raise AssertionError(sample)
            if not (sample["largest_native_raster_pixels"] == 0):
                raise AssertionError(sample)
            continue
        if not (sample["decoded"]):
            raise AssertionError(sample)

        if Path(sample["path"]).suffix.lower() == ".apng":
            if not (sample["format"] == "apng"):
                raise AssertionError(sample)
            if not (sample["animation"] is True):
                raise AssertionError(sample)
            if not (sample["frames"] >= 2):
                raise AssertionError(sample)
            declared_pixels = sample["declared_width"] * sample["declared_height"]
            if not (declared_pixels > 0):
                raise AssertionError(sample)
            if not (0 < sample["largest_native_raster_pixels"] <= declared_pixels):
                raise AssertionError(sample)
            if not (sample["native_scaled_option"] is False):
                raise AssertionError(sample)
            if not ("full admitted apng canvas" in sample["preview_path"].lower()):
                raise AssertionError(sample)

        if Path(sample["path"]).suffix.lower() == ".avif":
            if not (sample["format"] == "avif"):
                raise AssertionError(sample)
            if not ("libavif 1.4.2" in sample["decoder_backend"]):
                raise AssertionError(sample)
            if not ("dav1d" in sample["decoder_backend"]):
                raise AssertionError(sample)
            if not ("libyuv 1924" in sample["decoder_backend"]):
                raise AssertionError(sample)

            declared_pixels = (
                sample["declared_width"] * sample["declared_height"]
            )
            if not (declared_pixels > 0):
                raise AssertionError(sample)
            if not (sample["largest_native_raster_pixels"] >= declared_pixels):
                raise AssertionError(sample)
            if not ("full native raster required" in sample["preview_path"].lower()):
                raise AssertionError(sample)

        if args.jp2 and Path(sample["path"]).suffix.lower() in (".jp2", ".j2k"):
            if not (sample["format"] == "jp2"):
                raise AssertionError(sample)
            if not ((sample["declared_width"], sample["declared_height"]) == (640, 360)):
                raise AssertionError(sample)

        if not (0 < sample["decoded_width"] <= 1200 and 0 < sample["decoded_height"] <= 900):
            raise AssertionError(sample)
        if not (sample["largest_native_raster_pixels"] <= 25_000_000):
            raise AssertionError(sample)
        if not (sample["process"]["storage_write_bytes"] == report["before_decode"]["storage_write_bytes"]):
            raise AssertionError("format decode wrote to storage")
        for library in sample["process"]["mapped_optional_libraries"]:
            if not (Path(library).is_relative_to(prefix)):
                raise AssertionError(f"developer/system codec dependency: {library}")

    if args.avif:
        avif_rows = [
            sample
            for sample in report["samples"]
            if Path(sample["path"]).suffix.lower() == ".avif"
        ]
        if not (len(avif_rows) == len(avif_sample_paths)):
            raise AssertionError(avif_rows)

        mapped_avif = [
            library
            for sample in avif_rows
            for library in sample["process"]["mapped_optional_libraries"]
            if "libavif" in Path(library).name
        ]
        if not (mapped_avif):
            raise AssertionError("AVIF decode did not map libavif")
        for library in mapped_avif:
            if not (Path(library).is_relative_to(prefix)):
                raise AssertionError(library)

        backend = (
            prefix
            / "lib/licasa/plugins/licasa-codecs/liblicasa_avif.so"
        )
        adapter = (
            prefix
            / "lib/licasa/plugins/imageformats/liblicasa_avif_plugin.so"
        )
        if not (backend.is_file()):
            raise AssertionError(backend)
        if not (adapter.is_file()):
            raise AssertionError(adapter)

        backend_ldd = subprocess.check_output(
            ["ldd", str(backend)],
            text=True,
        )
        adapter_ldd = subprocess.check_output(
            ["ldd", str(adapter)],
            text=True,
        )

        forbidden_adapter = [
            token
            for token in ("libavif", "libdav1d", "libyuv")
            if token in adapter_ldd
        ]
        if not (not forbidden_adapter):
            raise AssertionError("Qt AVIF adapter eagerly links native AVIF libraries: "
            f"{forbidden_adapter}")

        def resolved_dependency(linkage, soname):
            prefix_text = soname + " => "
            for line in linkage.splitlines():
                stripped = line.strip()
                if stripped.startswith(prefix_text):
                    value = stripped.split("=>", 1)[1].split("(", 1)[0].strip()
                    if not value or value == "not found":
                        raise AssertionError(f"{soname} did not resolve: {line}")
                    path = Path(value).resolve(strict=True)
                    if not (path.is_relative_to(prefix)):
                        raise AssertionError(f"{soname} escaped installed package: {path}")
                    return path
            raise AssertionError(f"{soname} missing from backend linkage")

        avif_linkage = {
            soname: str(resolved_dependency(backend_ldd, soname))
            for soname in ("libavif.so.16", "libdav1d.so.7", "libyuv.so")
        }

        report["avif_package_qualification"] = {
            "corpus_provenance": avif_provenance,
            "corpus_offline_verifier_stdout": avif_verifier_output,
            "isolated_png_lazy_probe": avif_lazy_png,
            "backend_linkage": avif_linkage,
            "adapter_native_dependencies": forbidden_adapter,
            "representative_samples": [
                str(path.relative_to(avif_corpus))
                for path in avif_sample_paths
            ],
        }
        if args.avif_animation:
            report["avif_package_qualification"]["animation_fixture"] = (
                avif_animation_fixture
            )
            report["avif_package_qualification"]["animation_probe"] = (
                avif_animation_probe
            )

    if heif_sequence_path:
        backend = prefix / "lib/licasa/plugins/licasa-codecs/liblicasa_heif.so"
        adapter = prefix / "lib/licasa/plugins/imageformats/liblicasa_heif_plugin.so"
        if not (backend.is_file()):
            raise AssertionError(backend)
        if not (adapter.is_file()):
            raise AssertionError(adapter)

        backend_ldd = subprocess.check_output(["ldd", str(backend)], text=True)
        adapter_ldd = subprocess.check_output(["ldd", str(adapter)], text=True)
        if not ("libheif" not in adapter_ldd and "libde265" not in adapter_ldd):
            raise AssertionError("Qt HEIF adapter must lazy-load the native HEIF backend")

        report["heif_sequence_package_qualification"] = {
            "fixture": str(heif_sequence_path),
            "animation_probe": heif_sequence_probe,
            "backend_ldd": backend_ldd,
            "adapter_ldd": adapter_ldd,
            "desktop_mime_advertised": True,
            "libheif_version": "1.23.4",
            "libde265_version": "1.1.2",
        }

    if args.apng:
        backend = prefix / "lib/licasa/plugins/licasa-codecs/liblicasa_apng.so"
        adapter = prefix / "lib/licasa/plugins/imageformats/liblicasa_apng_plugin.so"
        if not (backend.is_file()):
            raise AssertionError(backend)
        if not (adapter.is_file()):
            raise AssertionError(adapter)

        backend_ldd = subprocess.check_output(["ldd", str(backend)], text=True)
        adapter_ldd = subprocess.check_output(["ldd", str(adapter)], text=True)
        if not ("licasa_apng" not in adapter_ldd):
            raise AssertionError("Qt APNG adapter must lazy-load the APNG backend")

        report["apng_package_qualification"] = {
            "fixtures": [str(path.relative_to(ROOT)) for path in apng_sample_paths],
            "animation_probe": apng_animation_probe,
            "static_png_fallback_probe": apng_static_png_probe,
            "apng_stored_as_png_probe": apng_png_extension_probe,
            "backend_ldd": backend_ldd,
            "adapter_ldd": adapter_ldd,
            "new_native_dependencies": [],
            "desktop_mime_advertised": True,
        }

    report["vendor_sample_sources"] = sources
    automatic = admitted["samples"][0]
    if not (admitted["qt_allocation_mib"] == 763):
        raise AssertionError
    if not (automatic["decoded"] and automatic["largest_native_raster_pixels"] == 48_000_000):
        raise AssertionError(automatic)
    if not (automatic["decoded_width"] == 1200 and automatic["decoded_height"] == 900):
        raise AssertionError(automatic)
    if not (automatic["process"]["storage_write_bytes"] == admitted["before_decode"]["storage_write_bytes"]):
        raise AssertionError
    for library in automatic["process"]["mapped_optional_libraries"]:
        if not (Path(library).is_relative_to(prefix)):
            raise AssertionError(library)
    report["automatic_single_coded_at_100mp"] = admitted
    if admitted_jxl:
        automatic_jxl = admitted_jxl["samples"][0]
        if not (automatic_jxl["decoded"] and automatic_jxl["largest_native_raster_pixels"] == 48_000_000):
            raise AssertionError
        if not (automatic_jxl["decoded_width"] == 1200 and automatic_jxl["decoded_height"] == 900):
            raise AssertionError
        if not (automatic_jxl["process"]["storage_write_bytes"] == admitted_jxl["before_decode"]["storage_write_bytes"]):
            raise AssertionError
        for library in automatic_jxl["process"]["mapped_optional_libraries"]:
            if not (Path(library).is_relative_to(prefix)):
                raise AssertionError(library)
        report["admitted_jxl_at_100mp"] = admitted_jxl
    if not (rejected_viewer.get("image_failed") and rejected_viewer["first_error_presented_ms"] < 5000):
        raise AssertionError(rejected_viewer)
    if not ("first_preview_presented_ms" not in rejected_viewer):
        raise AssertionError(rejected_viewer)
    report["rejected_single_coded_window_at_25mp"] = rejected_viewer
    report["qualification"] = "Installed package; no development library/plugin environment; hostile allocation overrides; all decodes and bounds checked. Qt base runtime supplied by Ubuntu 24.04."
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified {len(samples)} installed image samples; native codecs stayed lazy and package-local.")
    if args.avif:
        print(
            "AVIF package gate passed: corpus provenance, MIME registration, "
            "PNG lazy loading, representative still/clap/progressive decode, "
            "native-raster accounting, and package-local linkage verified."
        )
        if args.avif_animation:
            print(
                "AVIF animation package gate passed: 4K/8-frame metadata, "
                "frame 0/middle/final decode, timing, backward seek, native "
                "raster accounting, MIME registration and package-local native "
                "libraries verified."
            )
    if args.jp2:
        print("JP2 package gate passed: JP2 and raw J2K decoded through the installed reader.")
    if heif_sequence_path:
        print(
            "HEIF sequence package gate passed: streaming frame-count discovery, "
            "120-frame seeking, timing, backward seek, native-raster accounting, "
            "sequence MIME registration and package-local libheif/libde265 verified."
        )
    if args.apng:
        print(
            "APNG package gate passed: installed APNG registration, representative "
            "fixtures, animation seeking, static-PNG fallthrough, native-raster "
            "accounting, desktop MIME advertisement and package-local backend verified."
        )


if __name__ == "__main__":
    main()
