#!/usr/bin/env python3
"""Qualify a Motion Photo's HDR cover around playback and viewer actions in one production window."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile

from check_wayland_e2e import PASSTHROUGH_FP16, check_viewer_phase, is_sdr_format, require, run_child

WHITE, PEAK = 200, 800
# Checkpoints in journey order: name, output (hdr/sdr) and content (cover/motion).
CHECKPOINTS = (
    ("cover", "hdr", "cover"), ("playing", "hdr", "motion"), ("paused_seek", "hdr", "motion"),
    ("cover_sdr", "sdr", "cover"), ("playing_sdr", "sdr", "motion"), ("paused_sdr", "sdr", "motion"),
    ("replay_sdr", "sdr", "motion"), ("cover_after_hdr_restore", "hdr", "cover"),
    ("cover_after_stop", "hdr", "cover"), ("cover_after_editor", "hdr", "cover"),
    ("cover_after_cover_change", "hdr", "cover"), ("cover_after_cover_reset", "hdr", "cover"),
    ("cover_after_copy", "hdr", "cover"), ("cover_after_export", "hdr", "cover"),
    ("cover_after_hide", "hdr", "cover"), ("cover_after_navigation", "hdr", "cover"),
    ("cover_after_peer_close", "hdr", "cover"))
# Actions that must precede the checkpoint after them, by step marker.
ACTION_BEFORE = {"cover_sdr": "use-sdr:close-settings", "cover_after_hdr_restore": "restore-hdr",
                 "cover_after_stop": "stop:stop", "cover_after_editor": "editor:close",
                 "cover_after_cover_reset": "cover-change:clear", "cover_after_copy": "copy:dialog",
                 "cover_after_export": "export:dialog", "cover_after_hide": "hide:show",
                 "cover_after_navigation": "navigation:burst", "cover_after_peer_close": "peer:close"}
REQUIRED_STEPS = ("use-sdr:released", "ended", "replay:fresh-frame", "restore-hdr:released", "stop:released",
                  "editor:stopped", "cover-change:stored", "cover-change:applied", "cover-change:cleared",
                  "copy:written", "copy:verified", "copy:released", "export:written", "export:verified",
                  "export:released", "hide:hidden", "navigation:finished", "navigation:settled", "peer:closed")
STEP = re.compile(r"LICASA_MOTION_STEP (\d+) (\S+)$")
NAVIGATION = re.compile(r"LICASA_MOTION_ACTION (\d+) (ordinary|peer|main|toggle)$")
OUTPUTS = {"ordinary.png", "original-copy.jfif", "sdr-export.jpg"}


def navigation_plan(seed, peer):
    random = seed

    def next_value():
        nonlocal random
        random = (random * 1664525 + 1013904223) & 0xffffffff
        return random >> 16

    plan = []
    for _ in range(12):
        action = ("ordinary", "peer", "main", "toggle")[next_value() % 4]
        delay = next_value() % 13
        if action == "peer" and not peer:
            action = "ordinary"
        plan.append(dict(action=action, delay_ms=delay))
    return plan + [dict(action="main", delay_ms=0)]


def linear(value):
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def channels(frame):
    return [sample.get("rgba", []) for sample in frame.get("readback", {}).get("samples", [])]


def same_pixels(first, second):
    return len(first) == len(second) == 3 and all(
        len(a) == len(b) == 4 and all(math.isclose(x, y, rel_tol=0.002, abs_tol=0.002) for x, y in zip(a, b))
        for a, b in zip(first, second))


def video_like(path):
    if path.suffix.lower() in (".mp4", ".mov", ".m4v", ".3gp"):
        return True
    try:
        with path.open("rb") as stream:
            head = stream.read(12)
    except OSError:
        return False
    return len(head) == 12 and head[4:8] == b"ftyp"


def markers(trace_path):
    steps, navigation = [], []
    with trace_path.open(errors="replace") as trace:
        for line_number, line in enumerate(trace, 1):
            if match := STEP.search(line.rstrip()):
                steps.append(dict(index=int(match[1]), name=match[2], line=line_number))
            elif match := NAVIGATION.search(line.rstrip()):
                navigation.append(dict(index=int(match[1]), action=match[2], line=line_number))
    return steps, navigation


def validate(run, trace_path, seed, fixture, peer, payload, output, directories):
    report, trace = run["report"], run["trace"]
    require(run["exit_code"] == 0 and report.get("status") == "passed",
            f"The Motion Photo journey did not pass ({report.get('error', report.get('status'))})")
    require(report.get("drained") is True and report.get("gate_free_after_close") is True
            and report.get("remaining_bytes") == dict(decode=0, retained=0, upload=0, targets=0),
            "The journey retained HDR reservations or the processing gate")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong journey executable or missing source receipt")
    fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    require(Path(report.get("fixture", "")) == fixture and report.get("fixture_sha256") == fixture_hash
            and report.get("fixture_sha256_at_finish") == fixture_hash,
            "The Motion Photo is not the fixture or changed during the journey")
    require(report.get("seed") == seed and report.get("navigation_plan") == navigation_plan(seed, peer),
            "Wrong navigation plan")
    steps, navigation = markers(trace_path)
    require([item["index"] for item in steps] == list(range(1, len(steps) + 1))
            and len({item["name"] for item in steps}) == len(steps), "Missing or repeated journey step markers")
    step_line = {item["name"]: item["line"] for item in steps}
    plan = report["navigation_plan"]
    require([(item["index"], item["action"]) for item in navigation]
            == [(index, entry["action"]) for index, entry in enumerate(plan, 1)]
            and all(step_line["navigation:burst"] < item["line"] for item in navigation),
            "Missing or reordered navigation actions")
    completed = [item.get("step") for item in report.get("steps", [])]
    require(all(name in completed for name in REQUIRED_STEPS), "The journey skipped a required step")

    frames = report.get("frames", [])
    names = [f"motion_{index}_{name}" for index, (name, _, _) in enumerate(CHECKPOINTS, 1)]
    require([frame.get("phase") for frame in frames] == names, "Missing, extra or reordered checkpoints")
    previous_marker = 0
    serial = 0
    token = frames[0].get("window_token")
    covers = {}
    motion = {}
    for frame, (name, mode, content) in zip(frames, CHECKPOINTS):
        phase = frame["phase"]
        proof = check_viewer_phase(trace, phase, frame, fixture, fixture_hash, "linear" if mode == "hdr" else "sdr")
        require(proof["marker_line"] > previous_marker and proof["commit"] > previous_marker,
                f"The {phase} commit does not follow the previous checkpoint")
        if name in ACTION_BEFORE:
            require(proof["commit"] > step_line[ACTION_BEFORE[name]],
                    f"No commit after {ACTION_BEFORE[name]} for {phase}")
        if mode == "hdr":
            require(proof["presented_commit"] > previous_marker, f"No HDR presentation for {phase}")
        previous_marker = proof["marker_line"]
        require(frame.get("window_token") == token and type(frame.get("frame_serial")) is int
                and frame["frame_serial"] > serial, f"Another window or a stale frame in {phase}")
        serial = frame["frame_serial"]
        require(frame.get("output") == output and frame.get("image_ready") is True and frame.get("fullscreen") is True
                and frame.get("fixture_unchanged") is True and frame.get("settings_scrim_visible") is False,
                f"Wrong output, window state or covered photo in {phase}")
        capture = frame.get("readback", {})
        require(capture.get("size") == frame.get("pixel_size") and capture.get("revision") == frame.get("revision")
                and capture.get("frame_serial") == frame.get("frame_serial")
                and capture.get("format") == ("rgba16f" if mode == "hdr" else "rgba8"),
                f"Stale or mismatched native pixel capture in {phase}")
        pixels = channels(frame)
        require(len(pixels) == 3 and all(len(rgba) == 4 and all(type(n) in (int, float) and math.isfinite(n)
                                                                 for n in rgba) for rgba in pixels),
                f"Missing pixel samples in {phase}")
        if mode == "hdr":
            require(frame.get("code") == "hdr" and frame.get("app_surface_tagged") is True
                    and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                    and frame.get("paper_white_nits") == WHITE and frame.get("peak_nits") == PEAK
                    and math.isclose(frame.get("ui_white_scale", 0), WHITE / 80, abs_tol=0.001),
                    f"Wrong HDR output in {phase}")
        else:
            require(frame.get("code") == "disabled" and frame.get("app_surface_tagged") is False
                    and is_sdr_format(frame) and frame.get("paper_white_nits") == 0
                    and frame.get("ui_white_scale") == 0, f"Stale HDR output in {phase}")
        if content == "cover":
            require(frame.get("hdr_still_loaded") is (mode == "hdr") and frame.get("motion_frame") is False
                    and frame.get("session_available") is True and frame.get("session_active") is False
                    and frame.get("gate_free") is True,
                    f"The cover is not the still or playback still holds the gate in {phase}")
            if mode == "hdr":
                require(frame.get("photo_status") == "hdr" and frame.get("photo_rendition") == "hdr"
                        and frame.get("photo_visible_hdr_frames", 0) > 0, f"Missing HDR cover in {phase}")
            else:
                require(frame.get("photo_rendition") == "sdr" and frame.get("photo_visible_hdr_frames") == 0,
                        f"HDR photo frames on SDR output in {phase}")
            reference = covers.setdefault(mode, pixels)
            require(same_pixels(reference, pixels), f"The cover differs from the first {mode.upper()} cover in {phase}")
        else:
            require(frame.get("hdr_still_loaded") is False and frame.get("motion_frame") is True
                    and frame.get("photo_visible_hdr_frames") == 0 and frame.get("session_active") is True
                    and type(frame.get("motion_frame_timestamp_us")) in (int, float)
                    and frame["motion_frame_timestamp_us"] >= 0,
                    f"The video frame is not shown in place of the still in {phase}")
            if mode == "hdr":
                # Video is SDR content composited at paper white, never boosted.
                require(all(0 <= n <= WHITE / 80 * 1.01 for rgba in pixels for n in rgba[:3]),
                        f"Video brighter than paper white in {phase}")
            motion[name] = frame
    # Reported only: at camera size the SDR still and the HDR item are scaled
    # with different filters, so one screen pixel of a textured area differs
    # between them. Cover identity is checked within each output above.
    ratios = [round(hdr / (linear(sdr) * WHITE / 80), 3) for hdr_rgba, sdr_rgba in zip(covers["hdr"], covers["sdr"])
              for hdr, sdr in zip(hdr_rgba[:3], sdr_rgba[:3]) if sdr > 0.02]
    # The same paused video frame on both outputs.
    paused_hdr, paused_sdr = motion["paused_seek"], motion["paused_sdr"]
    require(paused_hdr["motion_frame_timestamp_us"] == paused_sdr["motion_frame_timestamp_us"] > 0
            and paused_hdr.get("session_playing") is False and paused_sdr.get("session_playing") is False,
            "The paused checkpoints do not show the same paused frame")
    for hdr_rgba, sdr_rgba in zip(channels(paused_hdr), channels(paused_sdr)):
        require(all(math.isclose(hdr, linear(sdr) * WHITE / 80, rel_tol=0.02, abs_tol=0.004)
                    for hdr, sdr in zip(hdr_rgba[:3], sdr_rgba[:3])),
                f"The paused frame on HDR output is not its SDR value at paper white: {hdr_rgba} {sdr_rgba}")
    fresh = report.get("replay_fresh_frame_us")
    require(type(fresh) in (int, float) and 0 <= fresh < 500000
            and motion["replay_sdr"]["motion_frame_timestamp_us"] >= fresh,
            "Replay did not restart from the beginning")

    saves = {item.get("step"): item for item in report.get("saves", [])}
    copy, export = output_files = (directories["output"] / "original-copy.jfif", directories["output"] / "sdr-export.jpg")
    require(set(saves) == {"copy", "export"} and all(item.get("playback_active_after") is False
                                                     and 0 <= item.get("elapsed_ms", -1) < 10000
                                                     for item in saves.values())
            and Path(saves["copy"]["path"]) == copy and Path(saves["export"]["path"]) == export,
            "A copy or export did not finish with playback stopped")
    require({path.name for path in directories["output"].iterdir()} == OUTPUTS,
            "Unexpected files in the journey output directory")
    require(copy.read_bytes() == fixture.read_bytes(), "The original copy differs from the Motion Photo")
    data = export.read_bytes()
    info = report.get("export", {})
    require(data[:2] == b"\xff\xd8" and b"MPF\0" not in data and b"hdrgm" not in data
            and b"MotionPhoto" not in data and b"MicroVideo" not in data and b"ftyp" not in data
            and info.get("sha256") == hashlib.sha256(data).hexdigest()
            and (info.get("width"), info.get("height")) == (info.get("upright_width"), info.get("upright_height"))
            and info.get("width", 0) > 0, "The SDR export is not a plain upright SDR JPEG")

    preferred = report.get("preferred_cover", {})
    observed = preferred.get("observed", [])
    first = preferred.get("first_at_or_past_ms", -1)
    require(observed and abs(first - preferred.get("preferred_us", 0) / 1000) <= 40
            and (len(observed) == 1 or first - preferred.get("previous_ms", first) >= 200),
            "Playback did not start from the preferred cover frame after reopening")
    require(len(report.get("gate_observations", [])) >= 6
            and all(item.get("gate_free") is True for item in report["gate_observations"]),
            "Playback did not release the processing gate after a stop")
    require(report.get("open_video_files") == [] and report.get("new_video_files") == [],
            "Playback opened or created a video file")
    created = [str(path) for name, root in directories.items() if name != "output"
               for path in root.rglob("*") if path.is_file() and video_like(path)]
    require(not created, f"Video files were written outside the photo: {created}")
    return dict(checkpoints=len(frames), steps=len(completed), navigation_actions=len(navigation),
                hdr_to_sdr_cover_ratios=ratios, copy_ms=saves["copy"]["elapsed_ms"],
                export_ms=saves["export"]["elapsed_ms"], preferred_cover=dict(
                    preferred_ms=preferred.get("preferred_us", 0) / 1000, first_ms=first))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--viewer-payload", type=Path)
    parser.add_argument("--fixture", required=True, type=Path, help="HDR Motion Photo")
    parser.add_argument("--peer", type=Path, help="Second Motion Photo for the navigation burst")
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--megapixels", type=int, default=536)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be unsigned 32-bit")
    args.viewer, args.fixture = args.viewer.resolve(strict=True), args.fixture.resolve(strict=True)
    peer = args.peer.resolve(strict=True) if args.peer else None
    payload = (args.viewer_payload or args.viewer).resolve(strict=True)
    args.capture.mkdir()
    record = dict(motion_seed=args.seed, output=args.output, status="failed", physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-motion-") as directory:
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
                       "--wayland-motion-journey", str(args.seed), "--motion-journey-output", directories["output"]]
            if peer:
                command += ["--motion-peer", peer]
            run = run_child(args.capture, "viewer", [*command, args.fixture], env, record)
            record["summary"] = validate(run, args.capture / "viewer.wayland.log", args.seed, args.fixture,
                                         peer, payload, args.output, directories)
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError, IndexError, ZeroDivisionError) as error:
        record["error"] = str(error)
    (args.capture / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"motion: {record['status']} ({record.get('error', '')})", flush=True)
    return int(record["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
