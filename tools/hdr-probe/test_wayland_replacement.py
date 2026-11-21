"""Reject receipts that show the wrong file, a stale HDR photo or an unrequested reload."""

import copy
import hashlib
import math
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_replacement as check
from check_wayland_e2e import inspect_trace
from test_wayland_e2e import TOPLEVEL, FEEDBACK, LINEAR_TAG, BUFFER_COMMIT

FIXTURES = Path(__file__).resolve().parents[2] / "tests/test-assets/formats/hdr"


class ReplacementReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.gray, self.red = FIXTURES / "jpeg-gainmap-dual.jpg", FIXTURES / "jpeg-gainmap-p3-red.jpg"
        self.scratch = root / check.SCRATCH_NAME
        self.payload = root / "viewer"
        self.payload.write_bytes(b"test executable")
        self.seed = 42
        self.plan = check.action_plan(self.seed)
        contents = check.padded_contents(self.gray, self.red)
        self.hashes = [hashlib.sha256(item).hexdigest() for item in contents]
        self.size = len(contents[0])
        report = dict(status="passed", drained=True, completed_actions=100, seed=self.seed, plan=self.plan,
                      remaining_bytes=dict(decode=0, retained=0, upload=0, targets=0),
                      fixture=str(self.scratch), frames=[], interrupted=[],
                      source_fixtures=dict(gray=str(self.gray), gray_sha256=check.GRAY_FIXTURE_SHA256,
                                           red=str(self.red), red_sha256=check.RED_FIXTURE_SHA256),
                      generations=[self.generation(0, "initial", 100)],
                      build=dict(generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                                 executablePath=str(self.payload), sourceReceipt="a" * 64))
        trace = dict(phases={}, replace_actions=[], unmaps=[], retirements={})
        self.run = dict(exit_code=0, report=report, trace=trace)
        states = [dict(white=200, peak=800, hdr=True, high=False, valid=True, loaded=0, request=1,
                       revision=3, lifetime=1, token=1),
                  dict(white=100, peak=200, hdr=True, high=False, valid=True, loaded=0, request=1,
                       revision=3, lifetime=2, token=2)]
        inode = 100
        for step in range(101):
            entry = self.plan[step - 1] if step else None
            if entry:
                slot, action, line = entry["slot"], entry["action"], step * 100
                state = states[slot]
                trace["replace_actions"].append(dict(step=step, slot=slot, action=action, line=line))
                if action.startswith("replace-") or action == "reopen":
                    report["interrupted"].append(dict(step=step, slot=slot, action=action,
                                                      barrier_ms=entry["barrier_ms"], stage="hdr_upload"))
                if action.startswith("replace-"):
                    inode += action != "replace-inplace"
                    report["generations"].append(self.generation(len(report["generations"]), action, inode))
                current = len(report["generations"]) - 1
                if action.startswith("replace-") or action == "reload":
                    state.update(loaded=current, valid=state["hdr"],
                                 request=state["request"] + (2 if action != "reload" else 1))
                elif action == "toggle":
                    state["hdr"] = not state["hdr"]
                    state["valid"] = state["hdr"] and state["loaded"] == current
                    state["revision"] += 1
                elif action == "calibrate":
                    state["high"], state["hdr"] = not state["high"], True
                    state["white"] = (300 if state["high"] else 200) if slot == 0 else (150 if state["high"] else 100)
                    state["peak"] = state["white"] * (4 if slot == 0 else 2)
                    state["valid"] = state["loaded"] == current
                    state["revision"] += 1
                elif action == "hide":
                    trace["unmaps"].append(dict(lifetime=state["lifetime"], line=line + 1))
                else:
                    trace["retirements"][state["lifetime"]] = line + 1
                    state.update(lifetime=line + 3, token=step + 2, revision=3, loaded=current,
                                 valid=state["hdr"], request=1)
                self.frame(states[1 - slot], step, 1 - slot, "during", line + 10, current)
                if action == "hide":
                    state["valid"] = state["hdr"] and state["loaded"] == current
            current = len(report["generations"]) - 1
            for slot in (0, 1):
                self.frame(states[slot], step, slot, "settled", step * 100 + 20 + slot, current)

    def generation(self, index, kind, inode):
        return dict(generation=index, kind=kind, content="red" if index % 2 else "gray",
                    sha256=self.hashes[index % 2],
                    identity=f"66306:{inode}:{self.size}:{check.MODIFIED}:1790000000.{index + 1}")

    def frame(self, state, step, slot, stage, line, current):
        hdr, valid, loaded = state["hdr"], state["valid"], state["loaded"]
        revision, phase = str(state["revision"]), f"replace_{step}_{slot}_{stage}"
        content = "red" if loaded % 2 else "gray"
        mode = "sdr8" if not hdr else "hdr" if valid else "sdr_on_hdr"
        pixels = check.expected_pixels(content, mode, state["white"], state["peak"])
        frame = dict(phase=phase, slot=slot, step=step, surface_id=state["lifetime"], frame_serial=line,
            window_token=f"00000000-0000-0000-0000-{state['token']:012d}",
            revision=revision, output="headless", image_ready=True, fullscreen=False, can_remember=False,
            code="hdr" if hdr else "disabled", photo_rendition="hdr" if hdr and valid else "sdr",
            photo_status="hdr" if hdr and valid else "unavailable" if hdr else "none",
            photo_visible_hdr_frames=3 if hdr else 0, app_surface_tagged=hdr,
            vk_format=97 if hdr else 44, vk_color_space=1000104013,
            paper_white_nits=state["white"] if hdr else 0, peak_nits=state["peak"] if hdr else 0,
            ui_white_scale=state["white"] / 80 if hdr else 0,
            source_generation=loaded, file_generation=current, fixture_sha256=self.hashes[loaded % 2],
            presented_identity=self.run["report"]["generations"][loaded]["identity"],
            photo_label="Photo", display_label="Display",
            current_request=f"image://licasa/{state['request']}/{quote(str(self.scratch), safe='')}?licasa_stage=full",
            logical_size=[32, 32], pixel_size=[32, 32], device_pixel_ratio=1)
        if hdr and not valid:
            frame["photo_new_hdr_frames"] = 0
        frame["readback"] = dict(size=[32, 32], revision=revision, frame_serial=line,
            format="rgba16f" if hdr else "rgba8",
            samples=[dict(x=x, y=4, rgba=[*pixels, 1]) for x in (2, 4, 6)])
        self.run["report"]["frames"].append(frame)
        self.run["trace"]["phases"][phase] = dict(surface_id=state["lifetime"], revision=revision,
            lifetime=state["lifetime"], commit=line, presented_commit=line,
            encoding="linear" if hdr else "sdr", observed=hdr, live_lifetimes=[state["lifetime"]])

    def validate(self):
        return check.validate(self.run, self.seed, self.gray, self.red, self.payload, "headless")

    def find(self, predicate):
        return next(frame for frame in self.run["report"]["frames"] if predicate(frame))

    def test_complete_receipt(self):
        summary = self.validate()
        self.assertEqual(summary["checkpoints"], 302)
        self.assertEqual(summary["replacements"], len(self.run["report"]["generations"]) - 1)
        self.assertEqual(set(summary["replacement_kinds"]),
                         {"replace-atomic", "replace-inplace", "replace-symlink"})

    def test_plan_is_fixed_then_seeded(self):
        plan = check.action_plan(self.seed)
        self.assertEqual(len(plan), 100)
        self.assertEqual([entry["action"] for entry in plan[:20:2]], list(check.FIXED))
        self.assertEqual([entry["slot"] for entry in plan[:20]], [0, 1] * 10)
        self.assertEqual([(entry["slot"], entry["action"]) for entry in plan[20:26]], list(check.MIRRORED))
        self.assertEqual([entry["barrier_ms"] for entry in plan[:26]], [check.BARRIERS[i % 10] for i in range(26)])
        self.assertTrue(all(entry["delay_ms"] == 0 for entry in plan[:26]))
        self.assertEqual({entry["action"] for entry in plan}, set(check.ACTIONS))
        self.assertNotEqual(plan[26:], check.action_plan(8)[26:])
        self.assertEqual(plan, check.action_plan(self.seed))

    def test_fixed_opening_holds_both_photos_in_every_mode(self):
        # Seeds 0 and 2147483648 never held a replaced gray photo in 8-bit SDR
        # while the opening ended after FIXED. The opening must not depend on the seed.
        loaded, hdr, valid, current, seen = [0, 0], [True, True], [True, True], 0, set()
        for entry in check.action_plan(0)[:26]:
            slot, action = entry["slot"], entry["action"]
            if action.startswith("replace-"):
                current += 1
            if action.startswith("replace-") or action in ("reload", "reopen"):
                loaded[slot], valid[slot] = current, hdr[slot]
            elif action == "toggle":
                hdr[slot] = not hdr[slot]
                valid[slot] = hdr[slot] and loaded[slot] == current
            elif action in ("calibrate", "hide"):
                hdr[slot] = hdr[slot] or action == "calibrate"
                valid[slot] = hdr[slot] and loaded[slot] == current
            for peer in (0, 1):
                seen.add(("red" if loaded[peer] % 2 else "gray",
                          "sdr8" if not hdr[peer] else "hdr" if valid[peer] else "sdr_on_hdr",
                          loaded[peer] == current))
        self.assertEqual(seen, {(content, mode, fresh) for content in ("gray", "red") for mode, fresh in (
            ("hdr", True), ("hdr", False), ("sdr_on_hdr", False), ("sdr8", True), ("sdr8", False))})
        for seed in (0, 2147483648):
            self.assertEqual(check.action_plan(seed)[:26], check.action_plan(42)[:26])

    def test_reference_pixels_are_independent_of_the_viewer(self):
        # Display P3 (254, 0, 0) at four times SDR white, as in the offscreen readback check.
        for actual, wanted in zip(check.expected_pixels("red", "hdr", 80, 320), (4.85616, -0.16673, -0.07785)):
            self.assertTrue(math.isclose(actual, wanted, abs_tol=0.0005))
        self.assertTrue(math.isclose(check.expected_pixels("gray", "hdr", 200, 800)[0], 5.2707, abs_tol=0.001))
        self.assertTrue(math.isclose(check.expected_pixels("gray", "sdr_on_hdr", 200, 800)[0], 1.3177, abs_tol=0.001))
        self.assertEqual(check.expected_pixels("red", "sdr_on_hdr", 300, 1200), (3.75, 0.0, 0.0))

    def test_replacement_hdr_over_the_presented_base_is_rejected(self):
        # The defect this journey guards against: a window that has not
        # reloaded reports the replacement's HDR photo over its old base.
        held = self.find(lambda frame: frame["photo_status"] == "unavailable")
        other = "gray" if held["source_generation"] % 2 else "red"
        held.update(photo_status="hdr", photo_rendition="hdr")
        del held["photo_new_hdr_frames"]
        pixels = check.expected_pixels(other, "hdr", held["paper_white_nits"], held["peak_nits"])
        for sample in held["readback"]["samples"]:
            sample["rgba"] = [*pixels, 1]
        with self.assertRaisesRegex(ValueError, "A replaced file was not held in SDR"):
            self.validate()

    def test_wrong_file_pixels_and_requests_are_rejected(self):
        original = copy.deepcopy(self.run)
        frames = lambda: self.run["report"]["frames"]
        replaced_hdr = lambda: self.find(lambda f: f["photo_status"] == "hdr"
                                         and f["source_generation"] != f["file_generation"])
        held = lambda: self.find(lambda f: f["photo_status"] == "unavailable")
        calibrated = lambda: next(frame for frame in frames() if frame["phase"] == "replace_3_0_settled")
        reloaded = lambda: next(frame for frame in frames() if frame["phase"] == "replace_15_0_settled")

        def swap_pixels(frame):
            content = "gray" if frame["source_generation"] % 2 else "red"
            mode = "hdr" if frame["photo_status"] == "hdr" else "sdr_on_hdr"
            pixels = check.expected_pixels(content, mode, frame["paper_white_nits"], frame["peak_nits"])
            for sample in frame["readback"]["samples"]:
                sample["rgba"] = [*pixels, 1]

        mutations = (
            lambda: self.run["report"].update(drained=False),
            lambda: self.run["report"]["remaining_bytes"].update(upload=1),
            lambda: self.run["report"].update(seed=8),
            lambda: frames().pop(30),
            lambda: self.run["trace"]["replace_actions"].reverse(),
            lambda: frames()[10].update(window_token="foreign"),
            lambda: frames()[10].update(paper_white_nits=450),
            lambda: frames()[10]["readback"].update(frame_serial=1),
            lambda: self.run["trace"]["phases"]["replace_3_1_during"].update(presented_commit=0),
            lambda: self.run["trace"].update(unmaps=[]),
            lambda: self.run["trace"].update(retirements={}),
            lambda: self.run["report"]["build"].update(generation="b" * 64),
            # A window that kept the old file shows the replacement's pixels.
            lambda: swap_pixels(replaced_hdr()),
            lambda: swap_pixels(held()),
            # The held SDR base is still covered by new HDR photo frames.
            lambda: held().update(photo_new_hdr_frames=2),
            lambda: held().update(photo_rendition="hdr"),
            # The receipt names another file than the plan loaded.
            lambda: replaced_hdr().update(presented_identity=self.run["report"]["generations"][-1]["identity"]),
            lambda: replaced_hdr().update(source_generation=replaced_hdr()["file_generation"]),
            lambda: frames()[40].update(fixture_sha256=self.hashes[1 - frames()[40]["source_generation"] % 2]),
            lambda: frames()[40].update(file_generation=0),
            # Calibration reloaded the image, or a reload issued no new request.
            lambda: calibrated().update(current_request=calibrated()["current_request"].replace("/3/", "/9/")),
            lambda: reloaded().update(current_request=next(
                f for f in frames() if f["phase"] == "replace_14_0_settled")["current_request"]),
            # The file was not actually replaced, or not in the stated way.
            lambda: self.run["report"]["generations"][2].update(
                identity=self.run["report"]["generations"][1]["identity"]),
            lambda: self.run["report"]["generations"][1].update(
                identity=self.run["report"]["generations"][1]["identity"].replace(f":{self.size}:", ":7:")),
            lambda: self.run["report"]["generations"][1].update(
                identity=self.run["report"]["generations"][1]["identity"].replace(check.MODIFIED, "1700000001.0")),
            lambda: self.run["report"]["generations"][1].update(
                identity=self.run["report"]["generations"][0]["identity"].replace(":1790000000.1", ":1790000000.9")),
            lambda: self.run["report"]["generations"][1].update(sha256=self.hashes[0]),
            lambda: self.run["report"]["generations"].pop(),
            lambda: self.run["report"]["interrupted"].pop(),
            lambda: self.run["report"]["interrupted"][0].update(stage="guessed"),
            lambda: self.run["report"]["source_fixtures"].update(red_sha256=check.GRAY_FIXTURE_SHA256),
            lambda: self.run["report"].update(fixture=str(self.scratch.with_name("photo.jpg"))),
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                self.run = copy.deepcopy(original)
                mutation()
                with self.assertRaises(ValueError):
                    self.validate()

    def test_a_journey_without_a_held_file_is_rejected(self):
        # Without the fixed opening no window would calibrate after a peer's replacement.
        for frame in self.run["report"]["frames"]:
            if frame["photo_status"] == "unavailable" and frame["source_generation"] % 2:
                frame.update(photo_status="hdr", photo_rendition="hdr")
        with self.assertRaises(ValueError):
            self.validate()

    def test_wire_markers_record_replacement_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "trace.log"
            trace.write_text(TOPLEVEL + FEEDBACK + LINEAR_TAG + BUFFER_COMMIT +
                             "LICASA_WAYLAND_FRAME replace_0_0_settled 14 3\n" +
                             "LICASA_REPLACE_ACTION 1 1 replace-symlink\n" +
                             "LICASA_REPLACE_ACTION 2 0 unknown\n")
            observed = inspect_trace(trace)
        self.assertIn("replace_0_0_settled", observed["phases"])
        self.assertEqual(observed["replace_actions"],
                         [dict(step=1, slot=1, action="replace-symlink", line=observed["replace_actions"][0]["line"])])


if __name__ == "__main__":
    unittest.main()
