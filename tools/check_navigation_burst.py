#!/usr/bin/env python3
"""Exercise held-arrow navigation against painted frames, without timing sleeps."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def write_png(path: Path, index: int) -> None:
    width, height = 3000, 2000
    pixel = bytes(((index * 31) % 256, 90, 220 - index * 13))
    row = b"\0" + pixel * width
    compressor = zlib.compressobj(1)
    pixels = bytearray()
    for _ in range(height):
        pixels.extend(compressor.compress(row))
    pixels.extend(compressor.flush())

    def chunk(name: bytes, contents: bytes) -> bytes:
        return (struct.pack(">I", len(contents)) + name + contents
                + struct.pack(">I", zlib.crc32(name + contents)))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", pixels)
        + chunk(b"IEND", b""))


def main() -> int:
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="licasa-burst-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        for index in range(12):
            write_png(images / f"photo-{index:02}.png", index)
        cases = (
            ("right", 1, 0, 30, 0, False),
            ("right", 10, 0, 12, 0, False),
            ("right", 20, 250, 12, 0, False),
            ("right", 40, 250, 12, 0, False),
            ("right", 80, 250, 12, 0, False),
            ("left", 20, 250, 12, 6, False),
            ("left-right", 40, 250, 12, 6, False),
            ("right", 1, 0, 30, 0, True),
        )
        for case_index, (direction, interval, warmup, presses, first, release) in enumerate(cases):
            config = root / f"config-{case_index}"
            cache = root / f"cache-{case_index}"
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
                       QSG_RENDER_LOOP="basic", XDG_CONFIG_HOME=str(config),
                       XDG_CACHE_HOME=str(cache), LICASA_VERIFY_FRAME_PIXELS="1")
            command = [str(executable), "--viewer", "--navigation-sequence",
                       "--sequence-presses", str(presses),
                       "--sequence-interval-ms", str(interval),
                       "--sequence-warmup-ms", str(warmup),
                       "--sequence-idle-ms", "500",
                       "--sequence-direction", direction]
            if release:
                command.append("--sequence-release-last")
            command.append(str(images / f"photo-{first:02}.png"))
            run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
            if run.returncode or "Binding loop detected" in run.stderr:
                raise RuntimeError(f"Burst case {case_index} failed: {run.stdout}\n{run.stderr}")
            report = json.loads(run.stdout)
            sequence = report["displayed_sequence"]
            numbers = [int(name[6:8]) for name in sequence]
            expected_step = -1 if direction == "left" else 1
            if (len(numbers) < (1 if release else 3)
                    or any(((right - left) % 12 not in (1, 11)
                            if direction == "left-right" else
                            (right - left) % 12 != expected_step % 12)
                           for left, right in zip(numbers, numbers[1:]))
                    or len(report["painted_samples"]) != len(numbers)
                    or any(not sample["valid"] or
                           abs(sample["red"] - (index * 31) % 256) > 5 or
                           abs(sample["green"] - 90) > 5 or
                           abs(sample["blue"] - (220 - index * 13)) > 5
                           for index, sample in zip(numbers, report["painted_samples"]))
                    or report["preview_phase_final"]
                    or "licasa_stage=full" not in report["active_request_key_final"]
                    or report["cache_bytes_final"] > 96 * 1024 * 1024
                    or (release and (not report["release_pending_before"]
                                     or report["release_pending_after"]
                                     or sequence[-1] != report["release_file"]))):
                raise RuntimeError(f"Burst case {case_index} skipped pixels or did not settle: {report}")
            print(f"Verified {direction} at {interval} ms: {len(numbers)} painted photos"
                  + ("; queued repeat discarded on release" if release else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
