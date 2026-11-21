"""Reject lifecycle soak receipts with a changed cover, a skipped round or a leaked resource."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_soak as check

ROUNDS = 4
ACTIONS = ["hdr-settings"]
for number in range(1, ROUNDS + 1):
    ACTIONS += [f"round{number}:{name}" for name in ("play", "pause", "seek", "resume", "replay", "editor-open",
                                                     "editor-close", "away", "back", "use-sdr", "restore-hdr", "measure")]
HDR_COVER = [[0.31, 0.215, 0.106, 1], [0.299, 0.195, 0.095, 1], [0.239, 0.149, 0.066, 1]]


class SoakReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.fixture = root / "pixel-motion.jfif"
        self.fixture.write_bytes(b"\xff\xd8motion photo with embedded video")
        self.payload = root / "viewer"
        self.payload.write_bytes(b"test executable")
        self.directories = {name: root / name for name in ("config", "data", "cache", "state", "tmp", "output")}
        for path in self.directories.values():
            path.mkdir()
        (self.directories["output"] / "ordinary.png").write_bytes(b"\x89PNG")
        self.trace_path = root / "viewer.wayland.log"
        fixture_hash = hashlib.sha256(self.fixture.read_bytes()).hexdigest()
        lines, phases, frames = [], {}, []

        def emit(text):
            lines.append(text)
            return len(lines)

        def checkpoint(index, name):
            commit = emit("commit")
            marker = emit("marker")
            frame = self.frame(index, name, fixture_hash, commit)
            frames.append(frame)
            phases[frame["phase"]] = dict(surface_id=frame["surface_id"], revision=frame["revision"],
                                          marker_line=marker, lifetime=frame["surface_id"], commit=commit,
                                          presented_commit=commit, encoding="linear", observed=True)

        for index, name in enumerate(ACTIONS, 1):
            if name.endswith(":measure"):
                checkpoint(len(frames) + 1, "soak_cover")
            emit(f"LICASA_MOTION_STEP {index} {name}")
            if name == "hdr-settings":
                checkpoint(1, "cover")
        self.trace_path.write_text("\n".join(lines) + "\n")
        steps = [dict(step=f"round{number}:{name}") for number in range(1, ROUNDS + 1) for name in check.ROUND_STEPS]
        soak = [dict(round=number, threads=41, rss_kib=900000.0 + number * 3000, malloc_live_kib=300000.0 + number % 2 * 40,
                     descriptors=dict(total=60, socket=4, drm=3, sync_file=0, dmabuf=2, file=30, photo=0),
                     pool=dict(decode=0, retained=73156608, upload=73156608, targets=964362240), gate_free=True,
                     elapsed_ms=20000.0 * number) for number in range(1, ROUNDS + 1)]
        report = dict(status="passed", drained=True, gate_free_after_close=True,
                      remaining_bytes=dict(decode=0, retained=0, upload=0, targets=0),
                      build=dict(generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                                 executablePath=str(self.payload), sourceReceipt="a" * 64),
                      fixture=str(self.fixture), fixture_sha256=fixture_hash, fixture_sha256_at_finish=fixture_hash,
                      soak_rounds=ROUNDS, steps=steps, frames=frames, soak=soak, playback_checks=ROUNDS,
                      open_video_files=[], new_video_files=[])
        self.run = dict(exit_code=0, report=report, trace=dict(phases=phases))

    def frame(self, index, name, fixture_hash, commit):
        frame = dict(phase=f"motion_{index}_{name}", surface_id=15 + index, revision=str(index), frame_serial=commit,
                     window_token="00000000-0000-0000-0000-000000000001", output="headless", image_ready=True,
                     fullscreen=True, fixture_unchanged=True, settings_scrim_visible=False, code="hdr",
                     app_surface_tagged=True, vk_format=97, vk_color_space=1000104013, paper_white_nits=200,
                     peak_nits=800, ui_white_scale=2.5, photo_label="Photo", display_label="Display",
                     photo_status="hdr", photo_rendition="hdr", photo_visible_hdr_frames=8, hdr_still_loaded=True,
                     motion_frame=False, motion_frame_timestamp_us=-1, session_available=True, session_active=False,
                     session_playing=False, gate_free=True, fixture_sha256=fixture_hash, pixel_size=[1920, 1080],
                     current_request=f"image://licasa/1/{quote(str(self.fixture), safe='')}?licasa_stage=full")
        frame["readback"] = dict(size=[1920, 1080], revision=str(index), frame_serial=commit, format="rgba16f",
                                 samples=[dict(x=x, y=540, rgba=list(rgba)) for x, rgba in zip((958, 960, 962), HDR_COVER)])
        return frame

    def validate(self):
        return check.validate(self.run, self.trace_path, ROUNDS, self.fixture, self.payload, "headless",
                              self.directories)

    def rejects(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.validate()

    def round(self, number):
        return self.run["report"]["soak"][number - 1]

    def test_complete_receipt(self):
        summary = self.validate()
        self.assertEqual(summary["rounds"], ROUNDS)
        self.assertEqual(summary["checkpoints"], ROUNDS + 1)
        self.assertEqual(summary["round_ms"], dict(min=20000.0, max=20000.0))

    def test_slope_is_per_round(self):
        self.assertEqual(check.slope([0, 10, 20, 30]), 10)
        self.assertEqual(check.slope([5, 5, 5]), 0)

    def test_thread_kept_per_round(self):
        for number in range(2, ROUNDS + 1):
            self.round(number)["threads"] = 41 + number
        self.rejects("Threads grew from 41 to 44 by round 3")

    def test_photo_kept_open_per_playback(self):
        for number in range(1, ROUNDS + 1):
            descriptors = self.round(number)["descriptors"]
            descriptors["photo"] = number
            descriptors["total"] = 60 + number
        self.rejects("Open photo descriptors grew from 1 to 4")

    def test_new_descriptor_kind(self):
        self.round(4)["descriptors"]["sync_file"] = 3
        self.rejects("Open sync_file descriptors grew")

    def test_live_allocations_step(self):
        self.round(3)["malloc_live_kib"] += 100 * 1024
        self.rejects("Live allocations grew")

    def test_live_allocations_creep(self):
        for number in range(1, ROUNDS + 1):
            self.round(number)["malloc_live_kib"] = 300000.0 + number * 1024
        self.rejects("Live allocations grow by 1024 KiB per round")

    def test_rss_growth_is_reported_not_judged(self):
        self.round(4)["rss_kib"] += 200 * 1024
        self.assertEqual(self.validate()["rss_kib"]["last"], self.round(4)["rss_kib"])

    def test_changed_reservations(self):
        self.round(2)["pool"]["retained"] += 1
        self.rejects("HDR reservations changed by round 2")

    def test_first_round_without_the_photo(self):
        for number in range(1, ROUNDS + 1):
            self.round(number)["pool"]["retained"] = 0
        self.rejects("exactly the shown HDR photo")

    def test_decode_in_flight_when_measured(self):
        for number in range(1, ROUNDS + 1):
            self.round(number)["pool"]["decode"] = 4096
        self.rejects("exactly the shown HDR photo")

    def test_gate_held_after_a_round(self):
        self.round(3)["gate_free"] = False
        self.rejects("held the processing gate after round 3")

    def test_missing_round(self):
        del self.run["report"]["soak"][2]
        self.rejects("Missing or reordered soak rounds")

    def test_skipped_step(self):
        self.run["report"]["steps"] = [step for step in self.run["report"]["steps"] if step["step"] != "round2:ended"]
        self.rejects("skipped a required step")

    def test_changed_cover_in_a_later_round(self):
        frame = self.run["report"]["frames"][3]
        frame["readback"]["samples"][1]["rgba"] = [n * 0.6 for n in HDR_COVER[1][:3]] + [1]
        self.rejects("differs from the first HDR cover")

    def test_cover_on_sdr_base(self):
        self.run["report"]["frames"][2]["photo_status"] = "sdr"
        self.rejects("HDR cover is missing")

    def test_missing_checkpoint(self):
        del self.run["report"]["frames"][2]
        self.rejects("Missing, extra or reordered checkpoints")

    def test_checkpoint_before_hdr_restore(self):
        lines = self.trace_path.read_text().splitlines()
        show = next(i for i, line in enumerate(lines) if line.endswith("round2:restore-hdr"))
        proof = self.run["trace"]["phases"]["motion_3_soak_cover"]
        moved = lines.pop(show)
        lines.insert(proof["commit"], moved)
        self.trace_path.write_text("\n".join(lines) + "\n")
        self.rejects("No commit after round2:restore-hdr")

    def test_wrong_round_count(self):
        self.run["report"]["soak_rounds"] = ROUNDS + 1
        self.rejects("Wrong number of soak rounds")

    def test_retained_reservation_after_close(self):
        self.run["report"]["remaining_bytes"]["targets"] = 4096
        self.rejects("retained HDR reservations")

    def test_another_window(self):
        self.run["report"]["frames"][4]["window_token"] = "00000000-0000-0000-0000-000000000002"
        self.rejects("Another window")

    def test_written_video(self):
        (self.directories["cache"] / "clip.bin").write_bytes(b"\0\0\0\x18ftypisom")
        self.rejects("Video files were written")


if __name__ == "__main__":
    unittest.main()
