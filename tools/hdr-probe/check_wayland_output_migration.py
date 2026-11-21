#!/usr/bin/env python3
"""Qualify native HDR/SDR output migration and virtual output hotplug."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile

from check_wayland_e2e import (
    GRAY_FIXTURE_SHA256, PASSTHROUGH_FP16, check_viewer_phase,
    is_sdr_format, require, run_child,
)
from check_wayland_multiwindow import APP_LIVE_RISE_KIB, APP_SLOPE_KIB_PER_ACTION, DESCRIPTOR_RISE, THREAD_RISE
from check_wayland_soak import slope

CYCLE = ["move_hdr", "calibrate", "move_sdr", "move_hdr", "calibrate",
         "off_hdr", "on_hdr", "move_hdr", "calibrate", "off_sdr", "on_sdr", "move_sdr"]


def action_plan(seed, cycles=8):
    random = seed
    actions = CYCLE * cycles
    actions += ["move_hdr", "calibrate", "move_sdr", "use_sdr"]
    plan = []
    for action in actions:
        random = (random * 1664525 + 1013904223) & 0xffffffff
        plan.append(dict(action=action, delay_ms=((random >> 16) % 4) * 25))
    return plan


def expected_state(action):
    if action in (None, "move_sdr", "off_hdr", "on_hdr", "use_sdr"):
        output, code = "sdr", "disabled" if action == "use_sdr" else "wayland-sdr"
    else:
        output, code = "headless", "hdr" if action == "calibrate" else "calibration"
    screens = [output] if action in ("off_hdr", "off_sdr") else ["headless", "sdr"]
    return output, code, screens


def check_resources(soak, cycles):
    """Twice per cycle, each time in the same state: calibrated HDR after the cycle's last HDR
    hotplug, and the cycle's end on the SDR output. Compare the process with itself there."""
    size = len(CYCLE)
    steps = sorted([size * cycle for cycle in range(cycles + 1)] + [size * cycle + 9 for cycle in range(cycles)])
    require([item.get("step") for item in soak] == steps, "Missing or reordered resource measurements")
    for item in soak:
        require(item.get("phase") == ("sdr" if item["step"] % size == 0 else "hdr")
                and all(type(item.get(key)) in (int, float) for key in ("threads", "malloc_live_kib", "rss_kib",
                                                                           "receipt_bytes", "windows", "screens"))
                and type(item.get("descriptors", {}).get("total")) is int and isinstance(item.get("pool"), dict),
                f"Missing measurements at action {item.get('step')}")
    summary = dict(cycles=cycles, actions=size * cycles + 4, receipt_kib=round(soak[-1]["receipt_bytes"] / 1024))
    for phase in ("hdr", "sdr"):
        items = [item for item in soak if item["phase"] == phase]
        first, baseline, last = items[0], items[1], items[-1]
        if phase == "hdr":
            require(first["pool"].get("retained", 0) > 0 and first["pool"].get("targets", 0) > 0,
                    "The calibrated HDR measurement holds no HDR reservations")
        for item in items:
            step = item["step"]
            require(item["pool"] == first["pool"] and item["pool"].get("decode") == 0,
                    f"HDR reservations changed by action {step}: {item['pool']}")
            require(item["screens"] == 2 and item["windows"] == first["windows"],
                    f"Screens or top-level windows changed by action {step}")
        for item in items[2:]:
            step = item["step"]
            require(item["threads"] <= baseline["threads"] + THREAD_RISE,
                    f"Threads grew from {baseline['threads']} to {item['threads']} by action {step}")
            descriptors = item["descriptors"]
            for kind in sorted(set(descriptors) | set(baseline["descriptors"]), key=lambda name: (name == "total", name)):
                require(descriptors.get(kind, 0) <= baseline["descriptors"].get(kind, 0) + DESCRIPTOR_RISE,
                        f"Open {kind} descriptors grew from {baseline['descriptors'].get(kind, 0)} to "
                        f"{descriptors.get(kind, 0)} by action {step}")
        app = [item["malloc_live_kib"] - item["receipt_bytes"] / 1024 for item in items[1:]]
        require(max(app) - app[0] <= APP_LIVE_RISE_KIB,
                f"Live allocations beyond the receipt grew by {max(app) - app[0]:.0f} KiB")
        half = app[len(app) // 2:]
        growth = slope(half) / size if len(half) >= 2 else 0.0
        require(growth <= APP_SLOPE_KIB_PER_ACTION,
                f"Live allocations beyond the receipt grow by {growth:.1f} KiB per action")
        summary[phase] = dict(pool=first["pool"],
                              threads=dict(baseline=baseline["threads"], last=last["threads"],
                                           max=max(item["threads"] for item in items[1:])),
                              descriptors=dict(baseline=baseline["descriptors"], last=last["descriptors"]),
                              app_live_kib=dict(baseline=round(app[0]), last=round(app[-1]), max=round(max(app)),
                                                slope_per_action_second_half=round(growth, 2)),
                              rss_kib=dict(first=first["rss_kib"], last=last["rss_kib"]))
    return summary


def validate(run, seed, fixture, payload, cycles=8):
    report, trace = run["report"], run["trace"]
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    require(run["exit_code"] == 0 and report.get("status") == "passed" and report.get("drained") is True,
            "The output migration viewer did not complete and drain")
    require(report.get("remaining_bytes") == dict(decode=0,retained=0,upload=0,targets=0),
            "The output migration viewer retained resource reservations")
    require(report.get("fixture") == str(fixture) and report.get("fixture_sha256") == digest == GRAY_FIXTURE_SHA256,
            "Wrong output migration fixture")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong output migration executable or missing source receipt")
    plan = action_plan(seed, cycles)
    require(report.get("seed") == seed and report.get("cycles", 8) == cycles and report.get("plan") == plan
            and report.get("completed_actions") == len(plan), "Wrong output migration action plan")
    actions = trace.get("output_actions", [])
    require([(item.get("step"), item.get("action")) for item in actions]
            == [(i+1, entry["action"]) for i, entry in enumerate(plan)],
            "Missing, duplicated or reordered output actions")
    frames = report.get("frames", [])
    require([frame.get("phase") for frame in frames] == [f"output_{i}" for i in range(len(plan) + 1)],
            "Missing, duplicate or reordered output checkpoints")
    previous, previous_proof, serial = None, None, 0
    events = trace.get("output_events", [])
    retired_bindings = set()
    for step, frame in enumerate(frames):
        action = plan[step-1]["action"] if step else None
        action_line = actions[step-1]["line"] if step else 0
        output, code, screens = expected_state(action)
        hdr = code == "hdr"
        phase = frame["phase"]
        proof = check_viewer_phase(trace, phase, frame, fixture, digest, "linear" if hdr else "sdr")
        require(frame.get("output") == output and frame.get("screens") == screens
                and frame.get("code") == code and frame.get("image_ready") is True
                and frame.get("can_remember") is False and frame.get("fullscreen") is False,
                f"Wrong output, review state or session policy in {phase}")
        token = frame.get("window_token")
        require(isinstance(token, str) and re.fullmatch(r"[a-f0-9-]{36}", token)
                and type(frame.get("frame_serial")) is int and frame["frame_serial"] > serial,
                f"Stale viewer generation or frame in {phase}")
        serial = frame["frame_serial"]
        require(proof["commit"] >= proof["presented_commit"] > action_line,
                f"No presentation after the output action in {phase}")
        members = proof.get("entered_outputs", [])
        require(len(members) == 1 and members[0].get("name") == output
                and type(members[0].get("binding")) is int and members[0]["binding"] > 0
                and not members[0].get("removed") and members[0]["binding"] not in retired_bindings,
                f"No native membership on the current output in {phase}")
        require(proof.get("presented_entered_outputs") == members and proof.get("presented_on") == members,
                f"No presentation on the current native output in {phase}")
        if action and action.startswith(("off_", "on_")):
            name = "headless" if action.endswith("hdr") else "sdr"
            kind = "remove" if action.startswith("off_") else "add"
            matching = [event for event in events if event.get("name") == name and event.get("kind") == kind
                        and action_line < event["line"] < proof["commit"]]
            require(len(matching) == 1, f"Missing or duplicate native output {kind} in {phase}")
            binding = matching[0]["binding"]
            if kind == "remove":
                retired_bindings.add(binding)
            else:
                require(binding not in retired_bindings, f"Hotplug reused a retired binding in {phase}")
        if hdr:
            require(frame.get("photo_rendition") == "hdr" and frame.get("photo_visible_hdr_frames", 0) > 0
                    and frame.get("app_surface_tagged") is True
                    and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                    and frame.get("paper_white_nits") == 200 and frame.get("peak_nits") == 800
                    and math.isclose(frame.get("ui_white_scale", 0), 2.5, abs_tol=0.001),
                    f"Wrong HDR calibration or native format in {phase}")
        else:
            require(frame.get("photo_rendition") == "sdr" and frame.get("photo_visible_hdr_frames") == 0
                    and frame.get("app_surface_tagged") is False and frame.get("paper_white_nits") == 0
                    and frame.get("peak_nits") == 0 and frame.get("ui_white_scale") == 0 and is_sdr_format(frame),
                    f"Stale HDR pixels or tagging in {phase}")
        capture, size, logical = frame.get("readback", {}), frame.get("pixel_size"), frame.get("logical_size")
        dpr, scale = frame.get("device_pixel_ratio"), frame.get("photo_scale")
        require(isinstance(size, list) and isinstance(logical, list) and len(size) == len(logical) == 2
                and all(type(n) is int and n > 0 for n in size+logical)
                and all(type(n) in (int,float) and math.isfinite(n) and n > 0 for n in (dpr,scale))
                and size == [math.floor(n*dpr+0.5) for n in logical]
                and capture.get("size") == size and capture.get("revision") == frame["revision"]
                and capture.get("frame_serial") == serial and capture.get("format") == ("rgba16f" if hdr else "rgba8"),
                f"Stale or mismatched native pixel capture in {phase}")
        x, y = 4, 4
        gray = ((192/255+0.055)/1.055)**2.4 * 10 if hdr else 192/255
        samples = capture.get("samples", [])
        require(len(samples) == 3, f"Missing pixel samples in {phase}")
        for offset, sample in zip((-2,0,2), samples):
            rgba = sample.get("rgba", [])
            require(sample.get("x") == x+offset and sample.get("y") == y
                    and len(rgba) == 4 and all(type(n) in (int,float) and math.isfinite(n) for n in rgba)
                    and all(math.isclose(n,gray,rel_tol=0.015,abs_tol=0.012) for n in rgba[:3])
                    and math.isclose(rgba[3],1,abs_tol=0.004), f"Wrong visible photo pixels in {phase}")
        if previous:
            require(token == previous["window_token"] and frame.get("current_request") == previous["current_request"],
                    f"Migration changed the viewer or current source in {phase}")
            require(frame.get("old_revision") == previous["revision"]
                    and int(frame["revision"]) > int(previous["revision"]), f"Unchanged output revision in {phase}")
            if action not in ("calibrate", "use_sdr"):
                require(frame.get("stale_rejected") is True, f"Old calibration was accepted in {phase}")
            if proof["lifetime"] != previous_proof["lifetime"]:
                retired = trace.get("retirements", {}).get(str(previous_proof["lifetime"]),
                          trace.get("retirements", {}).get(previous_proof["lifetime"], 0))
                require(action_line < retired < proof["lifetime"]
                        and previous_proof["lifetime"] not in proof.get("live_lifetimes", []),
                        f"Migration did not retire the previous native surface in {phase}")
        previous, previous_proof = frame, proof
    return check_resources(report.get("soak", []), cycles)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--viewer-payload", type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--cycles", type=int, default=8, help="12-action cycles, 8 to 400")
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be unsigned 32-bit")
    if not 8 <= args.cycles <= 400:
        parser.error("cycles must be from 8 to 400")
    args.viewer, args.fixture = args.viewer.resolve(strict=True), args.fixture.resolve(strict=True)
    payload = (args.viewer_payload or args.viewer).resolve(strict=True)
    args.capture.mkdir()
    record = dict(output_seed=args.seed, output_cycles=args.cycles, status="failed", physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-output-migration-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="wayland", QSG_RHI_BACKEND="vulkan", QSG_RENDER_LOOP="threaded",
                       QT_QPA_PLATFORMTHEME="generic", WAYLAND_DEBUG="client", LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH="1",
                       LICASA_HDR_PAPER_WHITE_NITS="200", LICASA_HDR_PEAK_NITS="800")
            for name in ("CONFIG", "DATA", "CACHE", "STATE"):
                path = Path(directory)/name.lower()
                path.mkdir()
                env[f"XDG_{name}_HOME"] = str(path)
            run = run_child(args.capture, "viewer", [args.viewer, "--viewer", "--wayland-hdr-check", "hdr",
                "--wayland-output-seed", str(args.seed), "--output-cycles", str(args.cycles), args.fixture], env, record)
            record["summary"] = validate(run, args.seed, args.fixture, payload, args.cycles)
            record["soak"] = run["report"].get("soak", [])
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError) as error:
        record["error"] = str(error)
    (args.capture/"result.json").write_text(json.dumps(record, indent=2)+"\n")
    print(f"output migration: {record['status']} ({record.get('error', '')})", flush=True)
    return int(record["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
