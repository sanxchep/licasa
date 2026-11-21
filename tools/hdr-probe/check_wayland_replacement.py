#!/usr/bin/env python3
"""Replay and qualify same-path photo replacement in two production Wayland viewer windows."""

import argparse
import collections
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
from urllib.parse import urlsplit

from check_wayland_e2e import (
    GRAY_FIXTURE_SHA256, PASSTHROUGH_FP16, check_viewer_phase,
    is_sdr_format, require, run_child,
)

RED_FIXTURE_SHA256 = "204ccaea4983bbf8e916544ee6170d164da2c6d569e091c5c180d072f6e9d349"
SCRATCH_NAME = "photo #1 ü.jpg"
BARRIERS = (0, 1, 2, 3, 5, 8, 13, 21, 34, 55)
FIXED = ("replace-atomic", "calibrate", "replace-inplace", "toggle", "toggle",
         "replace-symlink", "hide", "reload", "reopen", "calibrate")
# In FIXED the first window always falls behind, holding the red photo. One
# more replacement swaps the photos, so each is held in every mode.
MIRRORED = ((1, "replace-atomic"), (0, "calibrate"), (0, "toggle"), (1, "toggle"), (0, "toggle"), (1, "toggle"))
ACTIONS = ("replace-atomic", "replace-inplace", "replace-symlink", "reload",
           "calibrate", "toggle", "hide", "reopen")
STAGES = {"sdr_decode", "sdr_output", "hdr_not_requested", "hdr_decode", "hdr_upload",
          "hdr_presented", "hdr_unavailable"}
# First column of the D65 Display P3 to sRGB matrix. The red base is (254, 0, 0).
P3_RED_IN_SRGB = (1.2249401762, -0.0420569547, -0.0196375546)
MODIFIED = "1700000000.0"


def action_plan(seed):
    random = seed

    def next_value():
        nonlocal random
        random = (random * 1664525 + 1013904223) & 0xffffffff
        return random >> 16

    plan = [dict(slot=slot, action=action, delay_ms=0, barrier_ms=0)
            for slot, action in [(slot, action) for action in FIXED for slot in (0, 1)] + list(MIRRORED)]
    for index, entry in enumerate(plan):
        entry["barrier_ms"] = BARRIERS[index % len(BARRIERS)]
    while len(plan) < 100:
        slot = next_value() % 2
        action = ACTIONS[next_value() % len(ACTIONS)]
        delay = (next_value() % 4) * 25
        plan.append(dict(slot=slot, action=action, delay_ms=delay,
                         barrier_ms=BARRIERS[next_value() % len(BARRIERS)]))
    return plan


def linear(value):
    return ((value / 255 + 0.055) / 1.055) ** 2.4


def expected_pixels(content, mode, white, peak):
    """Independent values for the gray (192) and Display P3 red (254, 0, 0) photos."""
    if content == "gray":
        if mode == "sdr8":
            return (192 / 255,) * 3
        return (linear(192) * (peak if mode == "hdr" else white) / 80,) * 3
    if mode == "sdr8":
        return (1.0, 0.0, 0.0)  # Out-of-gamut red clips in the sRGB rendition.
    if mode == "sdr_on_hdr":
        return (white / 80, 0.0, 0.0)
    return tuple(linear(254) * peak / 80 * channel for channel in P3_RED_IN_SRGB)


def padded_contents(gray, red):
    data = [gray.read_bytes(), red.read_bytes()]
    length = max(map(len, data))
    return [item + bytes(length - len(item)) for item in data]


def validate(run, seed, gray, red, payload, output):
    report, trace = run["report"], run["trace"]
    require(run["exit_code"] == 0 and report.get("status") == "passed" and report.get("drained") is True,
            f"The replacement viewer did not complete and drain ({report.get('status')})")
    require(report.get("remaining_bytes") == dict(decode=0, retained=0, upload=0, targets=0),
            "The replacement viewer retained resource reservations")
    build = report.get("build", {})
    require(build.get("generation") == hashlib.sha256(payload.read_bytes()).hexdigest()
            and Path(build.get("executablePath", "")).resolve() == payload
            and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", "")),
            "Wrong replacement executable or missing source receipt")
    contents = padded_contents(gray, red)
    hashes = [hashlib.sha256(item).hexdigest() for item in contents]
    sources = report.get("source_fixtures", {})
    require(hashlib.sha256(gray.read_bytes()).hexdigest() == GRAY_FIXTURE_SHA256 == sources.get("gray_sha256")
            and hashlib.sha256(red.read_bytes()).hexdigest() == RED_FIXTURE_SHA256 == sources.get("red_sha256")
            and Path(sources.get("gray", "")) == gray and Path(sources.get("red", "")) == red,
            "Wrong replacement source fixtures")
    scratch = Path(report.get("fixture", ""))
    require(scratch.name == SCRATCH_NAME and scratch.is_absolute(), "Wrong scratch photo path")

    plan = action_plan(seed)
    require(report.get("seed") == seed and report.get("plan") == plan and report.get("completed_actions") == len(plan),
            "Wrong replacement action plan")
    actions = trace.get("replace_actions", [])
    require([(item.get("step"), item.get("slot"), item.get("action")) for item in actions]
            == [(i + 1, entry["slot"], entry["action"]) for i, entry in enumerate(plan)],
            "Missing, duplicated or reordered replacement actions")

    # Every replacement keeps the path, size and modification time and changes the file.
    kinds = ["initial"] + [entry["action"] for entry in plan if entry["action"].startswith("replace-")]
    generations = report.get("generations", [])
    require([item.get("kind") for item in generations] == kinds
            and [item.get("generation") for item in generations] == list(range(len(kinds))),
            "Missing or reordered file generations")
    identities = []
    for index, item in enumerate(generations):
        content = "red" if index % 2 else "gray"
        fields = str(item.get("identity", "")).split(":")
        require(item.get("content") == content and item.get("sha256") == hashes[index % 2]
                and len(fields) == 5 and fields[2] == str(len(contents[0])) and fields[3] == MODIFIED,
                f"Wrong content, size or modification time in file generation {index}")
        require(item["identity"] not in identities, f"File generation {index} repeats an earlier file")
        if index:
            same_inode = fields[:2] == identities[-1].split(":")[:2]
            require(same_inode == (item["kind"] == "replace-inplace"),
                    f"File generation {index} does not match a {item['kind']} replacement")
        identities.append(item["identity"])

    interrupted = report.get("interrupted", [])
    expected_interruptions = [(step, entry["slot"], entry["action"], entry["barrier_ms"])
                              for step, entry in enumerate(plan, 1)
                              if entry["action"].startswith("replace-") or entry["action"] == "reopen"]
    require([(item.get("step"), item.get("slot"), item.get("action"), item.get("barrier_ms"))
             for item in interrupted] == expected_interruptions
            and all(item.get("stage") in STAGES for item in interrupted),
            "Missing or unknown interrupted-revision observations")

    frames = report.get("frames", [])
    indexed = {frame.get("phase"): frame for frame in frames}
    expected_phases = {f"replace_0_{slot}_settled" for slot in (0, 1)}
    for step, entry in enumerate(plan, 1):
        expected_phases.update((f"replace_{step}_{1 - entry['slot']}_during",
                                f"replace_{step}_0_settled", f"replace_{step}_1_settled"))
    require(len(frames) == len(indexed) == len(expected_phases) and set(indexed) == expected_phases,
            "Missing, duplicate or foreign-window checkpoints")

    retirements = {int(key): line for key, line in trace.get("retirements", {}).items()}
    states = [dict(hdr=True, high=False, white=200, peak=800, loaded=0, valid=True),
              dict(hdr=True, high=False, white=100, peak=200, loaded=0, valid=True)]
    file_generation = 0
    previous, proofs, serials = {}, {}, {}
    used_tokens = set()
    classes = collections.Counter()
    for step in range(len(plan) + 1):
        entry = plan[step - 1] if step else None
        action = entry["action"] if entry else None
        action_line = actions[step - 1]["line"] if step else 0
        target = entry["slot"] if entry else None
        if entry:
            state = states[target]
            if action.startswith("replace-"):
                file_generation += 1
            if action.startswith("replace-") or action in ("reload", "reopen"):
                state["loaded"], state["valid"] = file_generation, state["hdr"]
            elif action == "toggle":
                state["hdr"] = not state["hdr"]
                state["valid"] = state["hdr"] and state["loaded"] == file_generation
            elif action == "calibrate":
                state["hdr"], state["high"] = True, not state["high"]
                state["white"] = (300 if state["high"] else 200) if target == 0 else (150 if state["high"] else 100)
                state["peak"] = state["white"] * (4 if target == 0 else 2)
                state["valid"] = state["loaded"] == file_generation
        stages = [("settled", 0), ("settled", 1)] if not step else [
            ("during", 1 - target), ("settled", 0), ("settled", 1)]
        current = {}
        for stage, slot in stages:
            if stage == "settled" and action == "hide" and slot == target:
                # Showing the window again re-derives its HDR rendition.
                states[slot]["valid"] = states[slot]["hdr"] and states[slot]["loaded"] == file_generation
            phase = f"replace_{step}_{slot}_{stage}"
            frame = indexed[phase]
            state = states[slot]
            hdr, valid, loaded = state["hdr"], state["valid"], state["loaded"]
            content = "red" if loaded % 2 else "gray"
            proof = check_viewer_phase(trace, phase, frame, scratch, hashes[loaded % 2], "linear" if hdr else "sdr")
            require(frame.get("source_generation") == loaded and frame.get("presented_identity") == identities[loaded]
                    and frame.get("file_generation") == file_generation,
                    f"The presented file is not the expected generation in {phase}")
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
            if stage == "during" and action in ("hide", "reopen"):
                retired = retirements.get(proofs[target]["lifetime"], 0)
                unmapped = any(item["lifetime"] == proofs[target]["lifetime"]
                               and action_line < item["line"] < proof["commit"]
                               for item in trace.get("unmaps", []))
                require(action_line < retired < proof["commit"] or unmapped,
                        f"The target was not absent during the peer observation in {phase}")
            if hdr:
                require(frame.get("code") == "hdr" and frame.get("app_surface_tagged") is True
                        and (frame.get("vk_format"), frame.get("vk_color_space")) == PASSTHROUGH_FP16
                        and frame.get("paper_white_nits") == state["white"] and frame.get("peak_nits") == state["peak"]
                        and math.isclose(frame.get("ui_white_scale", 0), state["white"] / 80, abs_tol=0.001),
                        f"Wrong HDR output calibration in {phase}")
                if valid:
                    require(frame.get("photo_rendition") == "hdr" and frame.get("photo_status") == "hdr"
                            and frame.get("photo_visible_hdr_frames", 0) > 0,
                            f"Missing HDR photo in {phase}")
                else:
                    # The path now names another file. Its HDR must not cover this base.
                    require(frame.get("photo_rendition") == "sdr" and frame.get("photo_status") == "unavailable"
                            and frame.get("photo_new_hdr_frames") == 0 and loaded != file_generation,
                            f"A replaced file was not held in SDR in {phase}")
            else:
                require(frame.get("code") == "disabled" and frame.get("photo_rendition") == "sdr"
                        and frame.get("photo_visible_hdr_frames") == 0 and frame.get("app_surface_tagged") is False
                        and frame.get("paper_white_nits") == 0 and frame.get("peak_nits") == 0
                        and frame.get("ui_white_scale") == 0 and is_sdr_format(frame),
                        f"Stale HDR output in {phase}")
            mode = "sdr8" if not hdr else "hdr" if valid else "sdr_on_hdr"
            classes[(content, mode, "fresh" if loaded == file_generation else "replaced")] += 1
            capture, size, dpr = frame.get("readback", {}), frame.get("pixel_size"), frame.get("device_pixel_ratio")
            logical = frame.get("logical_size")
            require(isinstance(size, list) and isinstance(logical, list) and len(size) == len(logical) == 2
                    and all(type(n) is int and n > 0 for n in size + logical)
                    and type(dpr) in (int, float) and math.isfinite(dpr) and dpr > 0
                    and size == [math.floor(n * dpr + 0.5) for n in logical]
                    and capture.get("size") == size and capture.get("revision") == frame["revision"]
                    and capture.get("frame_serial") == serial
                    and capture.get("format") == ("rgba16f" if hdr else "rgba8"),
                    f"Stale or mismatched native pixel capture in {phase}")
            samples = capture.get("samples", [])
            reference = expected_pixels(content, mode, state["white"], state["peak"])
            require(len(samples) == 3, f"Missing pixel samples in {phase}")
            for offset, sample in zip((-2, 0, 2), samples):
                rgba = sample.get("rgba", [])
                require(sample.get("x") == 4 + offset and sample.get("y") == 4
                        and len(rgba) == 4 and all(type(n) in (int, float) and math.isfinite(n) for n in rgba)
                        and all(math.isclose(n, wanted, rel_tol=0.015, abs_tol=0.012)
                                for n, wanted in zip(rgba, reference))
                        and math.isclose(rgba[3], 1, abs_tol=0.004),
                        f"Wrong visible photo pixels in {phase}: {rgba[:3]} is not the {content} {mode} photo")
            match = re.fullmatch(r"/(\d+)/.+", urlsplit(frame["current_request"]).path)
            revision = int(match[1])
            if entry and slot != target:
                for key in ("revision", "window_token", "surface_id", "current_request", "code",
                            "paper_white_nits", "peak_nits", "source_generation", "photo_status"):
                    require(frame.get(key) == previous[slot].get(key), f"Peer window changed {key} in {phase}")
                require(proof["lifetime"] == proofs[slot]["lifetime"], f"Peer native surface changed in {phase}")
            if stage == "settled":
                current[slot] = frame
                old = previous.get(slot)
                if old and entry and slot == target:
                    old_proof = proofs[slot]
                    old_revision = int(re.fullmatch(r"/(\d+)/.+", urlsplit(old["current_request"]).path)[1])
                    if action == "reopen":
                        require(token != old["window_token"] and token not in used_tokens
                                and retirements.get(old_proof["lifetime"], 0) > action_line,
                                f"Reopen reused or failed to retire its window in {phase}")
                    else:
                        require(token == old["window_token"], f"Unexpected new viewer in {phase}")
                        if action.startswith("replace-") or action == "reload":
                            require(revision > old_revision, f"No new image request after the reload in {phase}")
                        else:
                            # Only an explicit reload may move a window to the replacement.
                            require(frame["current_request"] == old["current_request"],
                                    f"The window reloaded its image without a request in {phase}")
                    if proof["lifetime"] != old_proof["lifetime"]:
                        retired = retirements.get(old_proof["lifetime"], 0)
                        require(action_line < retired < proof["lifetime"]
                                and old_proof["lifetime"] not in proof.get("live_lifetimes", []),
                                f"Unrelated replacement surface in {phase}")
                    if action in ("toggle", "calibrate"):
                        require(int(frame["revision"]) > int(old["revision"]), f"Unchanged output revision in {phase}")
                proofs[slot] = proof
        require(current[0]["window_token"] != current[1]["window_token"]
                and proofs[0]["lifetime"] != proofs[1]["lifetime"], "Both slots refer to the same window")
        previous = current
        used_tokens.update(frame["window_token"] for frame in current.values())

    # The fixed opening of the plan reaches every way of holding a replaced file.
    for content in ("gray", "red"):
        for mode, freshness in (("hdr", "fresh"), ("hdr", "replaced"), ("sdr_on_hdr", "replaced"),
                                ("sdr8", "fresh"), ("sdr8", "replaced")):
            require(classes[(content, mode, freshness)] > 0,
                    f"The journey never observed a {freshness} {content} photo as {mode}")
    return dict(checkpoints=len(frames), replacements=len(kinds) - 1,
                classes={"/".join(key): count for key, count in sorted(classes.items())},
                interrupted=dict(collections.Counter(item["stage"] for item in interrupted)),
                replacement_kinds=dict(collections.Counter(kinds[1:])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--viewer-payload", type=Path)
    parser.add_argument("--fixture", required=True, type=Path, help="Pinned gray gain-map fixture")
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--capture", required=True, type=Path)
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xffffffff:
        parser.error("seed must be unsigned 32-bit")
    args.viewer, args.fixture = args.viewer.resolve(strict=True), args.fixture.resolve(strict=True)
    red = (args.fixture.parent / "jpeg-gainmap-p3-red.jpg").resolve(strict=True)
    payload = (args.viewer_payload or args.viewer).resolve(strict=True)
    args.capture.mkdir()
    record = dict(replacement_seed=args.seed, output=args.output, status="failed", physical_hdr_verified=False)
    try:
        with tempfile.TemporaryDirectory(prefix="licasa-replacement-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="wayland", QSG_RHI_BACKEND="vulkan", QSG_RENDER_LOOP="threaded",
                       QT_QPA_PLATFORMTHEME="generic", WAYLAND_DEBUG="client", LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH="1",
                       LICASA_HDR_PAPER_WHITE_NITS="200", LICASA_HDR_PEAK_NITS="800")
            for name in ("CONFIG", "DATA", "CACHE", "STATE"):
                path = Path(directory) / name.lower()
                path.mkdir()
                env[f"XDG_{name}_HOME"] = str(path)
            run = run_child(args.capture, "viewer", [args.viewer, "--viewer", "--wayland-hdr-check", "hdr",
                "--wayland-replacement-seed", str(args.seed), args.fixture], env, record)
            record["summary"] = validate(run, args.seed, args.fixture, red, payload, args.output)
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError) as error:
        record["error"] = str(error)
    (args.capture / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"replacement: {record['status']} ({record.get('error', '')})", flush=True)
    return int(record["status"] != "passed")


if __name__ == "__main__":
    raise SystemExit(main())
