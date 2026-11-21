"""Reject camera navigation receipts with changed pixels, HDR at the low limit, a held pool or a misordered journey."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_camera as check

ACTIONS = ("hdr-settings", "second", "first-again", "burst", "limit-low", "first-low", "second-low",
           "limit-restore", "first-restored", "clear", "open-first:dialog", "open-second:dialog",
           "open-second:attach", "close-new-window")
# The checkpoint that follows each action, in journey order.
AFTER = {"hdr-settings": 1, "second": 2, "first-again": 3, "burst": 4, "limit-low": 4, "first-low": 5,
         "second-low": 6, "limit-restore": 7, "first-restored": 8, "clear": 8, "open-first:dialog": 9,
         "open-second:dialog": 9, "open-second:attach": 10, "close-new-window": 11}
PIXELS = {("first", "hdr"): [[0.9, 0.7, 0.5, 1], [1.2, 0.9, 0.6, 1], [0.8, 0.6, 0.4, 1]],
          ("second", "hdr"): [[0.31, 0.21, 0.1, 1], [0.3, 0.19, 0.09, 1], [0.24, 0.15, 0.07, 1]],
          ("first", "sdr_base"): [[0.5, 0.4, 0.3, 1], [0.6, 0.5, 0.35, 1], [0.45, 0.35, 0.25, 1]],
          ("second", "sdr_base"): [[0.3, 0.2, 0.1, 1], [0.29, 0.19, 0.09, 1], [0.23, 0.14, 0.07, 1]]}


class CameraNavigationReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.photos = dict(first=root / "pixel-6-pro.jpg", second=root / "pixel-9-pro-xl.jfif")
        self.photos["first"].write_bytes(b"\xff\xd8first camera photo")
        self.photos["second"].write_bytes(b"\xff\xd8second camera photo")
        self.payload = root / "viewer"
        self.payload.write_bytes(b"test executable")
        self.seed = 9
        self.trace_path = root / "viewer.wayland.log"
        hashes = {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in self.photos.items()}
        plan = check.navigation_plan(self.seed)
        lines, phases, frames, checkpoint = [], {}, [], 0
        for index, name in enumerate(ACTIONS, 1):
            lines.append(f"LICASA_CAMERA_STEP {index} {name}")
            if name == "burst":
                lines.extend(f"LICASA_CAMERA_ACTION {n} {entry['action']}" for n, entry in enumerate(plan, 1))
            while checkpoint < AFTER[name]:
                checkpoint += 1
                lines.append("commit")
                commit = len(lines)
                lines.append("marker")
                frame = self.frame(checkpoint, hashes, commit)
                frames.append(frame)
                lifetime = 8 if frame["phase"].endswith("second_new_window") else 7
                phases[frame["phase"]] = dict(surface_id=lifetime, revision=frame["revision"], marker_line=len(lines),
                                              lifetime=lifetime, commit=commit, presented_commit=commit,
                                              encoding="linear", observed=True)
        self.trace_path.write_text("\n".join(lines) + "\n")
        report = dict(status="passed", drained=True, remaining_bytes=dict(decode=0, retained=0, upload=0, targets=0),
                      build=dict(generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                                 executablePath=str(self.payload), sourceReceipt="a" * 64),
                      first=str(self.photos["first"]), second=str(self.photos["second"]),
                      first_sha256=hashes["first"], second_sha256=hashes["second"],
                      seed=self.seed, navigation_plan=plan, frames=frames, steps=[], low_limit_mp=100,
                      new_window_output="hdr")
        self.run = dict(exit_code=0, report=report, trace=dict(phases=phases))

    def frame(self, index, hashes, commit):
        name, photo, rendition = check.CHECKPOINTS[index - 1]
        hdr = rendition == "hdr"
        token = 3 if name == "second_new_window" else 1 if photo == "first" else 2
        frame = dict(phase=f"camera_{index}_{name}", surface_id=8 if token == 3 else 7, revision="4", frame_serial=commit,
                     window_token=f"00000000-0000-0000-0000-00000000000{token}",
                     output="headless", image_ready=True,
                     code="hdr", app_surface_tagged=True, vk_format=97, vk_color_space=1000104013,
                     paper_white_nits=200, peak_nits=800, photo_label="Photo", display_label="Display",
                     photo_status="hdr" if hdr else "unavailable", photo_rendition="hdr" if hdr else "sdr",
                     photo_visible_hdr_frames=6 if hdr else 0,
                     limit_mp=536 if hdr else 100,
                     fixture_sha256=hashes[photo], pixel_size=[1920, 1080],
                     current_request=f"image://licasa/1/{quote(str(self.photos[photo]), safe='')}?licasa_stage=full")
        if not hdr:
            frame["photo_new_hdr_frames"] = 0
        frame["readback"] = dict(format="rgba16f", frame_serial=commit, size=[1920, 1080],
                                 samples=[dict(x=x, y=540, rgba=list(rgba))
                                          for x, rgba in zip((958, 960, 962), PIXELS[(photo, rendition)])])
        return frame

    def validate(self):
        return check.validate(self.run, self.trace_path, self.seed, self.photos, self.payload, "headless")

    def named(self, name):
        return next(frame for frame in self.run["report"]["frames"] if frame["phase"].endswith("_" + name))

    def rejects(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.validate()

    def test_complete_receipt(self):
        self.assertEqual(self.validate()["checkpoints"], 11)

    def retirement_receipt(self, recovered):
        report = deepcopy(self.run["report"])
        reference = deepcopy(self.named("second_low_limit"))
        second, first, base, after = map(deepcopy, (self.named("second"), self.named("first"), reference,
                                                   self.named("second")))
        base["current_request"] = second["current_request"].replace("/1/", "/3/", 1)
        after["current_request"] = base["current_request"]
        frames = [second, first, base] + ([after] if recovered else [])
        outgoing = dict(decode=0, retained=100, upload=100, targets=900)
        incoming = dict(decode=0, retained=73, upload=73, targets=900)
        for index, (frame, name, count) in enumerate(zip(
                frames, ("second_reference", "first_reference", "retry_base", "retry_recovered"),
                (incoming, outgoing, outgoing, incoming)), 1):
            photo = "first" if index == 2 else "second"
            frame.update(phase=f"camera_{index}_{name}", limit_mp=126, window="9", budget_revision="3",
                         presented_identity=check.fixture_identity(self.photos[photo]), pool_bytes=count,
                         frame_serial=index)
            frame["readback"]["frame_serial"] = index
        report.update(retirement_retry=True, starting_limit_mp=126, closing_limit_mp=126, frames=frames,
                      closing_budget_revision="3", closing_output_revision="4",
                      budget_bytes=dict(retained=128), closing_pool_upload=73 if recovered else 0,
                      state_at_finish=dict(request=base["current_request"], resident=str(self.photos["second"])),
                      status="passed" if recovered else "failed",
                      error=None if recovered else "timeout:sample:retry_recovered")
        lines, phases = [], {}

        def step(index, label):
            lines.append(f"LICASA_CAMERA_STEP {index} {label}")

        def present(frame):
            lines.extend(("commit", "marker"))
            phases[frame["phase"]] = dict(surface_id=7, revision="4", marker_line=len(lines),
                                           lifetime=7, commit=len(lines)-1, presented_commit=len(lines)-1,
                                           encoding="linear", observed=True)

        def event(kind, bytes_used=outgoing, **data):
            row = dict(event=kind, window="9", revision="4", budget_revision="3", **bytes_used)
            row.update(data)
            lines.append("LICASA_RASTER_RETRY " + json.dumps(row))

        def decode(kind, serial, retry, bytes_used=outgoing):
            event(kind, bytes_used, source=str(self.photos["second"]), request=base["current_request"],
                  serial=serial, retry=retry, default_fits=True)

        step(1, "hdr-settings")
        step(2, "clear-start")
        step(3, "second-reference")
        present(second)
        step(4, "clear-reference")
        step(5, "first-reference")
        present(first)
        step(6, "retry-navigate")
        event("texture_retired", source=str(self.photos["first"]), request=first["current_request"])
        event("fence_submitted")
        decode("decode_submitted", 5, 0)
        decode("raster_refused", 5, 0)
        if recovered:
            decode("retry_scheduled", 5, 1)
        present(base)
        if recovered:
            decode("retry_executed", 5, 1)
            decode("decode_submitted", 7, 0)
            decode("raster_refused", 7, 1)
            decode("retry_scheduled", 7, 2)
        step(7, "retry-release")
        event("fence_complete")
        freed = dict(outgoing, retained=0, upload=0)
        event("lease_released", freed)
        if recovered:
            decode("retry_executed", 7, 2, freed)
            decode("decode_submitted", 9, 0, freed)
            decode("raster_admitted", 9, 2, dict(incoming, upload=0))
            present(after)
        lines.extend(("LICASA_RASTER_RETRY_CLOSE", "surface retired", "LICASA_RASTER_RETRY_DRAIN 1"))
        run = dict(exit_code=0 if recovered else 1, report=report,
                   trace=dict(phases=phases, retirements={7: len(lines)-1}))
        return run, lines, reference

    def verify_retirement(self, run, lines, reference):
        self.trace_path.write_text("\n".join(lines) + "\n")
        return check.validate_retirement(run, self.trace_path, self.photos, self.payload, "headless", reference)

    def test_retirement_recovery(self):
        self.assertTrue(self.verify_retirement(*self.retirement_receipt(True))["recovered"])

    def test_retirement_control_fails_at_recovery(self):
        self.assertFalse(self.verify_retirement(*self.retirement_receipt(False))["recovered"])

    def test_retirement_requires_refusal_before_release(self):
        run, lines, reference = self.retirement_receipt(False)
        lines = ["missing" if '"raster_refused"' in line else line for line in lines]
        with self.assertRaisesRegex(ValueError, "Missing raster refusal"):
            self.verify_retirement(run, lines, reference)

    def test_retirement_rejects_release_before_fence_completion(self):
        run, lines, reference = self.retirement_receipt(True)
        release = next(i for i, line in enumerate(lines) if '"lease_released"' in line)
        lines[release-1], lines[release] = lines[release], lines[release-1]
        with self.assertRaisesRegex(ValueError, "lease-release order"):
            self.verify_retirement(run, lines, reference)

    def test_retirement_requires_surface_destruction_before_drain(self):
        run, lines, reference = self.retirement_receipt(True)
        run["trace"]["retirements"][7] = len(lines) + 1
        with self.assertRaisesRegex(ValueError, "surface did not retire"):
            self.verify_retirement(run, lines, reference)

    def test_retirement_rejects_solo_presentation_before_open(self):
        run, lines, reference = self.retirement_receipt(True)
        phase = run["trace"]["phases"]["camera_1_second_reference"]
        phase["presented_commit"] = 2
        with self.assertRaisesRegex(ValueError, "solo photos"):
            self.verify_retirement(run, lines, reference)

    def test_retirement_requires_execution_and_new_request(self):
        for kind in ("retry_executed", "decode_submitted"):
            with self.subTest(kind=kind):
                run, lines, reference = self.retirement_receipt(True)
                index = max(i for i, line in enumerate(lines) if f'"{kind}"' in line)
                lines[index] = "missing"
                with self.assertRaisesRegex(ValueError, "Missing or reordered"):
                    self.verify_retirement(run, lines, reference)

    def test_plan_is_seeded_and_ends_on_the_second_photo(self):
        plan = check.navigation_plan(self.seed)
        self.assertEqual(len(plan), 17)
        self.assertEqual(plan[-1], dict(action="second", delay_ms=0))
        self.assertNotEqual(plan, check.navigation_plan(self.seed + 1))

    def test_changed_return(self):
        self.named("first_again")["readback"]["samples"][1]["rgba"][0] = 1.0
        self.rejects("pixels changed")

    def test_hdr_left_on_at_the_low_limit(self):
        frame = self.named("second_low_limit")
        frame.update(photo_status="hdr", photo_rendition="hdr")
        self.rejects("SDR base at the low limit")

    def test_no_recovery_after_raising_the_limit(self):
        frame = self.named("second_restored_limit")
        frame.update(photo_status="unavailable", photo_rendition="sdr", photo_visible_hdr_frames=0)
        self.rejects("Missing HDR photo")

    def test_boosted_sdr_base(self):
        self.named("first_low_limit")["readback"]["samples"][0]["rgba"][0] = 3.0
        self.rejects("brighter than paper white")

    def test_same_pixels_for_both_photos(self):
        for frame in self.run["report"]["frames"]:
            if frame["phase"].split("_", 2)[2].startswith("second") and frame["photo_status"] == "hdr":
                for sample, rgba in zip(frame["readback"]["samples"], PIXELS[("first", "hdr")]):
                    sample["rgba"] = list(rgba)
        self.rejects("not distinct")

    def test_retained_pool(self):
        self.run["report"]["remaining_bytes"]["retained"] = 4096
        self.rejects("retained HDR reservations")

    def test_missing_checkpoint(self):
        del self.run["report"]["frames"][3]
        self.rejects("Missing, extra or reordered checkpoints")

    def test_wrong_seed(self):
        self.run["report"]["navigation_plan"] = check.navigation_plan(self.seed + 1)
        self.rejects("Wrong navigation plan")

    def test_checkpoint_before_its_action(self):
        phases = self.run["trace"]["phases"]
        proof = phases[self.named("second_restored_limit")["phase"]]
        lines = self.trace_path.read_text().splitlines()
        marker = next(i for i, line in enumerate(lines) if line.endswith(" limit-restore"))
        lines[marker], lines[proof["commit"]] = "moved", lines[marker]
        self.trace_path.write_text("\n".join(lines) + "\n")
        self.rejects("No commit after limit-restore|step markers")

    def test_low_limit_outside_range(self):
        self.run["report"]["low_limit_mp"] = 536
        self.rejects("outside the qualified range")

    def test_open_reused_the_window(self):
        self.run["trace"]["phases"][self.named("second_new_window")["phase"]]["lifetime"] = 7
        self.named("second_new_window")["surface_id"] = 7
        self.run["trace"]["phases"][self.named("second_new_window")["phase"]]["surface_id"] = 7
        self.rejects("Another window|not distinct")

    def test_new_window_on_sdr_output_for_memory(self):
        self.run["report"]["new_window_output"] = "memory"
        frame = self.named("second_new_window")
        frame.update(code="memory", app_surface_tagged=False, vk_format=44, vk_color_space=0, photo_status="none",
                     photo_rendition="sdr", photo_visible_hdr_frames=0, ui_white_scale=0, paper_white_nits=0, peak_nits=0)
        frame["readback"]["format"] = "rgba8"
        self.run["trace"]["phases"][frame["phase"]].update(encoding="sdr", observed=False)
        self.assertEqual(self.validate()["new_window_output"], "memory")
        frame["code"] = "disabled"
        self.rejects("not on SDR output for memory")

    def test_new_window_on_its_sdr_base(self):
        self.run["report"]["new_window_output"] = "sdr-base"
        frame = self.named("second_new_window")
        frame.update(photo_status="unavailable", photo_rendition="sdr", photo_visible_hdr_frames=0, photo_new_hdr_frames=0)
        for sample, rgba in zip(frame["readback"]["samples"], PIXELS[("second", "sdr_base")]):
            sample["rgba"] = list(rgba)
        self.assertEqual(self.validate()["new_window_output"], "sdr-base")
        frame["readback"]["samples"][0]["rgba"][0] = 3.0
        self.rejects("not held on its SDR base")

    def test_new_window_without_hdr(self):
        self.named("second_new_window").update(photo_status="unavailable", photo_rendition="sdr", photo_visible_hdr_frames=0)
        self.rejects("Missing HDR photo")

    def test_another_window(self):
        self.run["trace"]["phases"][self.named("first_again")["phase"]]["lifetime"] = 9
        self.rejects("Another window")

    def test_changed_photo(self):
        self.photos["first"].write_bytes(b"\xff\xd8changed")
        self.rejects("not the fixture")


if __name__ == "__main__":
    unittest.main()
