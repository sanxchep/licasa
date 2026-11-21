"""Exercise JPEG gain-map reconstruction through the offscreen Vulkan probe."""

import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def require(condition, detail):
    if not condition:
        raise ValueError(f"Invalid pixel receipt: {detail}")


def capture(probe, image, headroom, *points):
    command = [probe, "--readback-gainmap", str(image), "--gainmap-headroom", str(headroom)]
    for point in points or ("16,16",):
        command.extend(("--sample", point))
    result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=20)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    return result.returncode, json.loads(result.stdout)


def check_render(probe, image, case):
    points = ("16,16",)
    size = (32, 32, 8, 8)
    linear_gray = ((192 / 255 + 0.055) / 1.055) ** 2.4
    if case == "p3":
        headroom = 4
        # Independently linearize the base JPEG's (254, 0, 0) and multiply
        # by four in Display P3, then apply the D65 P3-to-sRGB matrix.
        expected = ((4.85616, -0.16673, -0.07785),)
        primaries = "display_p3"
    elif case == "rgb-rotated":
        headroom = 4
        size = (16, 32, 8, 4)
        # EXIF 6 maps the stored (0, 0) and (28, 12) gain-grid
        # points to (15, 0) and (3, 28). The map is red and white there.
        points = ("15,0", "3,28")
        expected = (
            (linear_gray * 4 ** (254 / 255), linear_gray, linear_gray),
            (linear_gray * 4, linear_gray * 2, linear_gray * 3),
        )
        primaries = "srgb"
    else:
        headroom = int(case)
        expected = ((linear_gray * min(headroom, 4),) * 3,)
        primaries = "srgb"
    code, report = capture(probe, image, headroom, *points)
    if code == 77:
        require(report["status"] in ("vulkan_unavailable", "scene_graph_unavailable", "fp16_unavailable"), 'report["status"] in ("vulkan_unavailable", "scene_graph_unavailable", "fp16_unavailable")')
        return code
    require(code == 0 and report["status"] == "passed", report)
    require(report["mode"] == "gainmap_fp16_readback" and not report["physical_hdr_verified"], report)
    require(report["headroom"] == headroom and report["source_primaries"] == primaries, report)
    require((report["width"], report["height"], report["gain_width"], report["gain_height"]) == size, report)
    pixels = size[0] * size[1]
    require(report["pixels_checked"] == pixels and report["mismatched_pixels"] == 0, report)
    require(report["jpeg_bytes"] == image.stat().st_size, report)
    require(report["reused_decode_raster"] and report["async_decode"], report)
    require(report["decode_reservation_after_completion"] == 0, report)
    require(report["retained_raster_bytes"] == pixels * 8, report)
    require(report["upload_reserved_bytes"] == pixels * 8, report)
    for field in ("decoded_rgba16f_bytes", "conversion_raster_bytes",
                  "upload_rgba16f_bytes", "target_rgba16f_bytes", "readback_bytes"):
        require(report[field] == pixels * 8, report)
    require(len(report["samples"]) == len(points), report)
    for point, sample, channels in zip(points, report["samples"], expected):
        require(f'{sample["x"]},{sample["y"]}' == point and len(sample["rgba"]) == 4, sample)
        for actual, reference in zip(sample["rgba"], (*channels, 1.0)):
            require(math.isclose(actual, reference, rel_tol=0.015, abs_tol=0.004), (sample, channels))
    if case == "p3":
        require(report["min_linear_component"] < 0 and report["peak_linear_component"] > 1, report)
    elif case != "rgb-rotated":
        require(report["pixels_above_sdr_white"] == (0 if headroom == 1 else pixels), report)
    return 0


def check_rotated_rgb(probe, fixtures):
    data = bytearray((fixtures / "jpeg-gainmap-rgb.jpg").read_bytes())
    # Insert EXIF orientation 6 before MPF. Its relative gain offset stays
    # valid because the MPF and secondary JPEG move by the same distance.
    exif = bytes.fromhex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000")
    primary_size_field = data.index(b"MPF\0") + 4 + 54
    primary_size, = struct.unpack_from(">I", data, primary_size_field)
    struct.pack_into(">I", data, primary_size_field, primary_size + len(exif))
    data[2:2] = exif
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "rotated.jpg"
        path.write_bytes(data)
        return check_render(probe, path, "rgb-rotated")


def check_invalid(probe, fixtures):
    source = fixtures / "jpeg-gainmap-dual.jpg"
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "truncated.jpg"
        path.write_bytes(source.read_bytes()[:-2])
        for image, headroom, point, status in (
            (path, 4, "16,16", "gainmap_decode_failed"),
            (source, "nan", "16,16", "invalid_headroom"),
            (source, 4, "32,16", "invalid_sample"),
        ):
            code, report = capture(probe, image, headroom, point)
            require(code == 1 and report["status"] == status, report)
            require("upload_rgba16f_bytes" not in report, report)
    return 0


if __name__ == "__main__":
    binary, root, scenario = sys.argv[1:]
    fixtures = Path(root)
    if scenario == "invalid":
        sys.exit(check_invalid(binary, fixtures))
    if scenario == "rgb-rotated":
        sys.exit(check_rotated_rgb(binary, fixtures))
    fixture = "jpeg-gainmap-p3-red.jpg" if scenario == "p3" else "jpeg-gainmap-dual.jpg"
    sys.exit(check_render(binary, fixtures / fixture, scenario))
