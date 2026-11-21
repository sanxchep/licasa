#!/usr/bin/env python3
"""Replay and qualify output changes in two production Wayland viewer windows."""

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
from check_wayland_soak import slope

MEASURE_EVERY = 50
# Against the measurement after the first 50 actions, once lazily created
# workers exist: a thread or descriptor kept per window or output change grows
# by at least one per reopen or toggle.
THREAD_RISE = 2
DESCRIPTOR_RISE = 2
# Live allocations less the journey's own frame receipts, which it reports in
# bytes. A window's scene graph or a camera raster kept per reopen is megabytes.
APP_LIVE_RISE_KIB = 16 * 1024
APP_SLOPE_KIB_PER_ACTION = 8


def action_plan(seed, count=100):
    random = seed

    def next_value():
        nonlocal random
        random = (random * 1664525 + 1013904223) & 0xffffffff
        return random >> 16

    plan = [dict(slot=slot, action=action, delay_ms=0)
            for action in ("calibrate", "toggle", "toggle", "hide", "reopen")
            for slot in (0, 1)]
    while len(plan) < count:
        slot, choice = next_value() % 2, next_value() % 4
        action = ("toggle" if choice == 0 else "calibrate" if choice == 1 else
                  "hide" if choice == 2 or len(plan) % 10 else "reopen")
        plan.append(dict(slot=slot, action=action, delay_ms=(next_value() % 4) * 25))
    return plan


def check_resources(soak, count):
    """Bounded workers, descriptors, HDR reservations and live allocations over a long plan."""
    require([item.get("step") for item in soak] == list(range(0, count + 1, MEASURE_EVERY)),
            "Missing or reordered resource measurements")
    for item in soak:
        require(all(type(item.get(key)) in (int, float) for key in ("threads", "malloc_live_kib", "rss_kib",
                                                                       "receipt_bytes", "windows"))
                and type(item.get("descriptors", {}).get("total")) is int and len(item.get("outputs", [])) == 2,
                f"Missing measurements at action {item.get('step')}")
    first = soak[0]
    # Both windows start in HDR; each HDR window holds one share of every category.
    require(first["outputs"] == [True, True] and first["pool"].get("decode") == 0
            and all(first["pool"].get(key, 0) > 0 and first["pool"][key] % 2 == 0
                    for key in ("retained", "upload", "targets")),
            "The first measurement does not hold two HDR windows' reservations")
    share = {key: value // 2 for key, value in first["pool"].items()}
    for item in soak:
        windows = sum(1 for hdr in item["outputs"] if hdr)
        require(item["pool"] == {key: value * windows for key, value in share.items()},
                f"HDR reservations at action {item['step']} are not {windows} window share(s): {item['pool']}")
        require(item["windows"] == first["windows"], f"Top-level windows changed by action {item['step']}")
    baseline = soak[1]
    for item in soak[2:]:
        step = item["step"]
        require(item["threads"] <= baseline["threads"] + THREAD_RISE,
                f"Threads grew from {baseline['threads']} to {item['threads']} by action {step}")
        descriptors = item["descriptors"]
        for kind in sorted(set(descriptors) | set(baseline["descriptors"]), key=lambda name: (name == "total", name)):
            require(descriptors.get(kind, 0) <= baseline["descriptors"].get(kind, 0) + DESCRIPTOR_RISE,
                    f"Open {kind} descriptors grew from {baseline['descriptors'].get(kind, 0)} to "
                    f"{descriptors.get(kind, 0)} by action {step}")
    app = [item["malloc_live_kib"] - item["receipt_bytes"] / 1024 for item in soak[1:]]
    require(max(app) - app[0] <= APP_LIVE_RISE_KIB,
            f"Live allocations beyond the receipt grew by {max(app) - app[0]:.0f} KiB")
    half = app[len(app) // 2:]
    growth = slope(half) / MEASURE_EVERY if len(half) >= 2 else 0.0
    require(growth <= APP_SLOPE_KIB_PER_ACTION,
            f"Live allocations beyond the receipt grow by {growth:.1f} KiB per action")
    last = soak[-1]
    return dict(actions=count, threads=dict(baseline=baseline["threads"], last=last["threads"],
                                            max=max(item["threads"] for item in soak[1:])),
                descriptors=dict(baseline=baseline["descriptors"], last=last["descriptors"]),
                app_live_kib=dict(baseline=round(app[0]), last=round(app[-1]), max=round(max(app)),
                                  slope_per_action_second_half=round(growth, 2)),
                receipt_kib=round(last["receipt_bytes"] / 1024), rss_kib=dict(first=first["rss_kib"], last=last["rss_kib"]),
                hdr_window_share=share)


def validate(run, seed, fixture, payload, output, count=100):
    report, trace = run["report"], run["trace"]
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    require(run["exit_code"] == 0 and report.get("status") == "passed" and report.get("drained") is True,
            "The multiwindow viewer did not complete and drain")
    require(report.get("remaining_bytes") == dict(decode=0,retained=0,upload=0,targets=0),
            "The multiwindow viewer retained resource reservations")
    retirements = {int(key): line for key, line in trace.get("retirements", {}).items()}
    require(report.get("fixture") == str(fixture) and report.get("fixture_sha256") == digest == GRAY_FIXTURE_SHA256,
            "Wrong multiwindow fixture")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong multiwindow executable or missing source receipt")
    plan = action_plan(seed, count)
    require(report.get("seed") == seed and report.get("actions", 100) == count and report.get("plan") == plan
            and report.get("completed_actions") == len(plan),
            "Wrong multiwindow action plan")
    actions = trace.get("multi_actions", [])
    require([(item.get("step"), item.get("slot"), item.get("action")) for item in actions]
            == [(i+1, entry["slot"], entry["action"]) for i, entry in enumerate(plan)],
            "Missing, duplicated or reordered multiwindow actions")
    frames = report.get("frames", [])
    indexed = {frame.get("phase"): frame for frame in frames}
    expected_phases = {f"multi_0_{slot}_settled" for slot in (0, 1)}
    for step, entry in enumerate(plan, 1):
        expected_phases.update((f"multi_{step}_{1-entry['slot']}_during",
                                f"multi_{step}_0_settled", f"multi_{step}_1_settled"))
    require(len(frames) == len(indexed) == len(expected_phases) and set(indexed) == expected_phases,
            "Missing, duplicate or foreign-window checkpoints")
    states = [dict(hdr=True, high=False, white=200, peak=800),
              dict(hdr=True, high=False, white=100, peak=200)]
    previous, proofs, serials = {}, {}, {}
    used_tokens = set()
    for step in range(len(plan)+1):
        entry = plan[step-1] if step else None
        action_line = actions[step-1]["line"] if step else 0
        target = entry["slot"] if entry else None
        if entry:
            state = states[target]
            if entry["action"] == "toggle":
                state["hdr"] = not state["hdr"]
            if entry["action"] == "calibrate":
                state["hdr"], state["high"] = True, not state["high"]
                state["white"] = (300 if state["high"] else 200) if target == 0 else (150 if state["high"] else 100)
                state["peak"] = state["white"] * (4 if target == 0 else 2)
        stages = [("settled", 0), ("settled", 1)] if not step else [
            ("during", 1-target), ("settled", 0), ("settled", 1)]
        current = {}
        for stage, slot in stages:
            phase = f"multi_{step}_{slot}_{stage}"
            frame = indexed[phase]
            state = states[slot]
            hdr = state["hdr"]
            proof = check_viewer_phase(trace, phase, frame, fixture, digest, "linear" if hdr else "sdr")
            require(frame.get("slot") == slot and frame.get("step") == step
                    and frame.get("output") == output and frame.get("image_ready") is True
                    and frame.get("can_remember") is False and frame.get("fullscreen") is False,
                    f"Wrong window, output or session policy in {phase}")
            token, serial = frame.get("window_token"), frame.get("frame_serial")
            require(isinstance(token, str) and re.fullmatch(r"[a-f0-9-]{36}", token)
                    and type(serial) is int and serial > serials.get(token, 0),
                    f"Stale or invalid window generation in {phase}")
            serials[token] = serial
            require(proof["commit"] > action_line and proof["presented_commit"] > action_line,
                    f"No presentation after the action in {phase}")
            if stage == "during" and entry["action"] in ("hide","reopen"):
                retired = retirements.get(proofs[target]["lifetime"], 0)
                unmapped = any(item["lifetime"] == proofs[target]["lifetime"]
                               and action_line < item["line"] < proof["commit"]
                               for item in trace.get("unmaps", []))
                require(action_line < retired < proof["commit"] or unmapped,
                        f"The target was not absent during the peer observation in {phase}")
            if hdr:
                require(frame.get("code") == "hdr" and frame.get("photo_rendition") == "hdr"
                        and frame.get("photo_visible_hdr_frames", 0) > 0 and frame.get("app_surface_tagged") is True
                        and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                        and frame.get("paper_white_nits") == state["white"] and frame.get("peak_nits") == state["peak"]
                        and math.isclose(frame.get("ui_white_scale", 0), state["white"]/80, abs_tol=0.001),
                        f"Wrong HDR calibration or pixels policy in {phase}")
            else:
                require(frame.get("code") == "disabled" and frame.get("photo_rendition") == "sdr"
                        and frame.get("photo_visible_hdr_frames") == 0 and frame.get("app_surface_tagged") is False
                        and frame.get("paper_white_nits") == 0 and frame.get("peak_nits") == 0
                        and frame.get("ui_white_scale") == 0 and is_sdr_format(frame),
                        f"Stale HDR output in {phase}")
            capture, size, dpr = frame.get("readback", {}), frame.get("pixel_size"), frame.get("device_pixel_ratio")
            logical = frame.get("logical_size")
            require(isinstance(size, list) and isinstance(logical, list) and len(size) == len(logical) == 2
                    and all(type(n) is int and n > 0 for n in size+logical)
                    and type(dpr) in (int,float) and math.isfinite(dpr) and dpr > 0
                    and size == [math.floor(n*dpr+0.5) for n in logical]
                    and capture.get("size") == size and capture.get("revision") == frame["revision"]
                    and capture.get("frame_serial") == serial
                    and capture.get("format") == ("rgba16f" if hdr else "rgba8"),
                    f"Stale or mismatched native pixel capture in {phase}")
            samples = capture.get("samples", [])
            gray = ((192/255+0.055)/1.055)**2.4 * (state["peak"]/80) if hdr else 192/255
            require(len(samples) == 3, f"Missing pixel samples in {phase}")
            for offset, sample in zip((-2,0,2), samples):
                rgba = sample.get("rgba", [])
                require(sample.get("x") == 4+offset and sample.get("y") == 4
                        and len(rgba) == 4 and all(type(n) in (int,float) and math.isfinite(n) for n in rgba)
                        and all(math.isclose(n,gray,rel_tol=0.015,abs_tol=0.012) for n in rgba[:3])
                        and math.isclose(rgba[3],1,abs_tol=0.004),
                        f"Wrong visible photo pixels in {phase}")
            if entry and slot != target:
                for key in ("revision","window_token","surface_id","current_request","code","paper_white_nits","peak_nits"):
                    require(frame.get(key) == previous[slot].get(key), f"Peer window changed {key} in {phase}")
                require(proof["lifetime"] == proofs[slot]["lifetime"], f"Peer native surface changed in {phase}")
            if stage == "settled":
                current[slot] = frame
                old = previous.get(slot)
                if old and entry and slot == target:
                    old_proof = proofs[slot]
                    if entry["action"] == "reopen":
                        require(token != old["window_token"] and token not in used_tokens
                                and retirements.get(old_proof["lifetime"], 0) > action_line,
                                f"Reopen reused or failed to retire its window in {phase}")
                    else:
                        require(token == old["window_token"], f"Unexpected new viewer in {phase}")
                    if proof["lifetime"] != old_proof["lifetime"]:
                        retired = retirements.get(old_proof["lifetime"], 0)
                        require(action_line < retired < proof["lifetime"]
                                and old_proof["lifetime"] not in proof.get("live_lifetimes", []),
                                f"Unrelated replacement surface in {phase}")
                    if entry["action"] in ("toggle","calibrate"):
                        require(int(frame["revision"]) > int(old["revision"]), f"Unchanged output revision in {phase}")
                proofs[slot] = proof
        require(current[0]["window_token"] != current[1]["window_token"]
                and proofs[0]["lifetime"] != proofs[1]["lifetime"], "Both slots refer to the same window")
        previous = current
        used_tokens.update(frame["window_token"] for frame in current.values())
    return check_resources(report.get("soak", []), count)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--viewer-payload", type=Path)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--actions", type=int, default=100, help="a multiple of 50 from 100 to 5000")
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be unsigned 32-bit")
    if not 100 <= args.actions <= 5000 or args.actions % 50:
        parser.error("actions must be a multiple of 50 from 100 to 5000")
    args.viewer, args.fixture = args.viewer.resolve(strict=True), args.fixture.resolve(strict=True)
    payload = (args.viewer_payload or args.viewer).resolve(strict=True)
    args.capture.mkdir()
    record = dict(multiwindow_seed=args.seed, multiwindow_actions=args.actions, output=args.output, status="failed",
                  physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-multiwindow-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="wayland", QSG_RHI_BACKEND="vulkan",QSG_RENDER_LOOP="threaded",
                       QT_QPA_PLATFORMTHEME="generic",WAYLAND_DEBUG="client",LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH="1",
                       LICASA_HDR_PAPER_WHITE_NITS="200", LICASA_HDR_PEAK_NITS="800")
            for name in ("CONFIG","DATA","CACHE","STATE"):
                path=Path(directory)/name.lower()
                path.mkdir()
                env[f"XDG_{name}_HOME"]=str(path)
            run = run_child(args.capture,"viewer",[args.viewer,"--viewer","--wayland-hdr-check","hdr",
                "--wayland-multiwindow-seed",str(args.seed),"--multiwindow-actions",str(args.actions),
                args.fixture],env,record)
            record["summary"] = validate(run,args.seed,args.fixture,payload,args.output,args.actions)
            record["soak"] = run["report"].get("soak", [])
        record["status"]="passed"
    except (OSError,ValueError,KeyError,TypeError) as error:
        record["error"]=str(error)
    (args.capture/"result.json").write_text(json.dumps(record,indent=2)+"\n")
    print(f"multiwindow: {record['status']} ({record.get('error', '')})",flush=True)
    return int(record["status"]!="passed")


if __name__ == "__main__":
    raise SystemExit(main())
