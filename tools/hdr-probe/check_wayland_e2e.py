#!/usr/bin/env python3
"""Qualify Licasa against an already-running Wayland lab session."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit

# VkFormat and VkColorSpaceKHR for extended-linear sRGB FP16.
LINEAR_FP16 = (97, 1000104002)
PASSTHROUGH_FP16 = (97, 1000104013)
SDR_FORMATS = {37, 43, 44, 50}  # RGBA/BGRA 8-bit UNORM or SRGB
SDR_COLOR_SPACES = {0, 1000104013}  # sRGB nonlinear or pass-through
EVENT = re.compile(r"(->\s+)?(\w+)[#@](\d+)\.(\w+)\((.*)\)$")
PHASE = re.compile(r"LICASA_WAYLAND_FRAME (hdr|calibrated|sdr|window_initial|window_floating|window_zoomed|window_restored|window_fullscreen|window_seed_\d+|multi_\d+_[01]_(?:during|settled)|replace_\d+_[01]_(?:during|settled)|output_\d+|motion_\d+_[a-z_]+|camera_\d+_[a-z_]+) (\d+) (\d+)$")
ACTION = re.compile(r"LICASA_WAYLAND_ACTION (\d+) (floating|zoom|zoom-out|hide|restore|fullscreen)$")
MULTI_ACTION = re.compile(r"LICASA_MULTI_ACTION (\d+) ([01]) (toggle|calibrate|hide|reopen)$")
REPLACE_ACTION = re.compile(r"LICASA_REPLACE_ACTION (\d+) ([01]) (replace-atomic|replace-inplace|replace-symlink|reload|calibrate|toggle|hide|reopen)$")
OUTPUT_ACTION = re.compile(r"LICASA_OUTPUT_ACTION (\d+) (move_hdr|move_sdr|calibrate|off_hdr|on_hdr|off_sdr|on_sdr|use_sdr)$")
WINDOW_PHASES = ("window_initial", "window_floating", "window_zoomed", "window_restored", "window_fullscreen")
WINDOW_ACTIONS = ("floating", "zoom", "hide", "restore", "fullscreen")
GRAY_FIXTURE_SHA256 = "35a2dac85923eac476f98fac12f185428e73e8f9e62be511ace127b6d3388c44"
CREATOR = "wp_image_description_creator_params_v1"
DESCRIPTION = "wp_image_description_v1"
COLOR_SURFACE = "wp_color_management_surface_v1"
SURFACE_FEEDBACK = "wp_color_management_surface_feedback_v1"


def object_id(arguments, interface):
    match = re.search(rf"\b{interface}[#@](\d+)\b", arguments)
    if not match:
        raise ValueError(f"Missing {interface} object in Wayland trace arguments {arguments!r}")
    return match[1]


def inspect_trace(path):
    creators, descriptions, color_surfaces, xdg_surfaces = {}, {}, {}, {}
    toplevels, feedback, encodings = {}, {}, {}
    pending_presentation, presentation = {}, {}
    lifetimes, last_commit, presented_at, phases = {}, {}, {}, {}
    ready, attached = set(), set()
    output_objects, outputs, entered, commit_outputs = {}, {}, {}, {}
    output_events, output_actions = [], []
    synced_outputs, presented_outputs, presentation_lines = {}, {}, {}
    linear_commits = observed_linear_commits = other_color_commits = 0
    linear_presented = sdr_presented_after_hdr = 0
    last_encoding = hdr_surface = None
    last_hdr_commit = 0
    last_has_feedback = last_commit_follows_hdr = False
    errors = []
    actions, multi_actions, unmaps, retirements, detached = [], [], [], {}, set()
    replace_actions = []
    with path.open(errors="replace") as trace:
        for index, line in enumerate(trace, 1):
            output_action = OUTPUT_ACTION.search(line.rstrip())
            if output_action:
                step, name = output_action.groups()
                output_actions.append(dict(step=int(step),action=name,line=index))
                continue
            replaced = REPLACE_ACTION.search(line.rstrip())
            if replaced:
                step, slot, name = replaced.groups()
                replace_actions.append(dict(step=int(step),slot=int(slot),action=name,line=index))
                continue
            multi = MULTI_ACTION.search(line.rstrip())
            if multi:
                step, slot, name = multi.groups()
                multi_actions.append(dict(step=int(step),slot=int(slot),action=name,line=index))
                continue
            action = ACTION.search(line.rstrip())
            if action:
                sequence, name = action.groups()
                actions.append(dict(sequence=int(sequence), action=name, line=index))
                continue
            marker = PHASE.search(line.rstrip())
            if marker:
                phase, surface, revision = marker.groups()
                lifetime = lifetimes.get(surface)
                commit = last_commit.get(lifetime)
                if phase in phases or not commit:
                    errors.append(f"Invalid or duplicate {phase} viewer frame marker")
                else:
                    phases[phase] = dict(surface_id=int(surface), revision=revision,
                                         marker_line=index,
                                         lifetime=lifetime, commit=commit[0], encoding=commit[1], observed=commit[2],
                                         entered_outputs=commit_outputs.get((lifetime,commit[0]), []),
                                         live_lifetimes=sorted(lifetimes.values()),
                                         hdr_role_live=phases.get("hdr", {}).get("lifetime") in lifetimes.values())
                continue
            match = EVENT.search(line.rstrip())
            if not match:
                continue
            request, interface, obj, action, args = match.groups()
            if not request:
                if interface == "wl_output" and action == "name" and obj in output_objects:
                    output = outputs[output_objects[obj]]
                    output["name"] = args.strip('"')
                    output_events.append(dict(output,kind="add",line=index))
                elif interface == "wl_registry" and action == "global_remove":
                    for output in outputs.values():
                        if output["global"] == int(args) and not output["removed"]:
                            output["removed"] = index
                            output_events.append(dict(output,kind="remove",line=index))
                elif interface == "wl_surface" and action in ("enter","leave"):
                    binding = output_objects.get(object_id(args,"wl_output")) if args != "nil" else None
                    if args == "nil" and action == "enter":
                        errors.append("Unknown native output in surface enter")
                    if binding is not None:
                        members = entered.setdefault(obj,set())
                        members.add(binding) if action == "enter" else members.discard(binding)
                elif interface == "wp_presentation_feedback" and action == "sync_output":
                    # A queued receipt can name an output whose proxy Qt has
                    # already released after global_remove. It proves no live output.
                    binding = output_objects.get(object_id(args,"wl_output")) if args != "nil" else None
                    if binding is not None:
                        synced_outputs.setdefault(obj, {})[binding] = dict(outputs[binding])
                if interface == "wl_display" and action == "error":
                    errors.append(line.strip())
                elif interface == DESCRIPTION and action in {"ready", "ready2"}:
                    ready.add(obj)
                elif interface == "wp_presentation_feedback" and action in {"presented", "discarded"}:
                    event = presentation.pop(obj, None)
                    synced = synced_outputs.pop(obj, {})
                    if action == "presented" and event:
                        encoding, observed, follows_hdr, lifetime, commit = event
                        linear_presented += int(encoding == "linear" and observed)
                        sdr_presented_after_hdr += int(follows_hdr and linear_presented > 0)
                        presented_at.setdefault((lifetime, encoding), set()).add(commit)
                        presented_outputs[(lifetime,commit)] = list(synced.values())
                        presentation_lines[(lifetime,commit)] = index
                continue

            message = (interface, action)
            if message == ("wl_registry", "bind") and '"wl_output"' in args:
                binding = re.search(r"new id (?:\[unknown\]|wl_output)[#@](\d+)",args)
                if not binding:
                    errors.append("Missing native output binding identity")
                else:
                    output_objects[binding[1]] = index
                    outputs[index] = {"global":int(args.split(",")[0]),"binding":index,"name":"","removed":0}
            elif message == ("xdg_wm_base", "get_xdg_surface"):
                xdg_surfaces[object_id(args, "xdg_surface")] = object_id(args, "wl_surface")
            elif message == ("xdg_surface", "get_toplevel"):
                surface = xdg_surfaces[obj]
                toplevels[object_id(args, "xdg_toplevel")] = surface
                lifetimes[surface] = index
            elif message == ("xdg_toplevel", "destroy"):
                surface = toplevels.pop(obj, None)
                lifetime = lifetimes.pop(surface, None)
                if lifetime is not None:
                    retirements[lifetime] = index
                if surface == hdr_surface:
                    hdr_surface = None
            elif message == ("xdg_surface", "destroy"):
                xdg_surfaces.pop(obj, None)
            elif message == ("wp_color_manager_v1", "get_surface_feedback"):
                feedback[object_id(args, SURFACE_FEEDBACK)] = object_id(args, "wl_surface")
            elif message == ("wp_presentation", "feedback"):
                surface = object_id(args, "wl_surface")
                pending_presentation.setdefault(surface, []).append(object_id(args, "wp_presentation_feedback"))
            elif message == (SURFACE_FEEDBACK, "destroy"):
                feedback.pop(obj, None)
            elif message == ("wp_color_manager_v1", "get_surface"):
                surface = object_id(args, "wl_surface")
                if surface in color_surfaces.values():
                    errors.append(f"Duplicate live color-management object for wl_surface#{surface}")
                color_surfaces[object_id(args, COLOR_SURFACE)] = surface
            elif interface == CREATOR and action in {"set_primaries_named", "set_tf_named"}:
                creators.setdefault(obj, {})[action] = args
            elif message == (CREATOR, "create"):
                values = creators.pop(obj, {})
                # Named sRGB primaries are 1; the transfer is extended-linear (5) or sRGB (9).
                encoding = {"5": "linear", "9": "sdr"}.get(values.get("set_tf_named"), "other")
                descriptions[object_id(args, DESCRIPTION)] = (
                    encoding if values.get("set_primaries_named") == "1" else "other")
            elif message == ("wp_color_manager_v1", "create_windows_scrgb"):
                descriptions[object_id(args, DESCRIPTION)] = "linear"
            elif message == (DESCRIPTION, "destroy"):
                descriptions.pop(obj, None)
                ready.discard(obj)
            elif message == (COLOR_SURFACE, "set_image_description") and obj in color_surfaces:
                desc = object_id(args, DESCRIPTION)
                encodings[color_surfaces[obj]] = descriptions.get(desc, "other") if desc in ready else "other"
            elif interface == COLOR_SURFACE and action in {"unset_image_description", "destroy"}:
                surface = color_surfaces.get(obj)
                encodings.pop(surface, None)
                if action == "destroy":
                    color_surfaces.pop(obj, None)
            elif message == ("wl_surface", "attach"):
                if args.startswith(("wl_buffer#", "wl_buffer@")):
                    attached.add(obj)
                    detached.discard(obj)
                else:
                    attached.discard(obj)
                    detached.add(obj)
            elif message == ("wl_surface", "commit"):
                pending = pending_presentation.pop(obj, ())
                if obj in detached and obj in lifetimes:
                    unmaps.append(dict(lifetime=lifetimes[obj], line=index))
                detached.discard(obj)
                if obj in attached and obj in toplevels.values():
                    last_encoding = encodings.get(obj, "sdr")
                    last_has_feedback = obj in feedback.values()
                    lifetime = lifetimes[obj]
                    last_commit[lifetime] = (index, last_encoding, last_has_feedback)
                    commit_outputs[(lifetime,index)] = [dict(outputs[binding]) for binding in
                        sorted(entered.get(obj,set())) if not outputs[binding]["removed"]]
                    last_commit_follows_hdr = (last_encoding == "sdr" and observed_linear_commits > 0
                                               and (hdr_surface == obj or
                                                    (hdr_surface is None and lifetime > last_hdr_commit)))
                    if last_encoding == "linear":
                        linear_commits += 1
                        if last_has_feedback:
                            observed_linear_commits += 1
                            hdr_surface = obj
                            last_hdr_commit = index
                    elif last_encoding == "other":
                        other_color_commits += 1
                    for feedback_id in pending:
                        presentation[feedback_id] = (last_encoding, last_has_feedback, last_commit_follows_hdr,
                                                     lifetime, index)
                attached.discard(obj)
            elif message == ("wl_surface", "destroy"):
                entered.pop(obj,None)
                attached.discard(obj)
                detached.discard(obj)
                lifetime = lifetimes.pop(obj, None)
                if lifetime is not None:
                    retirements[lifetime] = index
                pending_presentation.pop(obj, None)
                encodings.pop(obj, None)
                if hdr_surface == obj:
                    hdr_surface = None
                feedback = {key: value for key, value in feedback.items() if value != obj}
                toplevels = {key: value for key, value in toplevels.items() if value != obj}
                color_surfaces = {key: value for key, value in color_surfaces.items() if value != obj}
    for name, phase in phases.items():
        phase["presented_commit"] = max((commit for commit in
            presented_at.get((phase["lifetime"], phase["encoding"]), ()) if commit <= phase["commit"]
            and (not name.startswith("output_") or
                 presentation_lines[(phase["lifetime"],commit)] < phase["marker_line"])), default=0)
        phase["presented_on"] = presented_outputs.get((phase["lifetime"],phase["presented_commit"]), [])
        phase["presented_entered_outputs"] = commit_outputs.get((phase["lifetime"],phase["presented_commit"]), [])
    return {"phases": phases, "actions": actions, "multi_actions": multi_actions,
            "replace_actions": replace_actions,
            "output_actions": output_actions, "output_events": output_events,
            "unmaps": unmaps, "retirements": retirements,
            "linear_commits": linear_commits, "linear_commits_with_feedback": observed_linear_commits,
            "linear_presented": linear_presented, "sdr_presented_after_hdr": sdr_presented_after_hdr,
            "other_color_commits": other_color_commits, "last_toplevel_encoding": last_encoding,
            "last_toplevel_has_feedback": last_has_feedback,
            "last_toplevel_commit_follows_hdr": last_commit_follows_hdr, "errors": errors}


def run_child(directory, name, command, env, record):
    stdout_path = directory / f"{name}.json"
    trace_path = directory / f"{name}.wayland.log"
    record[name] = run = {"command": [str(part) for part in command]}
    with stdout_path.open("wb") as stdout, trace_path.open("wb") as stderr:
        try:
            run["exit_code"] = subprocess.run(command, env=env, stdout=stdout, stderr=stderr,
                                              timeout=600 + 120 * record["soak_rounds"] if record.get("soak_rounds") else
                                              900 if record.get("motion_seed") is not None or record.get("camera_seed") is not None else
                                              320 * max(1, record.get("multiwindow_actions", 100) // 100)
                                              if record.get("multiwindow_seed") is not None else
                                              320 * max(1, record.get("output_cycles", 8) // 8)
                                              if record.get("output_seed") is not None else
                                              320 if record.get("replacement_seed") is not None else
                                              200 if record.get("window_seed") is not None else 40,
                                              check=False).returncode
        except subprocess.TimeoutExpired:
            raise ValueError(f"{name} timed out") from None
    run["trace"] = inspect_trace(trace_path)
    require(not run["trace"]["errors"], f"{name} reported Wayland protocol errors")
    try:
        run["report"] = json.loads(stdout_path.read_text())
    except ValueError:
        raise ValueError(f"{name} did not produce a single JSON receipt (exit {run['exit_code']})") from None
    require(run["report"]["platform"].startswith("wayland"), f"{name} did not use native Wayland")
    return run


def require(condition, message):
    if not condition:
        raise ValueError(message)


def is_sdr_format(frame):
    return frame.get("vk_format") in SDR_FORMATS and frame.get("vk_color_space") in SDR_COLOR_SPACES


def check_viewer_phase(trace, phase, frame, fixture, fixture_hash, encoding=None):
    proof = trace.get("phases", {}).get(phase, {})
    encoding = encoding or ("sdr" if phase == "sdr" else "linear")
    request = frame.get("current_request", "")
    match = re.fullmatch(r"/\d+/(.+)", urlsplit(request).path)
    require(frame.get("fixture_sha256") == fixture_hash and request.startswith("image://licasa/")
            and match and Path(unquote(match[1])) == fixture,
            f"The {phase} frame is missing the current fixture request")
    require(proof and frame.get("surface_id") == proof["surface_id"]
            and frame.get("revision") == proof["revision"] and proof["encoding"] == encoding
            and (encoding == "sdr" or proof["observed"]),
            f"The {phase} frame is missing its matching toplevel commit")
    require(frame.get("photo_label") and frame.get("display_label"),
            f"The {phase} frame is missing the photo or display label")
    return proof


def seeded_window_plan(seed):
    random = seed

    def next_value():
        nonlocal random
        random = (random * 1664525 + 1013904223) & 0xffffffff
        return random >> 16

    plan = []

    def append(action):
        plan.append(dict(action=action, delay_ms=(next_value() % 4) * 25))

    for action in WINDOW_ACTIONS:
        append(action)
    fullscreen, zoom = True, 1
    while len(plan) < 100:
        choice = next_value() % 4
        if choice == 0:
            fullscreen = not fullscreen
            append("fullscreen" if fullscreen else "floating")
        elif choice == 1 or len(plan) == 99:
            append("zoom" if zoom < 2 else "zoom-out")
            zoom += 1 if zoom < 2 else -1
        elif choice == 2:
            append("zoom-out" if zoom > 0 else "zoom")
            zoom += -1 if zoom > 0 else 1
        else:
            append("hide")
            append("restore")
    return plan


def check_window_journey(args, output, trace, fixture_hash, hdr, app_tag, expected_pair):
    frames = output.get("window_frames", [])
    actions = trace.get("actions", [])
    seed = getattr(args, "window_seed", None)
    seeded = seed is not None
    plan = seeded_window_plan(seed) if seeded else [dict(action=name) for name in WINDOW_ACTIONS]
    names = [item["action"] for item in plan]
    checkpoints = [None] + [index for index, name in enumerate(names) if name != "hide"]
    phases = [f"window_seed_{i}" for i in range(len(checkpoints))] if seeded else list(WINDOW_PHASES)
    require(not seeded or (output.get("window_seed") == str(seed) and output.get("window_plan") == plan
                           and output.get("hidden_count") == names.count("hide")),
            "Wrong seeded window journey plan or hide count")
    require(isinstance(frames, list) and [frame.get("phase") for frame in frames] == phases
            and output.get("window_actions") == names
            and [item.get("action") for item in actions] == names
            and [item.get("sequence") for item in actions] == list(range(1, len(plan) + 1))
            and (seeded or output.get("hidden_confirmed") is True),
            "Incomplete or reordered window journey")
    require(fixture_hash == GRAY_FIXTURE_SHA256, "The window journey requires the pinned gray gain-map fixture")
    token = frames[0].get("window_token")
    require(isinstance(token, str) and re.fullmatch(r"[a-f0-9-]{36}", token)
            and output["last_frame"].get("window_token") == token, "The window journey changed viewer identity")
    preceding = None
    expected_actions = [None if index is None else actions[index] for index in checkpoints]
    previous_serial = 0
    fullscreen = True
    for index, (frame, action) in enumerate(zip(frames, expected_actions)):
        phase = phases[index]
        proof = check_viewer_phase(trace, phase, frame, args.fixture, fixture_hash, "linear" if hdr else "sdr")
        size, pixels, dpr = frame.get("logical_size"), frame.get("pixel_size"), frame.get("device_pixel_ratio")
        require(isinstance(size, list) and isinstance(pixels, list) and len(size) == len(pixels) == 2
                and all(type(value) is int and value > 0 for value in size + pixels)
                and type(dpr) in (int, float) and math.isfinite(dpr) and dpr > 0
                and pixels == [math.floor(value * dpr + 0.5) for value in size],
                f"Invalid window journey geometry in {phase}")
        expected_dpr = getattr(args, "expected_dpr", None)
        require(expected_dpr is None or math.isclose(dpr, expected_dpr, abs_tol=0.001),
                f"Wrong window journey scale in {phase}")
        if action and action["action"] in ("floating", "fullscreen"):
            fullscreen = action["action"] == "fullscreen"
        serial = frame.get("frame_serial")
        require(frame.get("window_token") == token and frame.get("fullscreen") is fullscreen
                and frame.get("visibility") == (5 if fullscreen else 2)
                and type(serial) is int and serial > previous_serial and frame.get("image_ready")
                and frame.get("output") == args.output
                and frame.get("photo_rendition") == ("hdr" if hdr else "sdr"),
                f"Wrong viewer state in window journey {phase}")
        previous_serial = serial
        if hdr:
            require(frame.get("code") == "hdr" and frame.get("photo_visible_hdr_frames", 0) > 0
                    and (frame.get("vk_format"), frame.get("vk_color_space")) == expected_pair
                    and bool(frame.get("app_surface_tagged")) == app_tag
                    and frame.get("paper_white_nits") == 200 and frame.get("peak_nits") == 800
                    and math.isclose(frame.get("ui_white_scale", 0), 2.5, abs_tol=0.001),
                    f"Missing HDR window journey frame in {phase}")
        else:
            require(is_sdr_format(frame) and frame.get("ui_white_scale") == 0
                    and frame.get("photo_visible_hdr_frames") == 0 and not frame.get("app_surface_tagged")
                    and frame.get("code") == output["last_frame"]["code"],
                    f"Missing SDR window journey frame in {phase}")
        after_action = action["line"] if action else 0
        if seeded and index:
            previous = frames[index - 1]
            if action["action"] in ("zoom", "zoom-out"):
                before, after = previous.get("photo_scale"), frame.get("photo_scale")
                require(type(before) in (int, float) and type(after) in (int, float)
                        and math.isfinite(before) and math.isfinite(after) and before > 0 and after > 0,
                        f"Missing zoom scale in {phase}")
                factor = 1.15 if action["action"] == "zoom" else 1 / 1.15
                # This pinned 32-pixel fixture fits below the window's minimum
                # screen fraction, so floating zoom stops at native size.
                expected_scale = max(0.02 if fullscreen else 1, before * factor)
                require(math.isclose(after, expected_scale, rel_tol=0.001),
                        f"Wrong zoom change in {phase}")
            if action["action"] == "restore":
                hidden = actions[checkpoints[index] - 1]
                lifetime = preceding["lifetime"]
                retirement = trace.get("retirements", {}).get(lifetime,
                    trace.get("retirements", {}).get(str(lifetime), 0))
                require(hidden["action"] == "hide" and
                        (hidden["line"] < retirement < action["line"]
                         or any(item.get("lifetime") == lifetime
                                and hidden["line"] < item.get("line", 0) < action["line"]
                                for item in trace.get("unmaps", []))),
                        f"No native unmap during seeded hide in {phase}")
        require(proof["commit"] > after_action and proof["presented_commit"] > after_action,
                f"No presentation after the window journey action in {phase}")
        if preceding:
            require(proof["commit"] > preceding["commit"]
                    and (proof["lifetime"] == preceding["lifetime"]
                         or (preceding["lifetime"] not in proof.get("live_lifetimes", [])
                             and proof["lifetime"] > preceding["commit"])),
                    f"Unrelated or reordered window journey surface in {phase}")
        preceding = proof
        readback = frame.get("readback", {})
        samples = readback.get("samples", [])
        require(readback.get("size") == pixels and readback.get("format") == ("rgba16f" if hdr else "rgba8")
                and readback.get("frame_serial") == serial and readback.get("revision") == frame["revision"]
                and isinstance(samples, list) and len(samples) == 3,
                f"Missing window journey pixels in {phase}")
        gray = ((192 / 255 + 0.055) / 1.055) ** 2.4 * 4 * 2.5 if hdr else 192 / 255
        scale = frame.get("photo_scale")
        require(not seeded or (type(scale) in (int, float) and math.isfinite(scale) and scale > 0),
                f"Missing seeded photo scale in {phase}")
        for offset, sample in zip((-2, 0, 2), samples):
            channels = sample.get("rgba", [])
            x, y = (pixels[0] // 2, pixels[1] // 2) if fullscreen else (4, 4)
            if seeded and fullscreen:
                half_cell = math.floor(scale * dpr / 2 + 0.5)
                x, y = x - half_cell, y - half_cell
            require(sample.get("x") == x + offset and sample.get("y") == y
                    and len(channels) == 4 and all(type(value) in (int, float) and math.isfinite(value) for value in channels)
                    and all(math.isclose(value, gray, rel_tol=0.015, abs_tol=0.012) for value in channels[:3])
                    and math.isclose(channels[3], 1, abs_tol=0.004),
                    f"Wrong window journey pixels in {phase}")
    initial, floating, zoomed, restored, fullscreen = frames[:5]
    discarded = output.get("discarded_capture_serial")
    require(type(discarded) is int and initial["frame_serial"] < discarded < floating["frame_serial"],
            "The window journey did not replace its discarded capture")
    require(floating["logical_size"] != initial["logical_size"]
            and all(after > before for before, after in zip(floating["logical_size"], zoomed["logical_size"]))
            and restored["logical_size"] == zoomed["logical_size"]
            and fullscreen["logical_size"] == initial["logical_size"], "The window journey did not change and restore geometry")
    hidden_lifetime = trace["phases"][phases[2]]["lifetime"]
    retirement = trace.get("retirements", {}).get(hidden_lifetime,
        trace.get("retirements", {}).get(str(hidden_lifetime), 0))
    require(actions[2]["line"] < retirement < actions[3]["line"]
            or any(item.get("lifetime") == hidden_lifetime and actions[2]["line"] < item.get("line", 0) < actions[3]["line"]
                   for item in trace.get("unmaps", [])), "No native unmap during the window journey hide")
    final_proof = trace["phases"]["hdr" if hdr else "sdr"]
    require(final_proof["commit"] > preceding["commit"], "The window journey ended before its final presentation")


def check(args, record, env):
    app_tag = args.expect == "hdr-app-tag"
    hdr = args.expect == "hdr" or app_tag
    passthrough = args.expect == "hdr-passthrough"
    hdr_preferred = hdr or passthrough
    calibrate = getattr(args, "calibrate", False)
    window_churn = getattr(args, "window_churn", False)
    window_seed = getattr(args, "window_seed", None)
    require(window_seed is None or (window_churn and 0 <= window_seed <= 0xffffffff),
            "A window seed requires window churn and an unsigned 32-bit seed")
    require(not calibrate or hdr, "Calibration requires a confirmed HDR run")
    expected_reason = "hdr_preferred" if hdr_preferred else {
        "sdr": "sdr_preferred", "no-color-manager": "no_color_manager"}[args.expect]
    expected_pair = PASSTHROUGH_FP16 if app_tag else LINEAR_FP16
    cap = run_child(args.capture, "capabilities", [args.probe, "--wayland-capabilities"], env, record)
    info = cap["report"]
    native = info.get("native_surface", {})
    require(cap["exit_code"] == (0 if hdr_preferred else 77) and info["reason"] == expected_reason
            and info["status"] == ("capabilities_ready" if hdr_preferred else "sdr_fallback"),
            f"Expected {expected_reason}, got {info.get('reason')} (exit {cap['exit_code']})")
    require(info["color_manager"] == (args.expect != "no-color-manager") and info["surface_attached"]
            and native.get("screen") == args.output and native.get("device"),
            "The capability probe did not reach the expected native output")
    require(is_sdr_format(native)
            and info["sdr_frame_captured"] and math.isclose(info["first_patch_red"], 0.18, abs_tol=0.02)
            and cap["trace"]["last_toplevel_encoding"] == "sdr" and cap["trace"]["linear_commits"] == 0
            and cap["trace"]["other_color_commits"] == 0,
            "The SDR control did not capture the expected Vulkan patch and buffer commit")
    if args.expect != "no-color-manager":
        require(info["description_accepted"] and info["preferred_identity"] > 0
                and info["preferred_fingerprint"] and info["reference_nits"] > 0
                and info["target_nits"] > 0
                and (info["target_nits"] > info["reference_nits"]) == hdr_preferred,
                "Incomplete or inconsistent preferred luminance metadata")
    if hdr_preferred:
        surface = run_child(args.capture, "surface", [args.probe, "--surface-check"], env, record)
        receipt = surface["report"]
    if hdr:
        require(surface["exit_code"] == 0 and receipt["status"] == "linear_fp16_frame_submitted"
                and receipt["screen"] == args.output and receipt["preferred_fingerprint"] == info["preferred_fingerprint"]
                and (receipt["vk_format"], receipt["vk_color_space"]) == expected_pair
                and bool(receipt.get("app_surface_tagged")) == app_tag,
                "No confirmed extended-linear sRGB FP16 frame on the expected output")
        require(surface["trace"]["last_toplevel_has_feedback"]
                and surface["trace"]["last_toplevel_encoding"] == "linear",
                "No accepted linear image description committed with a buffer on the observed probe surface")
        if app_tag:
            require(surface["trace"]["linear_presented"] > 0, "The compositor did not present a tagged probe buffer")
    if passthrough:
        require(surface["exit_code"] == 77 and receipt["status"] == "sdr_fallback"
                and receipt["reason"] == "unverified_passthrough_color_space"
                and receipt["screen"] == args.output and receipt["preferred_fingerprint"] == info["preferred_fingerprint"]
                and (receipt["vk_format"], receipt["vk_color_space"]) == PASSTHROUGH_FP16
                and surface["trace"]["linear_commits"] == 0,
                "The probe did not refuse unverified FP16 pass-through on the expected output")

    viewer_mode, final_code = "sdr", "wayland-sdr"
    if hdr:
        viewer_mode, final_code = "hdr-calibration" if calibrate else "hdr", "disabled"
    elif passthrough:
        viewer_mode, final_code = "unsupported", "unsupported"
    viewer = run_child(args.capture, "viewer", [args.viewer, "--viewer", "--wayland-hdr-check",
                                                viewer_mode, *(["--wayland-window-churn"] if window_churn else []),
                                                *(["--wayland-window-seed", str(window_seed)] if window_seed is not None else []),
                                                args.fixture], env, record)
    output = viewer["report"]
    fixture_hash = hashlib.sha256(args.fixture.read_bytes()).hexdigest()
    payload = args.viewer_payload or args.viewer
    payload_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
    build = output.get("build", {})
    require(output.get("fixture") == str(args.fixture) and output.get("fixture_sha256") == fixture_hash,
            "The viewer reported a different fixture")
    require(build.get("generation") == payload_hash and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", ""))
            and Path(build.get("executablePath", "")).resolve() == payload.resolve(),
            "The viewer reported a different executable or missing source receipt")
    final = output.get("last_frame", {})
    require(viewer["exit_code"] == 0 and output["status"] == "passed"
            and final["code"] == final_code
            and final["image_ready"] and final["output"] == args.output and final["ui_white_scale"] == 0
            and not final.get("app_surface_tagged")
            and final.get("photo_visible_hdr_frames") == 0 and final.get("photo_rendition") == "sdr"
            and viewer["trace"]["last_toplevel_encoding"] == "sdr",
            "The production viewer did not finish on the expected SDR image and output")
    final_format = (final.get("vk_format"), final.get("vk_color_space"))
    require(is_sdr_format(final), f"The final viewer frame did not use an SDR Vulkan pair (got {final_format})")
    final_proof = check_viewer_phase(viewer["trace"], "sdr", final, args.fixture, fixture_hash)
    if window_churn:
        check_window_journey(args, output, viewer["trace"], fixture_hash, hdr, app_tag, expected_pair)
    if hdr:
        require(output.get("used_sdr_control") is True, "The viewer did not use the SDR control")
        active = output["hdr_frame"]
        active_proof = check_viewer_phase(viewer["trace"], "hdr", active, args.fixture, fixture_hash)
        active_format = (active.get("vk_format"), active.get("vk_color_space"))
        require(active_format == expected_pair and bool(active.get("app_surface_tagged")) == app_tag,
                f"The HDR viewer frame did not use linear FP16 (got {active_format})")
        require(active["code"] == "hdr" and active["output"] == args.output and active["image_ready"]
                and active["photo_visible_hdr_frames"] > 0 and active.get("photo_rendition") == "hdr"
                and active["paper_white_nits"] == 200
                and active["peak_nits"] == 800 and math.isclose(active["ui_white_scale"], 2.5, abs_tol=0.001)
                and active["revision"] != final["revision"] and viewer["trace"]["linear_commits_with_feedback"] > 0,
                "The production viewer did not present the HDR photo at the requested paper-white scale")
        require(viewer["trace"]["last_toplevel_commit_follows_hdr"]
                and final_proof["commit"] > active_proof["commit"]
                and (final_proof["lifetime"] == active_proof["lifetime"]
                     or (not final_proof["hdr_role_live"]
                         and final_proof["lifetime"] > active_proof["commit"])),
                "No SDR buffer commit followed the same HDR window or its replacement")
        if app_tag:
            require(viewer["trace"]["sdr_presented_after_hdr"] > 0
                    and active_proof["presented_commit"] > 0
                    and final_proof["presented_commit"] > active_proof["presented_commit"],
                    "The compositor did not present both phases of the same viewer window")
        if calibrate:
            calibrated = output.get("calibration_frame", {})
            proof = check_viewer_phase(viewer["trace"], "calibrated", calibrated, args.fixture, fixture_hash)
            require(output.get("used_calibration_controls") is True
                    and calibrated.get("code") == "hdr" and calibrated.get("output") == args.output
                    and calibrated.get("image_ready") and calibrated.get("photo_rendition") == "hdr"
                    and calibrated.get("photo_visible_hdr_frames", 0) > 0
                    and (calibrated.get("vk_format"), calibrated.get("vk_color_space")) == expected_pair
                    and bool(calibrated.get("app_surface_tagged")) == app_tag
                    and calibrated.get("paper_white_nits") == 300 and calibrated.get("peak_nits") == 1200
                    and math.isclose(calibrated.get("ui_white_scale", 0), 3.75, abs_tol=0.001),
                    "The calibration controls did not produce the requested HDR frame")
            require(calibrated["revision"] not in {active["revision"], final["revision"]}
                    and proof["lifetime"] == active_proof["lifetime"]
                    and active_proof["commit"] < proof["commit"] < final_proof["commit"],
                    "The calibration frame is stale, reordered or from a different window")
            if app_tag:
                require(active_proof["presented_commit"] < proof["presented_commit"]
                        < final_proof["presented_commit"],
                        "The compositor did not present the calibration frame between HDR and SDR")
    else:
        require(final["photo_visible_hdr_frames"] == 0 and viewer["trace"]["linear_commits"] == 0
                and viewer["trace"]["other_color_commits"] == 0,
                "The SDR run reported an HDR photo frame or a non-SDR buffer commit")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect", required=True, choices=("hdr", "hdr-app-tag", "hdr-passthrough", "sdr", "no-color-manager"))
    parser.add_argument("--output", required=True, help="Expected Qt screen name")
    parser.add_argument("--compositor", required=True, help="Record compositor version and output mode")
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--viewer", required=True, type=Path, help="HDR opt-in licasa_diagnostics executable")
    parser.add_argument("--viewer-payload", type=Path, help="Actual diagnostics executable when using a launcher")
    parser.add_argument("--fixture", required=True, type=Path, help="Known JPEG gain-map fixture")
    parser.add_argument("--calibrate", action="store_true", help="Exercise session calibration controls before Use SDR")
    parser.add_argument("--window-churn", action="store_true", help="Exercise fullscreen, floating zoom and hide/restore")
    parser.add_argument("--window-seed", type=int, help="Replay 100 window actions with this unsigned 32-bit seed")
    parser.add_argument("--expected-dpr", type=float, help="Require this device-pixel ratio at each window checkpoint")
    parser.add_argument("--render-loop", choices=("basic", "threaded"), default="threaded")
    parser.add_argument("--capture", required=True, type=Path, help="New directory for raw logs and result")
    args = parser.parse_args()
    for name in ("probe", "viewer", "fixture"):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    if args.viewer_payload:
        args.viewer_payload = args.viewer_payload.resolve(strict=True)
    args.capture.mkdir()
    record = {"expect": args.expect, "calibrate": args.calibrate, "window_churn": args.window_churn,
              "window_seed": args.window_seed,
              "expected_dpr": args.expected_dpr, "output": args.output, "compositor": args.compositor,
              "render_loop": args.render_loop,
              "status": "failed", "physical_hdr_verified": False, "inputs": {}}
    try:
        for path in {args.probe, args.viewer, args.viewer_payload or args.viewer, args.fixture}:
            with path.open("rb") as source:
                record["inputs"][str(path)] = hashlib.file_digest(source, "sha256").hexdigest()
        env = dict(os.environ, QT_QPA_PLATFORM="wayland", QT_QPA_PLATFORMTHEME="generic",
                   QSG_RENDER_LOOP=args.render_loop, QSG_RHI_BACKEND="vulkan", WAYLAND_DEBUG="client",
                   LICASA_HDR_PAPER_WHITE_NITS="200", LICASA_HDR_PEAK_NITS="800")
        display = env.get("WAYLAND_DISPLAY")
        runtime = env.get("XDG_RUNTIME_DIR")
        require(display and runtime and (Path(runtime) / display).is_socket(),
                "Select an existing Wayland socket with WAYLAND_DISPLAY and XDG_RUNTIME_DIR")
        for key in ("WAYLAND_SOCKET", "QT_QUICK_BACKEND", "QSG_RHI_HDR"):
            env.pop(key, None)
        tag_key = "LICASA_HDR_WAYLAND_TAGGED_PASSTHROUGH"
        if args.expect == "hdr-app-tag":
            env[tag_key] = "1"
        else:
            env.pop(tag_key, None)
        record["runtime"] = {key: env[key] for key in (
            "WAYLAND_DISPLAY", "QT_PLUGIN_PATH", "QML_IMPORT_PATH", "LD_LIBRARY_PATH",
            "VK_DRIVER_FILES", "VK_ICD_FILENAMES", tag_key) if key in env}
        with tempfile.TemporaryDirectory(prefix="licasa-wayland-e2e-") as isolated:
            for name in ("CONFIG", "DATA", "CACHE", "STATE"):
                folder = Path(isolated) / name.lower()
                folder.mkdir()
                env[f"XDG_{name}_HOME"] = str(folder)
            check(args, record, env)
        record["status"] = "passed"
    except (OSError, ValueError, KeyError, TypeError) as error:
        record["error"] = str(error)
        print(f"Wayland HDR check failed: {error}", file=sys.stderr)
    finally:
        (args.capture / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"{args.expect}: {record['status']} ({args.capture / 'result.json'})")
    return 0 if record["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
