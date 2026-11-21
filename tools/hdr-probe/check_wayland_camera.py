#!/usr/bin/env python3
"""Qualify navigation between two camera-sized gain-map photos in one production HDR window."""

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
HIGH_LIMIT = 536
RETIREMENT_LIMIT = 126
RETIREMENT_SIZE = [1920, 1080]
# The runner picks the low limit for its output size; the floor is 384 MiB.
LOW_LIMITS = range(25, 101)
# Checkpoints in order: name, photo and rendition (hdr, or the SDR base on HDR output).
CHECKPOINTS = (
    ("first", "first", "hdr"), ("second", "second", "hdr"), ("first_again", "first", "hdr"),
    ("second_after_burst", "second", "hdr"), ("first_low_limit", "first", "sdr_base"),
    ("second_low_limit", "second", "sdr_base"), ("second_restored_limit", "second", "hdr"),
    ("first_restored_limit", "first", "hdr"), ("first_opened", "first", "hdr"),
    ("second_new_window", "second", "hdr"), ("first_after_window_closed", "first", "hdr"))
# The production Open over a shown photo opens a new window.
NEW_WINDOW = "second_new_window"
ACTION_BEFORE = {"second": "second", "first_again": "first-again", "second_after_burst": "burst",
                 "first_low_limit": "first-low", "second_low_limit": "second-low",
                 "second_restored_limit": "limit-restore", "first_restored_limit": "first-restored",
                 "first_opened": "open-first:dialog", "second_new_window": "open-second:dialog",
                 "first_after_window_closed": "close-new-window"}
STEP = re.compile(r"LICASA_CAMERA_STEP (\d+) (\S+)$")
NAVIGATION = re.compile(r"LICASA_CAMERA_ACTION (\d+) (first|second|ordinary)$")
RETIREMENT_EVENT = re.compile(r"LICASA_RASTER_RETRY (\{.*\})$")


def navigation_plan(seed):
    random = seed

    def next_value():
        nonlocal random
        random = (random * 1664525 + 1013904223) & 0xffffffff
        return random >> 16

    plan = []
    for _ in range(16):
        action = ("first", "second", "ordinary")[next_value() % 3]
        plan.append(dict(action=action, delay_ms=next_value() % 13))
    return plan + [dict(action="second", delay_ms=0)]


def markers(trace_path):
    steps, navigation = [], []
    with trace_path.open(errors="replace") as trace:
        for number, line in enumerate(trace, 1):
            if match := STEP.search(line.rstrip()):
                steps.append(dict(index=int(match[1]), name=match[2], line=number))
            elif match := NAVIGATION.search(line.rstrip()):
                navigation.append(dict(index=int(match[1]), action=match[2], line=number))
    return steps, navigation


def same_pixels(first, second):
    return len(first) == len(second) == 3 and all(
        len(a) == len(b) == 4 and all(math.isclose(x, y, rel_tol=0.002, abs_tol=0.002) for x, y in zip(a, b))
        for a, b in zip(first, second))


def frame_pixels(frame):
    capture = frame["readback"]
    require(capture["format"] == "rgba16f" and capture["frame_serial"] == frame["frame_serial"]
            and capture["size"] == frame["pixel_size"], "Stale FP16 capture")
    width, height = frame["pixel_size"]
    require([(sample["x"], sample["y"]) for sample in capture["samples"]]
            == [(width // 2 + offset, height // 2) for offset in (-2, 0, 2)],
            "Wrong FP16 sample positions")
    values = [sample["rgba"] for sample in capture["samples"]]
    require(len(values) == 3 and all(len(rgba) == 4 and all(type(n) in (int, float) and math.isfinite(n)
                for n in rgba) for rgba in values), "Missing FP16 samples")
    return values


def retirement_trace(trace_path):
    events, close, drain = [], [], []
    with trace_path.open(errors="replace") as trace:
        for line, text in enumerate(trace, 1):
            if match := RETIREMENT_EVENT.search(text.rstrip()):
                events.append(dict(json.loads(match[1]), line=line))
            elif text.rstrip().endswith("LICASA_RASTER_RETRY_CLOSE"):
                close.append(line)
            elif text.rstrip().endswith("LICASA_RASTER_RETRY_DRAIN 1"):
                drain.append(line)
    require(len(close) == len(drain) == 1 and close[0] < drain[0], "Missing or reordered close and drain")
    return events, close[0], drain[0]


def fixture_identity(path):
    info = path.stat()
    mtime = divmod(info.st_mtime_ns, 10**9)
    ctime = divmod(info.st_ctime_ns, 10**9)
    return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{mtime[0]}.{mtime[1]}:{ctime[0]}.{ctime[1]}"


def validate_retirement(run, trace_path, photos, payload, output, sdr_reference):
    report = run["report"]
    recovered = run["exit_code"] == 0 and report.get("status") == "passed"
    require(recovered or (run["exit_code"] == 1 and report.get("status") == "failed"
            and report.get("error") == "timeout:sample:retry_recovered"),
            f"The retirement journey failed before the recovery check ({report.get('error')})")
    require(report.get("retirement_retry") is True and report.get("starting_limit_mp") == RETIREMENT_LIMIT
            and report.get("closing_limit_mp") == RETIREMENT_LIMIT and report.get("drained") is True
            and report.get("remaining_bytes") == dict(decode=0, retained=0, upload=0, targets=0),
            "Wrong fixed budget or undrained reservations")
    build = report["build"]
    require(build["generation"] == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build["executablePath"]).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build["sourceReceipt"]), "Wrong viewer identity")
    hashes = {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in photos.items()}
    require(all(Path(report[key]) == photos[key] and report[f"{key}_sha256"] == hashes[key] for key in photos),
            "Wrong camera fixtures")
    steps, navigation = markers(trace_path)
    expected_steps = ("hdr-settings", "clear-start", "second-reference", "clear-reference",
                      "first-reference", "retry-navigate", "retry-release")
    require(not navigation and [(row["index"], row["name"]) for row in steps]
            == list(enumerate(expected_steps, 1)), "Missing, repeated or reordered fixed journey actions")
    step = {row["name"]: row["line"] for row in steps}
    events, close, drain = retirement_trace(trace_path)
    frames = report["frames"]
    checkpoints = [("second_reference", "second"), ("first_reference", "first"), ("retry_base", "second")]
    if recovered:
        checkpoints.append(("retry_recovered", "second"))
    require([frame["phase"] for frame in frames]
            == [f"camera_{index}_{name}" for index, (name, _) in enumerate(checkpoints, 1)],
            "Missing or reordered retirement frames")
    anchor = frames[0]
    window, revision, budget = (anchor[key] for key in ("window", "revision", "budget_revision"))
    lifetimes, frame_lines, present_lines, pixels, tokens, serials = set(), {}, {}, {}, {}, {}
    previous = 0
    for frame, (name, photo) in zip(frames, checkpoints):
        proof = check_viewer_phase(run["trace"], frame["phase"], frame, photos[photo], hashes[photo], "linear")
        require(previous < proof["presented_commit"] <= proof["commit"] < proof["marker_line"] < close,
                f"Missing ordered presentation for {name}")
        previous = frame_lines[name] = proof["marker_line"]
        present_lines[name] = proof["presented_commit"]
        lifetimes.add(proof["lifetime"])
        token = tokens.setdefault(photo, frame["window_token"])
        require(frame["window_token"] == token and frame["frame_serial"] > serials.get(token, 0),
                f"Wrong sampler or stale serial for {name}")
        serials[token] = frame["frame_serial"]
        require((frame["window"], frame["revision"], frame["budget_revision"]) == (window, revision, budget)
                and frame["limit_mp"] == RETIREMENT_LIMIT and frame["pixel_size"] == RETIREMENT_SIZE
                and frame["output"] == output and frame["app_surface_tagged"] is True and frame["code"] == "hdr"
                and frame["image_ready"] is True and (frame["vk_format"], frame["vk_color_space"]) == PASSTHROUGH_FP16
                and frame["paper_white_nits"] == WHITE and frame["peak_nits"] == PEAK
                and frame["presented_identity"] == fixture_identity(photos[photo]), f"Wrong window, output, or source for {name}")
        pixels[name] = frame_pixels(frame)
        if name == "retry_base":
            require(frame["photo_rendition"] == "sdr" and frame["photo_status"] in ("preparing", "unavailable")
                    and frame["photo_new_hdr_frames"] == 0 and all(0 <= n <= WHITE / 80 * 1.01
                        for rgba in pixels[name] for n in rgba[:3]), "B's SDR base is missing")
        else:
            require(frame["photo_rendition"] == frame["photo_status"] == "hdr"
                    and frame["photo_visible_hdr_frames"] > 0, f"Missing HDR for {name}")
    require(len(lifetimes) == 1 and len(set(tokens.values())) == 2
            and (report["closing_output_revision"], report["closing_budget_revision"]) == (revision, budget),
            "The window, output, or budget changed during the trial")
    require(close < run["trace"]["retirements"].get(next(iter(lifetimes)), 0) < drain,
            "The sampled surface did not retire between close and drain")
    require(step["hdr-settings"] < step["clear-start"] < step["second-reference"]
            < present_lines["second_reference"] < frame_lines["second_reference"] < step["clear-reference"]
            < step["first-reference"] < present_lines["first_reference"],
            "The solo photos were not presented in the fixed journey order")
    require(sdr_reference["fixture_sha256"] == hashes["second"] and sdr_reference["output"] == output
            and sdr_reference["pixel_size"] == RETIREMENT_SIZE and sdr_reference["photo_rendition"] == "sdr"
            and same_pixels(pixels["retry_base"], frame_pixels(sdr_reference)), "B's SDR pixels differ from the reference")
    require(not same_pixels(pixels["first_reference"], pixels["second_reference"]), "The HDR references are identical")
    sdr_frame = frames[2]
    request = sdr_frame["current_request"]
    outgoing = frames[1]["pool_bytes"]
    incoming = frames[0]["pool_bytes"]
    require(outgoing["upload"] == outgoing["retained"] > 0 and incoming["upload"] == incoming["retained"] > 0
            and all(frame["pool_bytes"]["targets"] == outgoing["targets"] for frame in frames)
            and request != anchor["current_request"]
            and sdr_frame["pool_bytes"]["upload"] == outgoing["upload"]
            and sdr_frame["pool_bytes"]["retained"] >= outgoing["retained"], "SDR capture lacks A's lease")

    by_kind = {}
    for event in events:
        require((event["window"], event["revision"], event["budget_revision"]) == (window, revision, budget)
                and step["retry-navigate"] < event["line"] < close and event["targets"] == outgoing["targets"],
                "A retirement event belongs to another window, output or budget")
        by_kind.setdefault(event["event"], []).append(event)
    fence_events = ("texture_retired", "fence_submitted", "fence_complete", "lease_released")
    require(all(len(by_kind.get(kind, [])) == 1 for kind in fence_events), "Missing or repeated fence transition")
    retired, fence, complete, released = [by_kind[kind][0] for kind in fence_events]
    require(retired["source"] == str(photos["first"]) and retired["request"] == frames[1]["current_request"]
            and retired["upload"] == outgoing["upload"], "The retired texture is not A's")
    refused = by_kind.get("raster_refused", [])
    require(refused and all(row["default_fits"] is True and row["upload"] == outgoing["upload"]
                           and row["retained"] == outgoing["retained"] and row["line"] < complete["line"]
                           for row in refused),
            "Missing raster refusal with the default request fitting beside A")
    require(frame_lines["first_reference"] < step["retry-navigate"] < retired["line"] < fence["line"] < refused[0]["line"]
            < present_lines["retry_base"] < frame_lines["retry_base"] < step["retry-release"]
            < complete["line"] < released["line"] < close < drain
            and fence["upload"] == complete["upload"] == outgoing["upload"]
            and released["upload"] == released["retained"] == 0,
            "Fence, captured SDR, or lease-release order is incorrect")

    # Walk the actual decoder serials. A retry must submit a new serial and
    # settle that serial. An independent refresh cannot count as a retry.
    attempts = [event for event in events if event["event"] not in fence_events]
    state, serial, retry = "decode_submitted", 0, 0
    admitted = []
    for event in attempts:
        kind = event["event"]
        require(event["source"] == str(photos["second"])
                and event["request"] == request,
                "A retry event belongs to a different source request")
        if state == "settled":
            require(kind in ("raster_refused", "raster_admitted") and event["serial"] == serial
                    and event["retry"] == retry, "Wrong raster settlement for the submitted decode")
            if kind == "raster_admitted":
                admitted.append(event)
            state = "retry_scheduled" if kind == "raster_refused" else "done"
            continue
        require(kind == state, f"Missing or reordered {state}")
        if kind == "decode_submitted":
            require(event["serial"] > serial and event["retry"] == 0, "A retry did not submit a new decode serial")
            serial, state = event["serial"], "settled"
        elif kind == "retry_scheduled":
            retry += 1
            require(event["serial"] == serial and event["retry"] == retry <= 8, "Invalid bounded retry")
            state = "retry_executed"
        elif kind == "retry_executed":
            require(event["serial"] == serial and event["retry"] == retry, "Wrong timer execution")
            state = "decode_submitted"
    if recovered:
        executions = by_kind.get("retry_executed", [])
        submissions = by_kind.get("decode_submitted", [])
        require(state == "done" and len(refused) >= 2
                and any(row["line"] < complete["line"] for row in executions)
                and len(admitted) == 1 and released["line"] < admitted[0]["line"] < present_lines["retry_recovered"]
                and any(released["line"] < row["line"] < admitted[0]["line"] for row in executions)
                and any(released["line"] < row["line"] < admitted[0]["line"]
                        and row["serial"] == admitted[0]["serial"] for row in submissions)
                and frames[-1]["current_request"] == request and frames[-1]["pool_bytes"] == incoming
                and admitted[0]["retained"] == incoming["retained"] and admitted[0]["upload"] == 0
                and report["closing_pool_upload"] == incoming["upload"]
                and same_pixels(pixels["retry_recovered"], pixels["second_reference"]),
                "B did not recover through the bounded retry after retirement")
    else:
        require(len(attempts) == 2 and attempts[-1]["event"] == "raster_refused" and close > released["line"]
                and report["closing_pool_upload"] == 0
                and report["state_at_finish"]["request"] == sdr_frame["current_request"]
                and Path(report["state_at_finish"]["resident"]) == photos["second"],
                "The failed recovery did not isolate the raster-refusal retry")
    limits = report["budget_bytes"]
    require(0 < outgoing["retained"] < limits["retained"]
            and 0 < incoming["retained"] < limits["retained"]
            and outgoing["retained"] + incoming["retained"] > limits["retained"],
            "The retained cap does not separate solo admission from overlap")
    return dict(recovered=recovered, events=events, pixels=pixels, frames=frame_lines, presented_commits=present_lines,
                close=close, drain=drain,
                budget_bytes=limits, outgoing_bytes=outgoing, incoming_bytes=incoming, final_bytes=report["remaining_bytes"])


def validate(run, trace_path, seed, photos, payload, output):
    report, trace = run["report"], run["trace"]
    require(run["exit_code"] == 0 and report.get("status") == "passed",
            f"The camera journey did not pass ({report.get('error', report.get('status'))})")
    require(report.get("drained") is True and report.get("remaining_bytes") == dict(decode=0, retained=0, upload=0, targets=0),
            "The camera journey retained HDR reservations")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong journey executable or missing source receipt")
    hashes = {}
    for key, path in photos.items():
        hashes[key] = hashlib.sha256(path.read_bytes()).hexdigest()
        require(Path(report.get(key, "")) == path and report.get(f"{key}_sha256") == hashes[key],
                f"The {key} photo is not the fixture")
    require(report.get("seed") == seed and report.get("navigation_plan") == navigation_plan(seed),
            "Wrong navigation plan")
    low_limit = report.get("low_limit_mp")
    require(low_limit in LOW_LIMITS, "The low image limit is outside the qualified range")
    # Targets for two fullscreen 1080p windows exceed the 960 MiB cap; on a
    # smaller output both fit. The runner states which applies.
    new_window_output = report.get("new_window_output")
    # On a smaller output the targets fit, but the retained category holds one
    # maximum raster, so the second camera photo stays on its SDR base.
    require(new_window_output in ("hdr", "memory", "sdr-base"), "Unknown expectation for the opened window")
    steps, navigation = markers(trace_path)
    require([item["index"] for item in steps] == list(range(1, len(steps) + 1))
            and len({item["name"] for item in steps}) == len(steps), "Missing or repeated step markers")
    step_line = {item["name"]: item["line"] for item in steps}
    require([(item["index"], item["action"]) for item in navigation]
            == [(index, entry["action"]) for index, entry in enumerate(report["navigation_plan"], 1)]
            and all(step_line["burst"] < item["line"] for item in navigation),
            "Missing or reordered navigation actions")
    frames = report.get("frames", [])
    names = [f"camera_{index}_{name}" for index, (name, _, _) in enumerate(CHECKPOINTS, 1)]
    require([frame.get("phase") for frame in frames] == names, "Missing, extra or reordered checkpoints")
    # One sampler per window and photo, so one receipt token for each; the main
    # window keeps one native toplevel, and the opened window has another.
    previous, serials, lifetimes, tokens, references = 0, {}, {}, {}, {}
    for frame, (name, photo, rendition) in zip(frames, CHECKPOINTS):
        phase = frame["phase"]
        proof = check_viewer_phase(trace, phase, frame, photos[photo], hashes[photo],
                                   "sdr" if name == NEW_WINDOW and new_window_output == "memory" else "linear")
        require(proof["marker_line"] > previous and proof["commit"] > previous
                and (proof["encoding"] == "sdr" or proof["presented_commit"] > previous),
                f"No presented commit for {phase} after the previous checkpoint")
        if name in ACTION_BEFORE:
            require(proof["commit"] > step_line[ACTION_BEFORE[name]], f"No commit after {ACTION_BEFORE[name]} for {phase}")
        previous = proof["marker_line"]
        window = "new" if name == NEW_WINDOW else "main"
        token = tokens.setdefault((window, photo), frame.get("window_token"))
        lifetime = lifetimes.setdefault(window, proof["lifetime"])
        require(frame.get("window_token") == token and proof["lifetime"] == lifetime
                and type(frame.get("frame_serial")) is int and frame["frame_serial"] > serials.get(token, 0),
                f"Another window or a stale frame in {phase}")
        serials[token] = frame["frame_serial"]
        sdr_output = name == NEW_WINDOW and new_window_output == "memory"
        if sdr_output:
            require(frame.get("output") == output and frame.get("image_ready") is True
                    and frame.get("code") == new_window_output and frame.get("app_surface_tagged") is False
                    and is_sdr_format(frame) and frame.get("photo_rendition") == "sdr"
                    and frame.get("photo_visible_hdr_frames") == 0 and frame.get("ui_white_scale") == 0,
                    f"The opened window is not on SDR output for {new_window_output} in {phase}")
            rendition = "sdr_output"
        else:
            require(frame.get("output") == output and frame.get("image_ready") is True
                    and frame.get("code") == "hdr" and frame.get("app_surface_tagged") is True
                    and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                    and frame.get("paper_white_nits") == WHITE and frame.get("peak_nits") == PEAK,
                    f"Wrong HDR output in {phase}")
        capture = frame.get("readback", {})
        require(capture.get("format") == ("rgba8" if sdr_output else "rgba16f")
                and capture.get("frame_serial") == frame["frame_serial"]
                and capture.get("size") == frame.get("pixel_size"), f"Stale native capture in {phase}")
        pixels = [sample.get("rgba", []) for sample in capture.get("samples", [])]
        require(len(pixels) == 3 and all(len(rgba) == 4 and all(type(n) in (int, float) and math.isfinite(n) for n in rgba)
                                         for rgba in pixels), f"Missing pixel samples in {phase}")
        if rendition == "hdr" and not (name == NEW_WINDOW and new_window_output == "sdr-base"):
            require(frame.get("limit_mp") == HIGH_LIMIT and frame.get("photo_status") == "hdr"
                    and frame.get("photo_rendition") == "hdr" and frame.get("photo_visible_hdr_frames", 0) > 0,
                    f"Missing HDR photo in {phase}")
        if name == NEW_WINDOW and new_window_output == "sdr-base":
            rendition = "sdr_base_shared"
            require(frame.get("limit_mp") == HIGH_LIMIT and frame.get("photo_status") == "unavailable"
                    and frame.get("photo_rendition") == "sdr" and frame.get("photo_new_hdr_frames") == 0
                    and all(0 <= n <= WHITE / 80 * 1.01 for rgba in pixels for n in rgba[:3]),
                    f"The second camera photo was not held on its SDR base in {phase}")
        elif rendition == "sdr_base":
            require(frame.get("limit_mp") == low_limit and frame.get("photo_status") == "unavailable"
                    and frame.get("photo_rendition") == "sdr" and frame.get("photo_new_hdr_frames") == 0,
                    f"The photo was not held on its SDR base at the low limit in {phase}")
            require(all(0 <= n <= WHITE / 80 * 1.01 for rgba in pixels for n in rgba[:3]),
                    f"The SDR base is brighter than paper white in {phase}")
        reference = references.setdefault((photo, rendition), pixels)
        require(same_pixels(reference, pixels), f"The {photo} photo's {rendition} pixels changed in {phase}")
    require(not same_pixels(references[("first", "hdr")], references[("second", "hdr")])
            and len(set(tokens.values())) == 3 and lifetimes["new"] != lifetimes["main"],
            "The photos or windows were not distinct")
    return dict(checkpoints=len(frames), steps=len(report.get("steps", [])), navigation_actions=len(navigation),
                low_limit_mp=low_limit, new_window_output=new_window_output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--first", required=True, type=Path)
    parser.add_argument("--second", required=True, type=Path)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--retirement-retry", action="store_true")
    parser.add_argument("--sdr-reference", type=Path, help="Verified camera result.json at the same output and size")
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be unsigned 32-bit")
    if args.retirement_retry != bool(args.sdr_reference):
        parser.error("--retirement-retry and --sdr-reference must be provided together")
    viewer = args.viewer.resolve(strict=True)
    photos = dict(first=args.first.resolve(strict=True), second=args.second.resolve(strict=True))
    args.capture.mkdir()
    record = dict(camera_seed=args.seed, output=args.output, status="failed", physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-camera-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="wayland", QSG_RHI_BACKEND="vulkan", QSG_RENDER_LOOP="threaded",
                       QT_QPA_PLATFORMTHEME="generic", WAYLAND_DEBUG="client", LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH="1",
                       LICASA_HDR_PAPER_WHITE_NITS=str(WHITE), LICASA_HDR_PEAK_NITS=str(PEAK))
            for name in ("config", "data", "cache", "state", "tmp"):
                path = Path(directory) / name
                path.mkdir()
                env["TMPDIR" if name == "tmp" else f"XDG_{name.upper()}_HOME"] = str(path)
            if args.retirement_retry:
                env["LICASA_CAMERA_RETIREMENT_RETRY"] = "1"
            limit = RETIREMENT_LIMIT if args.retirement_retry else HIGH_LIMIT
            command = [viewer, "--viewer", "--megapixels", str(limit), "--wayland-hdr-check", "hdr",
                       "--wayland-camera-navigation", str(args.seed), "--camera-second", photos["second"],
                       "--camera-output", Path(directory) / "output", photos["first"]]
            run = run_child(args.capture, "viewer", command, env, record)
            trace = args.capture / "viewer.wayland.log"
            if args.retirement_retry:
                baseline = json.loads(args.sdr_reference.read_text())
                require(baseline["status"] == "passed", "The SDR reference did not qualify")
                reference, = [frame for frame in baseline["viewer"]["report"]["frames"]
                              if frame["phase"].endswith("_second_low_limit")]
                record["reference_sha256"] = hashlib.sha256(args.sdr_reference.read_bytes()).hexdigest()
                record["summary"] = validate_retirement(run, trace, photos, viewer, args.output, reference)
                require(record["summary"]["recovered"], "B did not recover after A's lease was released")
            else:
                record["summary"] = validate(run, trace, args.seed, photos, viewer, args.output)
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        record["error"] = str(error)
    (args.capture / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"camera: {record['status']} ({record.get('error', '')})", flush=True)
    return int(record["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
