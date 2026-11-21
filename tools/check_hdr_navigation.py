#!/usr/bin/env python3
"""Run replayable source churn against the production viewer in SDR."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import unquote, urlsplit


FIXTURE_SHA256 = "35a2dac85923eac476f98fac12f185428e73e8f9e62be511ace127b6d3388c44"
PPM_HEADER = b"P6\n1600 1200\n255\n"
REPLACEMENT_COLORS = ("#cf1c45", "#2b3ddb")


def ppm_hash(color):
    return hashlib.sha256(PPM_HEADER + bytes.fromhex(color[1:]) * (1600 * 1200)).hexdigest()


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def validate(receipt, seed, viewer, viewer_hash, fixture, fixture_bytes):
    fixture_hash = hashlib.sha256(fixture_bytes).hexdigest()
    require(fixture_hash == FIXTURE_SHA256 and receipt.get("fixture_sha256") == fixture_hash,
            "unexpected navigation fixture")
    build = receipt.get("build", {})
    require(build.get("generation") == viewer_hash and re.fullmatch(r"[a-f0-9]{64}", build.get("sourceReceipt", ""))
            and Path(build.get("executablePath", "")).resolve() == viewer,
            "wrong diagnostics build")
    require(receipt.get("status") == "passed" and receipt.get("seed") == seed,
            "failed or wrong seed")
    window = receipt.get("window_id", "")
    require(isinstance(window, str) and re.fullmatch(r"[a-f0-9-]{36}", window), "missing viewer identity")
    actions = iter(receipt["actions"])

    def take(action, image=None):
        item = next(actions, {})
        require(item.get("action") == action and (image is None or item.get("image") == image),
                f"expected {action} {image}, got {item}")
        return item

    last_revision = -1
    last_request = ""
    scratch_root = None

    def checkpoint(image, color, digest):
        nonlocal last_revision, last_request, scratch_root
        event = take("verified", image)
        require(event.get("window_id") == window, "checkpoint belongs to a different window")
        request = event.get("request", "")
        match = re.fullmatch(r"/(\d+)/(.+)", urlsplit(request).path)
        require(request.startswith("image://licasa/") and match, "invalid provider request")
        path = Path(unquote(match[2]))
        if image == fixture.name:
            require(path == fixture, "checkpoint used a different original")
        else:
            require(path.is_absolute() and path.name == image
                    and (scratch_root is None or path.parent == scratch_root),
                    "checkpoint used a different scratch source")
            scratch_root = path.parent
        require(int(match[1]) > last_revision, "stale or duplicate request revision")
        last_revision = int(match[1])
        last_request = request
        actual_hash = event.get("source_sha256", "")
        require(re.fullmatch(r"[a-f0-9]{64}", actual_hash) and
                (actual_hash == digest if digest else actual_hash != fixture_hash),
                "checkpoint used unexpected source bytes")
        frames = event.get("frames", [])
        require(len(frames) == 3 and all(len(frame) == 3 for frame in frames), "incomplete pixel samples")
        target = bytes.fromhex(color[1:])
        for frame in frames:
            for sample in frame:
                require(isinstance(sample, str) and re.fullmatch(r"#[a-f0-9]{6}", sample),
                        "malformed pixel")
                require(all(abs(a - b) <= 3 for a, b in zip(bytes.fromhex(sample[1:]), target)),
                        "checkpoint pixels differ from the expected source")

    checkpoint(fixture.name, "#c0c0c0", fixture_hash)
    replacement = "replaceable.ppm"
    color = REPLACEMENT_COLORS[0]
    broken_hash = hashlib.sha256(fixture_bytes[:-1] + b"\0").hexdigest()
    expected_targets = (
        ("broken-map.jpg", "#c0c0c0", broken_hash),
        (replacement, None, None),
        ("ordinary.png", "#23b95d", None),
        (fixture.name, "#c0c0c0", fixture_hash),
    )
    pending_count = 0
    for image, target, digest in expected_targets:
        for step in range(12):
            if step == 3:
                take("open", replacement)
                changed = take("replaced_same_size_and_mtime")
                pending_count += changed.get("decode_pending") is True
                after = REPLACEMENT_COLORS[1] if color == REPLACEMENT_COLORS[0] else REPLACEMENT_COLORS[0]
                require(changed.get("before_sha256") == ppm_hash(color)
                        and changed.get("after_sha256") == ppm_hash(after),
                        "replacement content differs from the planned pixels")
                color = after
                take("open", replacement)
            else:
                require(take("open").get("image") in {item[0] for item in expected_targets},
                        "unexpected source in the burst")
            delay = take("wait").get("ms")
            require(type(delay) is int and 0 <= delay <= 12, "invalid delay")
        take("open", image)
        checkpoint(image, target or color, digest if image != replacement else ppm_hash(color))
    require(pending_count == len(expected_targets) and next(actions, None) is None
            and receipt.get("active_request") == last_request,
            "incomplete, extra or stale navigation evidence")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("viewer", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--seed", type=int, action="append", help="Repeat to run several seeds")
    args = parser.parse_args()
    viewer, fixture = (path.resolve(strict=True) for path in (args.viewer, args.fixture))
    fixture_bytes = fixture.read_bytes()
    viewer_hash = hashlib.sha256(viewer.read_bytes()).hexdigest()
    print(json.dumps({"sha256": {str(viewer): viewer_hash,
                                 str(fixture): hashlib.sha256(fixture_bytes).hexdigest()}}), flush=True)
    failed = False
    for seed in args.seed or [8, 21496]:
        with tempfile.TemporaryDirectory(prefix="licasa-hdr-navigation-") as directory:
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_PLATFORMTHEME="generic",
                       QT_QUICK_BACKEND="software", QSG_RENDER_LOOP="basic")
            for key in ("CONFIG", "CACHE", "DATA", "STATE"):
                env[f"XDG_{key}_HOME"] = str(Path(directory) / key.lower())
            command = [str(viewer), "--viewer", "--hdr-navigation-seed", str(seed), str(fixture)]
            try:
                run = subprocess.run(command, env=env, capture_output=True, text=True, timeout=18)
                print(json.dumps({"seed": seed, "exit": run.returncode,
                                  "stdout": run.stdout, "stderr": run.stderr}), flush=True)
                receipt = json.loads(run.stdout)
                require(run.returncode == 0 and "Binding loop detected" not in run.stderr,
                        "viewer failed or produced a binding loop")
                validate(receipt, seed, viewer, viewer_hash, fixture, fixture_bytes)
            except (KeyError, TypeError, ValueError, subprocess.TimeoutExpired) as error:
                print(f"Seed {seed} failed: {error}", flush=True)
                failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
