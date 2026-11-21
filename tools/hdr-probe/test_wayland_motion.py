"""Reject Motion Photo journey receipts with a changed cover, a stale still, a held gate or a written video."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_motion as check

ACTIONS = ("hdr-settings", "first:play", "first:pause", "first:seek", "use-sdr:badge", "use-sdr:close-settings",
           "sdr:play", "sdr:pause", "sdr:seek", "resume", "replay", "restore-hdr", "stop:play", "stop:stop",
           "editor:play", "editor:open", "editor:close", "cover-change:play", "cover-change:pause",
           "cover-change:seek", "cover-change:set", "cover-change:stop", "cover-change:away",
           "cover-change:reopen", "cover-change:play-again", "cover-change-play:stop", "cover-change:clear",
           "copy:play", "copy:dialog", "export:play", "export:pause", "export:dialog", "hide:play", "hide:hide",
           "hide:show", "navigation:play", "navigation:burst", "peer:open", "peer:play", "peer:close")
# The checkpoint that follows each action in journey order.
AFTER = {"hdr-settings": 1, "first:play": 2, "first:seek": 3, "use-sdr:close-settings": 4, "sdr:play": 5,
         "sdr:seek": 6, "replay": 7, "restore-hdr": 8, "stop:stop": 9, "editor:close": 10,
         "cover-change:stop": 11, "cover-change:clear": 12, "copy:dialog": 13, "export:dialog": 14,
         "hide:show": 15, "navigation:burst": 16, "peer:close": 17}
HDR_COVER = [[0.31, 0.215, 0.106, 1], [0.299, 0.195, 0.095, 1], [0.239, 0.149, 0.066, 1]]
SDR_COVER = [[0.369, 0.306, 0.212, 1], [0.384, 0.318, 0.224, 1], [0.376, 0.306, 0.212, 1]]
SDR_VIDEO = [[0.282, 0.243, 0.196, 1], [0.286, 0.251, 0.204, 1], [0.298, 0.255, 0.212, 1]]
HDR_VIDEO = [[check.linear(n) * 2.5 for n in rgba[:3]] + [1] for rgba in SDR_VIDEO]


class MotionJourneyReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.fixture = root / "pixel-motion.jfif"
        self.fixture.write_bytes(b"\xff\xd8motion photo with embedded video")
        self.peer = root / "synthetic-motion.jpg"
        self.peer.write_bytes(b"\xff\xd8peer")
        self.payload = root / "viewer"
        self.payload.write_bytes(b"test executable")
        self.directories = {name: root / name for name in ("config", "data", "cache", "state", "tmp", "output")}
        for path in self.directories.values():
            path.mkdir()
        output = self.directories["output"]
        (output / "ordinary.png").write_bytes(b"\x89PNG")
        (output / "original-copy.jfif").write_bytes(self.fixture.read_bytes())
        export = b"\xff\xd8plain sdr jpeg\xff\xd9"
        (output / "sdr-export.jpg").write_bytes(export)
        self.seed = 7
        self.trace_path = root / "viewer.wayland.log"
        fixture_hash = hashlib.sha256(self.fixture.read_bytes()).hexdigest()
        plan = check.navigation_plan(self.seed, self.peer)
        lines, line, phases, frames = [], 0, {}, []
        checkpoint = 0

        def emit(text):
            nonlocal line
            lines.append(text)
            line += 1
            return line

        for index, name in enumerate(ACTIONS, 1):
            emit(f"LICASA_MOTION_STEP {index} {name}")
            if name == "navigation:burst":
                for number, entry in enumerate(plan, 1):
                    emit(f"LICASA_MOTION_ACTION {number} {entry['action']}")
            while checkpoint < AFTER.get(name, 0):
                checkpoint += 1
                commit = emit("commit")
                marker = emit("marker")
                frames.append(self.frame(checkpoint, fixture_hash, commit))
                frame = frames[-1]
                phases[frame["phase"]] = dict(surface_id=frame["surface_id"], revision=frame["revision"],
                                              marker_line=marker, lifetime=frame["surface_id"], commit=commit,
                                              presented_commit=commit, encoding="linear" if frame["code"] == "hdr" else "sdr",
                                              observed=frame["code"] == "hdr")
        self.trace_path.write_text("\n".join(lines) + "\n")
        steps = [dict(step=name) for name in check.REQUIRED_STEPS]
        report = dict(status="passed", drained=True, gate_free_after_close=True,
                      remaining_bytes=dict(decode=0, retained=0, upload=0, targets=0),
                      build=dict(generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                                 executablePath=str(self.payload), sourceReceipt="a" * 64),
                      fixture=str(self.fixture), fixture_sha256=fixture_hash, fixture_sha256_at_finish=fixture_hash,
                      seed=self.seed, navigation_plan=plan, steps=steps, frames=frames,
                      saves=[dict(step="copy", path=str(output / "original-copy.jfif"), elapsed_ms=50,
                                  playback_active_after=False),
                             dict(step="export", path=str(output / "sdr-export.jpg"), elapsed_ms=170,
                                  playback_active_after=False)],
                      export=dict(sha256=hashlib.sha256(export).hexdigest(), width=2268, height=4032,
                                  upright_width=2268, upright_height=4032),
                      preferred_cover=dict(preferred_us=466689, first_at_or_past_ms=491, previous_ms=0,
                                           observed=[[491, 533356, 9081]]),
                      gate_observations=[dict(step=f"s{i}", gate_free=True) for i in range(8)],
                      replay_fresh_frame_us=66678, open_video_files=[], new_video_files=[])
        self.run = dict(exit_code=0, report=report, trace=dict(phases=phases))

    def frame(self, index, fixture_hash, commit):
        name, mode, content = check.CHECKPOINTS[index - 1]
        hdr = mode == "hdr"
        cover = content == "cover"
        surface = 15 if index <= 3 else 103 if index <= 7 else 132
        if cover:
            pixels = HDR_COVER if hdr else SDR_COVER
        else:
            pixels = HDR_VIDEO if hdr else SDR_VIDEO
        frame = dict(phase=f"motion_{index}_{name}", surface_id=surface, revision=str(index), frame_serial=commit,
                     window_token="00000000-0000-0000-0000-000000000001", output="headless", image_ready=True,
                     fullscreen=True, fixture_unchanged=True, settings_scrim_visible=False,
                     code="hdr" if hdr else "disabled", app_surface_tagged=hdr, vk_format=97 if hdr else 44,
                     vk_color_space=1000104013, paper_white_nits=200 if hdr else 0, peak_nits=800 if hdr else 0,
                     ui_white_scale=2.5 if hdr else 0, photo_label="Photo", display_label="Display",
                     photo_status="hdr" if hdr and cover else "none",
                     photo_rendition="hdr" if hdr and cover else "sdr",
                     photo_visible_hdr_frames=8 if hdr and cover else 0,
                     hdr_still_loaded=hdr and cover, motion_frame=not cover,
                     motion_frame_timestamp_us=-1 if cover else 133344 if name == "replay_sdr" else 800033,
                     session_available=True, session_active=not cover, session_playing=name in ("playing", "playing_sdr"),
                     gate_free=cover, fixture_sha256=fixture_hash, pixel_size=[1920, 1080],
                     current_request=f"image://licasa/1/{quote(str(self.fixture), safe='')}?licasa_stage=full")
        frame["readback"] = dict(size=[1920, 1080], revision=str(index), frame_serial=commit,
                                 format="rgba16f" if hdr else "rgba8",
                                 samples=[dict(x=x, y=540, rgba=list(rgba)) for x, rgba in zip((958, 960, 962), pixels)])
        return frame

    def validate(self):
        return check.validate(self.run, self.trace_path, self.seed, self.fixture, self.peer, self.payload,
                              "headless", self.directories)

    def frame_named(self, name):
        return next(frame for frame in self.run["report"]["frames"] if frame["phase"].endswith("_" + name))

    def rejects(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.validate()

    def test_complete_receipt(self):
        summary = self.validate()
        self.assertEqual(summary["checkpoints"], 17)
        self.assertEqual(summary["navigation_actions"], 13)

    def test_navigation_plan_is_seeded_and_ends_on_the_photo(self):
        plan = check.navigation_plan(self.seed, self.peer)
        self.assertEqual(len(plan), 13)
        self.assertEqual(plan[-1], dict(action="main", delay_ms=0))
        self.assertNotIn("peer", [entry["action"] for entry in check.navigation_plan(self.seed, None)])
        self.assertNotEqual(plan, check.navigation_plan(self.seed + 1, self.peer))

    def test_changed_restored_cover(self):
        frame = self.frame_named("cover_after_hdr_restore")
        frame["readback"]["samples"][1]["rgba"] = [n * 0.6 for n in frame["readback"]["samples"][1]["rgba"][:3]] + [1]
        self.rejects("differs from the first HDR cover")

    def test_still_left_under_the_video(self):
        self.frame_named("paused_seek")["hdr_still_loaded"] = True
        self.rejects("video frame is not shown in place of the still")

    def test_boosted_video(self):
        self.frame_named("playing")["readback"]["samples"][0]["rgba"][0] = 3.0
        self.rejects("brighter than paper white")

    def test_paused_frame_not_at_paper_white(self):
        sample = self.frame_named("paused_seek")["readback"]["samples"][2]["rgba"]
        sample[1] *= 1.2
        self.rejects("not its SDR value at paper white")

    def test_paused_frames_differ(self):
        self.frame_named("paused_sdr")["motion_frame_timestamp_us"] = 766700
        self.rejects("same paused frame")

    def test_cover_sampled_while_playback_holds_the_gate(self):
        self.frame_named("cover_after_copy")["gate_free"] = False
        self.rejects("holds the gate")

    def test_scrim_over_checkpoint(self):
        self.frame_named("cover_sdr")["settings_scrim_visible"] = True
        self.rejects("covered photo")

    def test_changed_copy(self):
        (self.directories["output"] / "original-copy.jfif").write_bytes(b"other")
        self.rejects("original copy differs")

    def test_export_keeps_gain_map(self):
        path = self.directories["output"] / "sdr-export.jpg"
        data = b"\xff\xd8hdrgm\xff\xd9"
        path.write_bytes(data)
        self.run["report"]["export"]["sha256"] = hashlib.sha256(data).hexdigest()
        self.rejects("not a plain upright SDR JPEG")

    def test_save_left_playback_running(self):
        self.run["report"]["saves"][0]["playback_active_after"] = True
        self.rejects("playback stopped")

    def test_preferred_cover_played_through(self):
        self.run["report"]["preferred_cover"].update(previous_ms=433, observed=[[400, 0, 0], [433, 0, 0], [491, 0, 0]])
        self.rejects("preferred cover")

    def test_written_video(self):
        (self.directories["tmp"] / "motion.bin").write_bytes(b"\0\0\0\x18ftypisom")
        self.rejects("Video files were written")

    def test_reported_open_video(self):
        self.run["report"]["open_video_files"] = ["/tmp/x.mp4"]
        self.rejects("opened or created a video file")

    def test_missing_checkpoint(self):
        del self.run["report"]["frames"][9]
        self.rejects("Missing, extra or reordered checkpoints")

    def test_checkpoint_before_its_action(self):
        phases = self.run["trace"]["phases"]
        proof = phases[self.frame_named("cover_after_hide")["phase"]]
        self.trace_path.write_text(self.trace_path.read_text().replace(
            "LICASA_MOTION_STEP 35 hide:show", "LICASA_MOTION_STEP 35 hide:shown-late"))
        lines = self.trace_path.read_text().splitlines()
        lines.insert(proof["commit"], "LICASA_MOTION_STEP 35 hide:show")
        self.trace_path.write_text("\n".join(lines).replace("LICASA_MOTION_STEP 35 hide:shown-late", "") + "\n")
        self.rejects("step markers|No commit after hide:show")

    def test_wrong_seed(self):
        self.run["report"]["navigation_plan"] = check.navigation_plan(self.seed + 1, self.peer)
        self.rejects("Wrong navigation plan")

    def test_changed_photo(self):
        self.run["report"]["fixture_sha256_at_finish"] = "0" * 64
        self.rejects("changed during the journey")

    def test_retained_gate(self):
        self.run["report"]["gate_free_after_close"] = False
        self.rejects("processing gate")

    def test_another_window(self):
        self.frame_named("cover_after_peer_close")["window_token"] = "00000000-0000-0000-0000-000000000002"
        self.rejects("Another window")

    def test_replay_from_the_middle(self):
        self.run["report"]["replay_fresh_frame_us"] = 800033
        self.rejects("Replay did not restart")

    def test_stale_sdr_output(self):
        self.frame_named("replay_sdr")["code"] = "hdr"
        self.rejects("Stale HDR output")


if __name__ == "__main__":
    unittest.main()
