#!/usr/bin/env python3
"""Qualify a repeated Motion Photo lifecycle in one HDR window: every round ends on the
same HDR cover, and the process returns to the same workers, descriptors, HDR
reservations and allocator level."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile

from check_wayland_e2e import PASSTHROUGH_FP16, check_viewer_phase, require, run_child
from check_wayland_motion import WHITE, PEAK, channels, markers, same_pixels, video_like

ROUND_STEPS = ("first-frame", "paused", "seek-frame", "ended", "fresh-frame", "editor-stopped", "tray",
               "away-shown", "back-ready", "sdr-shown", "settled")
# Bounds between the first measured round and every later one. A thread or a
# descriptor kept per playback grows by at least one per round, and a kept
# camera raster or video buffer by megabytes per round. Caches warm up over
# the first rounds, so the growth per round is fitted to the second half; the
# soak's own receipt adds about 21 KiB of live allocations per round.
THREAD_RISE = 2
DESCRIPTOR_RISE = 2
LIVE_RISE_KIB = 24 * 1024
SLOPE_KIB_PER_ROUND = 128
OUTPUTS = {"ordinary.png"}


def slope(values):
    """Least-squares growth per round."""
    count = len(values)
    mean_x, mean_y = (count - 1) / 2, sum(values) / count
    spread = sum((x - mean_x) ** 2 for x in range(count))
    return sum((x - mean_x) * (y - mean_y) for x, y in enumerate(values)) / spread if spread else 0.0


def validate(run, trace_path, rounds, fixture, payload, output, directories):
    report, trace = run["report"], run["trace"]
    require(run["exit_code"] == 0 and report.get("status") == "passed",
            f"The lifecycle soak did not pass ({report.get('error', report.get('status'))})")
    require(report.get("drained") is True and report.get("gate_free_after_close") is True
            and report.get("remaining_bytes") == dict(decode=0, retained=0, upload=0, targets=0),
            "The soak retained HDR reservations or the processing gate")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong soak executable or missing source receipt")
    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    require(Path(report.get("fixture", "")) == fixture and report.get("fixture_sha256") == fixture_hash
            and report.get("fixture_sha256_at_finish") == fixture_hash,
            "The Motion Photo is not the fixture or changed during the soak")
    require(report.get("soak_rounds") == rounds, "Wrong number of soak rounds")
    steps, _ = markers(trace_path)
    require([item["index"] for item in steps] == list(range(1, len(steps) + 1))
            and len({item["name"] for item in steps}) == len(steps), "Missing or repeated soak step markers")
    step_line = {item["name"]: item["line"] for item in steps}
    completed = [item.get("step") for item in report.get("steps", [])]
    missing = [f"round{index}:{name}" for index in range(1, rounds + 1) for name in ROUND_STEPS
               if f"round{index}:{name}" not in completed]
    require(not missing, f"The soak skipped a required step: {missing[:3]}")
    require(report.get("playback_checks", 0) >= rounds, "A round played without the playback checks")

    frames = report.get("frames", [])
    names = ["motion_1_cover"] + [f"motion_{index}_soak_cover" for index in range(2, rounds + 2)]
    require([frame.get("phase") for frame in frames] == names, "Missing, extra or reordered checkpoints")
    previous_marker, serial, token, reference = 0, 0, frames[0].get("window_token"), None
    for index, frame in enumerate(frames):
        phase = frame["phase"]
        proof = check_viewer_phase(trace, phase, frame, fixture, fixture_hash, "linear")
        require(proof["marker_line"] > previous_marker and proof["commit"] > previous_marker
                and proof["presented_commit"] > previous_marker,
                f"The {phase} commit does not follow the previous checkpoint")
        if index:
            require(proof["commit"] > step_line.get(f"round{index}:restore-hdr", math.inf),
                    f"No commit after round{index}:restore-hdr for {phase}")
        previous_marker = proof["marker_line"]
        require(frame.get("window_token") == token and type(frame.get("frame_serial")) is int
                and frame["frame_serial"] > serial, f"Another window or a stale frame in {phase}")
        serial = frame["frame_serial"]
        require(frame.get("output") == output and frame.get("image_ready") is True and frame.get("fullscreen") is True
                and frame.get("fixture_unchanged") is True and frame.get("settings_scrim_visible") is False,
                f"Wrong output, window state or covered photo in {phase}")
        capture = frame.get("readback", {})
        require(capture.get("size") == frame.get("pixel_size") and capture.get("revision") == frame.get("revision")
                and capture.get("frame_serial") == frame.get("frame_serial") and capture.get("format") == "rgba16f",
                f"Stale or mismatched native pixel capture in {phase}")
        pixels = channels(frame)
        require(len(pixels) == 3 and all(len(rgba) == 4 and all(type(n) in (int, float) and math.isfinite(n)
                                                                 for n in rgba) for rgba in pixels),
                f"Missing pixel samples in {phase}")
        require(frame.get("code") == "hdr" and frame.get("app_surface_tagged") is True
                and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                and frame.get("paper_white_nits") == WHITE and frame.get("peak_nits") == PEAK
                and math.isclose(frame.get("ui_white_scale", 0), WHITE / 80, abs_tol=0.001),
                f"Wrong HDR output in {phase}")
        require(frame.get("hdr_still_loaded") is True and frame.get("motion_frame") is False
                and frame.get("session_available") is True and frame.get("session_active") is False
                and frame.get("gate_free") is True and frame.get("photo_status") == "hdr"
                and frame.get("photo_rendition") == "hdr" and frame.get("photo_visible_hdr_frames", 0) > 0,
                f"The HDR cover is missing or playback still holds the gate in {phase}")
        reference = reference or pixels
        require(same_pixels(reference, pixels), f"The cover differs from the first HDR cover in {phase}")

    soak = report.get("soak", [])
    require([item.get("round") for item in soak] == list(range(1, rounds + 1)), "Missing or reordered soak rounds")
    for index, item in enumerate(soak, 1):
        require(step_line.get(f"round{index}:measure", 0) > step_line.get(f"round{index}:restore-hdr", math.inf),
                f"Round {index} was measured before its lifecycle finished")
        require(item.get("gate_free") is True, f"Playback still held the processing gate after round {index}")
        require(all(type(item.get(key)) in (int, float) for key in ("threads", "malloc_live_kib", "rss_kib"))
                and type(item.get("descriptors", {}).get("total")) is int, f"Missing measurements in round {index}")
    first = soak[0]
    pool = first.get("pool", {})
    # The shown photo holds its retained raster, its texture's upload lease and
    # the window's display targets; no decode is in flight.
    require(pool.get("decode") == 0 and pool.get("retained", 0) > 0 and pool.get("upload", 0) > 0
            and pool.get("targets", 0) > 0, "The first round does not hold exactly the shown HDR photo")
    for item in soak[1:]:
        round_number = item["round"]
        require(item.get("pool") == pool, f"HDR reservations changed by round {round_number}: {item.get('pool')}")
        require(item["threads"] <= first["threads"] + THREAD_RISE,
                f"Threads grew from {first['threads']} to {item['threads']} by round {round_number}")
        descriptors = item["descriptors"]
        # Kinds before the total, so a leak names the kind it leaked.
        for kind in sorted(set(descriptors) | set(first["descriptors"]), key=lambda name: (name == "total", name)):
            require(descriptors.get(kind, 0) <= first["descriptors"].get(kind, 0) + DESCRIPTOR_RISE,
                    f"Open {kind} descriptors grew from {first['descriptors'].get(kind, 0)} to "
                    f"{descriptors.get(kind, 0)} by round {round_number}")
        require(item["malloc_live_kib"] <= first["malloc_live_kib"] + LIVE_RISE_KIB,
                f"Live allocations grew by {item['malloc_live_kib'] - first['malloc_live_kib']:.0f} KiB "
                f"by round {round_number}")
    live = [item["malloc_live_kib"] for item in soak]
    growth = slope(live[len(live) // 2:]) if len(live) >= 4 else 0.0
    require(growth <= SLOPE_KIB_PER_ROUND, f"Live allocations grow by {growth:.0f} KiB per round")
    require(report.get("open_video_files") == [] and report.get("new_video_files") == [],
            "Playback opened or created a video file")
    created = [str(path) for name, root in directories.items() if name != "output"
               for path in root.rglob("*") if path.is_file() and video_like(path)]
    require(not created, f"Video files were written outside the photo: {created}")
    require({path.name for path in directories["output"].iterdir()} == OUTPUTS,
            "Unexpected files in the soak output directory")
    durations = [b["elapsed_ms"] - a["elapsed_ms"] for a, b in zip(soak, soak[1:])]
    last = soak[-1]
    return dict(rounds=rounds, checkpoints=len(frames), steps=len(completed),
                threads=dict(first=first["threads"], last=last["threads"],
                             max=max(item["threads"] for item in soak)),
                descriptors=dict(first=first["descriptors"], last=last["descriptors"]),
                malloc_live_kib=dict(first=first["malloc_live_kib"], last=last["malloc_live_kib"],
                                     max=max(live), slope_per_round_second_half=round(growth, 1)),
                rss_kib=dict(first=first["rss_kib"], last=last["rss_kib"]), pool=pool,
                round_ms=dict(min=min(durations, default=0), max=max(durations, default=0)),
                total_ms=last["elapsed_ms"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--viewer-payload", type=Path)
    parser.add_argument("--fixture", required=True, type=Path, help="HDR Motion Photo")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--megapixels", type=int, default=536)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    if not 2 <= args.rounds <= 500:
        parser.error("rounds must be between 2 and 500")
    args.viewer, args.fixture = args.viewer.resolve(strict=True), args.fixture.resolve(strict=True)
    payload = (args.viewer_payload or args.viewer).resolve(strict=True)
    args.capture.mkdir()
    # The soak runs one plan; the navigation seed is fixed and unused.
    record = dict(motion_seed=0, soak_rounds=args.rounds, output=args.output, status="failed",
                  physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-soak-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="wayland", QSG_RHI_BACKEND="vulkan", QSG_RENDER_LOOP="threaded",
                       QT_QPA_PLATFORMTHEME="generic", WAYLAND_DEBUG="client", LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH="1",
                       LICASA_HDR_PAPER_WHITE_NITS=str(WHITE), LICASA_HDR_PEAK_NITS=str(PEAK),
                       QT_MEDIA_BACKEND="ffmpeg", QT_FFMPEG_DECODING_HW_DEVICE_TYPES=",")
            directories = {}
            for name in ("config", "data", "cache", "state", "tmp"):
                directories[name] = path = Path(directory) / name
                path.mkdir()
                env["TMPDIR" if name == "tmp" else f"XDG_{name.upper()}_HOME"] = str(path)
            directories["output"] = Path(directory) / "output"
            command = [args.viewer, "--viewer", "--megapixels", str(args.megapixels), "--wayland-hdr-check", "hdr",
                       "--wayland-motion-journey", "0", "--motion-journey-output", directories["output"],
                       "--motion-soak-rounds", str(args.rounds)]
            run = run_child(args.capture, "viewer", [*command, args.fixture], env, record)
            record["summary"] = validate(run, args.capture / "viewer.wayland.log", args.rounds, args.fixture,
                                         payload, args.output, directories)
            record["soak"] = run["report"].get("soak", [])
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError, IndexError, ZeroDivisionError) as error:
        record["error"] = str(error)
    (args.capture / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"soak: {record['status']} ({record.get('error', '')})", flush=True)
    return int(record["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
