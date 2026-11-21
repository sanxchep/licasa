"""Test the receipt and wire-trace verifier without a desktop session."""

import hashlib
import copy
import tempfile
from urllib.parse import quote
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import check_wayland_e2e as e2e


TOPLEVEL = """\
[11.100] {Default Queue}  -> xdg_wm_base#3.get_xdg_surface(new id xdg_surface#16, wl_surface#14)
[11.101] {Default Queue}  -> xdg_surface#16.get_toplevel(new id xdg_toplevel#17)
"""
FEEDBACK = """\
[11.102]  -> wp_color_manager_v1#3.get_surface_feedback(new id wp_color_management_surface_feedback_v1#20, wl_surface#14)
"""
LINEAR_TAG = """\
[11.103]  -> wp_color_manager_v1#3.get_surface(new id wp_color_management_surface_v1#21, wl_surface#14)
[11.104]  -> wp_image_description_creator_params_v1#22.set_primaries_named(1)
[11.105]  -> wp_image_description_creator_params_v1#22.set_tf_named(5)
[11.106]  -> wp_image_description_creator_params_v1#22.create(new id wp_image_description_v1#23)
[11.107] {Mesa} wp_image_description_v1#23.ready(46)
[11.108]  -> wp_color_management_surface_v1#21.set_image_description(wp_image_description_v1#23, 0)
[11.109]  -> wp_image_description_v1#23.destroy()
"""
BUFFER_COMMIT = """\
[11.110]  -> wl_surface#14.attach(wl_buffer#39, 0, 0)
[11.111]  -> wl_surface#14.commit()
"""
LINEAR_FRAME = TOPLEVEL + FEEDBACK + LINEAR_TAG + BUFFER_COMMIT
UNTAG = " -> wp_color_management_surface_v1#21.unset_image_description()\n"


class WireTraceTest(unittest.TestCase):
    def trace(self, messages):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "trace.log"
            path.write_text(messages)
            return e2e.inspect_trace(path)

    def test_tag_applies_to_the_committed_surface(self):
        prefix = TOPLEVEL + FEEDBACK + LINEAR_TAG
        self.assertEqual(self.trace(prefix + BUFFER_COMMIT)["linear_commits_with_feedback"], 1)
        for unrelated in (BUFFER_COMMIT.replace("surface#14", "surface#15"),
                          BUFFER_COMMIT.replace("wl_buffer#39", "nil")):
            self.assertEqual(self.trace(prefix + unrelated)["linear_commits"], 0)
        without_feedback = self.trace(TOPLEVEL + LINEAR_TAG + BUFFER_COMMIT)
        self.assertEqual(without_feedback["linear_commits"], 1)
        self.assertEqual(without_feedback["linear_commits_with_feedback"], 0)

    def test_description_must_be_ready_and_linear(self):
        for original, replacement, encoding in ((".ready(46)", '.failed(0, "bad")', "other"),
                                                ("set_tf_named(5)", "set_tf_named(9)", "sdr"),
                                                ("set_tf_named(5)", "set_tf_named(11)", "other"),
                                                ("set_primaries_named(1)", "set_primaries_named(6)", "other")):
            with self.subTest(replacement=replacement):
                tag = LINEAR_TAG.replace(original, replacement)
                trace = self.trace(TOPLEVEL + FEEDBACK + tag + BUFFER_COMMIT)
                self.assertEqual(trace["last_toplevel_encoding"], encoding)
                self.assertEqual(trace["other_color_commits"], int(encoding == "other"))

    def test_legacy_trace_and_new_description_ready_event(self):
        for messages in (LINEAR_FRAME.replace("#", "@"), LINEAR_FRAME.replace(".ready(46)", ".ready2(0, 46)")):
            self.assertEqual(self.trace(messages)["linear_commits_with_feedback"], 1)

    def test_feedback_must_be_live_for_hdr_commit(self):
        destroy = " -> wp_color_management_surface_feedback_v1#20.destroy()\n"
        trace = self.trace(TOPLEVEL + FEEDBACK + destroy + LINEAR_TAG + BUFFER_COMMIT)
        self.assertEqual(trace["linear_commits"], 1)
        self.assertEqual(trace["linear_commits_with_feedback"], 0)

    def test_sdr_requires_a_new_buffer_commit_on_the_hdr_toplevel(self):
        self.assertTrue(self.trace(LINEAR_FRAME + UNTAG + BUFFER_COMMIT)["last_toplevel_commit_follows_hdr"])
        unrelated = (TOPLEVEL + BUFFER_COMMIT).replace("surface#14", "surface#15").replace("#16", "#26").replace("#17", "#27")
        for suffix in (UNTAG + " -> wl_surface#14.commit()\n", unrelated,
                       UNTAG + BUFFER_COMMIT.replace("wl_buffer#39", "nil")):
            with self.subTest(suffix=suffix):
                self.assertFalse(self.trace(LINEAR_FRAME + suffix)["last_toplevel_commit_follows_hdr"])

    def test_sdr_after_retirement_and_surface_id_reuse(self):
        trace = self.trace(TOPLEVEL + FEEDBACK + LINEAR_TAG + BUFFER_COMMIT +
                           " -> wp_color_management_surface_v1#21.destroy()\n" +
                           " -> wl_surface#14.destroy()\n" + TOPLEVEL + BUFFER_COMMIT)
        self.assertEqual(trace["linear_commits_with_feedback"], 1)
        self.assertEqual(trace["last_toplevel_encoding"], "sdr")
        self.assertTrue(trace["last_toplevel_commit_follows_hdr"])
        self.assertFalse(trace["errors"])

    def test_older_unrelated_toplevel_cannot_replace_the_hdr_role(self):
        unrelated = (TOPLEVEL + BUFFER_COMMIT).replace("surface#14", "surface#15").replace("#16", "#26").replace("#17", "#27")
        role, commit = unrelated.rsplit(" -> wl_surface#15.attach", 1)
        trace = self.trace(role + LINEAR_FRAME + " -> xdg_toplevel#17.destroy()\n"
                           + " -> wl_surface#15.attach" + commit)
        self.assertFalse(trace["last_toplevel_commit_follows_hdr"])

    def test_viewer_markers_bind_to_reused_surface_lifetimes(self):
        trace = self.trace(LINEAR_FRAME + "LICASA_WAYLAND_FRAME hdr 14 7\n"
                           + " -> xdg_toplevel#17.destroy()\n"
                           + " -> wl_surface#14.destroy()\n"
                           + TOPLEVEL + BUFFER_COMMIT + "LICASA_WAYLAND_FRAME sdr 14 8\n")
        hdr, sdr = trace["phases"]["hdr"], trace["phases"]["sdr"]
        self.assertEqual(hdr["surface_id"], sdr["surface_id"])
        self.assertGreater(sdr["lifetime"], hdr["commit"])
        self.assertFalse(sdr["hdr_role_live"])
        self.assertEqual((hdr["encoding"], sdr["encoding"]), ("linear", "sdr"))
        self.assertFalse(trace["errors"])

    def test_duplicate_color_surface_and_protocol_error(self):
        trace = self.trace(TOPLEVEL + LINEAR_TAG + LINEAR_TAG.splitlines()[0] + "\n" +
                           'wl_display#1.error(wp_color_manager_v1#3, 1, "duplicate")\n')
        self.assertEqual(len(trace["errors"]), 2)

    def test_presentation_feedback_matches_the_buffer_commit(self):
        request = " -> wp_presentation#30.feedback(wl_surface#14, new id wp_presentation_feedback#31)\n"
        presented = "wp_presentation_feedback#31.presented(0, 0, 0, 0, 0, 0, 0)\n"
        hdr = TOPLEVEL + FEEDBACK + LINEAR_TAG + request + BUFFER_COMMIT + presented
        sdr = UNTAG + request + BUFFER_COMMIT + presented
        self.assertEqual(self.trace(hdr + sdr)["sdr_presented_after_hdr"], 1)
        self.assertEqual(self.trace(hdr + sdr.replace("presented(0, 0, 0, 0, 0, 0, 0)", "discarded()"))[
            "sdr_presented_after_hdr"], 0)
        self.assertEqual(self.trace(hdr + sdr.replace("wl_buffer#39", "nil"))["sdr_presented_after_hdr"], 0)

    def test_later_hdr_presentation_cannot_fill_an_earlier_phase(self):
        request = " -> wp_presentation#30.feedback(wl_surface#14, new id wp_presentation_feedback#31)\n"
        presented = "wp_presentation_feedback#31.presented(0, 0, 0, 0, 0, 0, 0)\n"
        initial = TOPLEVEL + FEEDBACK + LINEAR_TAG + BUFFER_COMMIT + "LICASA_WAYLAND_FRAME hdr 14 7\n"
        calibrated = request + BUFFER_COMMIT + "LICASA_WAYLAND_FRAME calibrated 14 8\n" + presented
        trace = self.trace(initial + calibrated)
        self.assertEqual(trace["phases"]["hdr"]["presented_commit"], 0)
        self.assertEqual(trace["phases"]["calibrated"]["presented_commit"],
                         trace["phases"]["calibrated"]["commit"])

    def test_window_actions_bind_native_hide_to_the_surface_lifetime(self):
        trace = self.trace(LINEAR_FRAME + "LICASA_WAYLAND_FRAME window_zoomed 14 7\n"
                           + "LICASA_WAYLAND_ACTION 3 hide\n"
                           + " -> wl_surface#14.attach(nil, 0, 0)\n -> wl_surface#14.commit()\n"
                           + " -> xdg_toplevel#17.destroy()\n"
                           + "LICASA_WAYLAND_ACTION 4 restore\n")
        lifetime = trace["phases"]["window_zoomed"]["lifetime"]
        hide, restore = trace["actions"]
        self.assertEqual(trace["unmaps"][0]["lifetime"], lifetime)
        self.assertLess(hide["line"], trace["unmaps"][0]["line"])
        self.assertLess(trace["retirements"][lifetime], restore["line"])


class ReceiptTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        fixture, viewer = root / "photo.jpg", root / "viewer"
        fixture.write_bytes(b"fixture bytes")
        viewer.write_bytes(b"viewer bytes")
        self.fixture_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
        self.args = SimpleNamespace(expect="hdr", output="DP-1", probe=Path("probe"),
                                    viewer=viewer, viewer_payload=None, capture=Path("capture"), fixture=fixture)
        self.cap = {"exit_code": 0, "report": {
            "status": "capabilities_ready", "reason": "hdr_preferred", "surface_attached": True,
            "color_manager": True, "description_accepted": True, "preferred_identity": 1,
            "preferred_fingerprint": "hash", "reference_nits": 203, "target_nits": 1000,
            "sdr_frame_captured": True, "first_patch_red": 0.1804,
            "native_surface": {"screen": "DP-1", "device": "lab", "vk_format": 44, "vk_color_space": 0},
        }, "trace": {"last_toplevel_encoding": "sdr", "linear_commits": 0, "other_color_commits": 0}}
        self.surface = {"exit_code": 0, "report": {
            "status": "linear_fp16_frame_submitted", "screen": "DP-1", "preferred_fingerprint": "hash",
            "vk_format": 97, "vk_color_space": 1000104002,
        }, "trace": {"last_toplevel_has_feedback": True, "last_toplevel_encoding": "linear"}}
        self.viewer = {"exit_code": 0, "report": {
            "status": "passed",
            "hdr_frame": {"code": "hdr", "output": "DP-1", "revision": "9", "image_ready": True,
                          "paper_white_nits": 200, "peak_nits": 800, "ui_white_scale": 2.5,
                          "vk_format": 97, "vk_color_space": 1000104002,
                          "photo_visible_hdr_frames": 1},
            "last_frame": {"code": "disabled", "output": "DP-1", "revision": "10", "image_ready": True,
                           "vk_format": 44, "vk_color_space": 0,
                           "ui_white_scale": 0, "photo_visible_hdr_frames": 0},
        }, "trace": {"last_toplevel_encoding": "sdr", "linear_commits_with_feedback": 1, "linear_commits": 1,
                     "other_color_commits": 0, "last_toplevel_commit_follows_hdr": True}}
        self.viewer["report"].update(
            fixture=str(fixture), fixture_sha256=self.fixture_hash, used_sdr_control=True,
            build={"generation": hashlib.sha256(viewer.read_bytes()).hexdigest(),
                   "sourceReceipt": "a" * 64, "executablePath": str(viewer)})
        request = "image://licasa/1/" + quote(str(fixture), safe="") + "?licasa_stage=full"
        self.viewer["report"]["hdr_frame"].update(
            current_request=request, fixture_sha256=self.fixture_hash, surface_id=14, photo_rendition="hdr",
            photo_label="HDR photo", display_label="HDR display")
        self.viewer["report"]["last_frame"].update(
            current_request=request, fixture_sha256=self.fixture_hash, surface_id=14, photo_rendition="sdr",
            photo_label="SDR photo", display_label="SDR display")
        self.viewer["trace"]["phases"] = {
            "hdr": dict(surface_id=14, revision="9", encoding="linear", lifetime=2,
                        commit=9, presented_commit=9, hdr_role_live=False, observed=True),
            "sdr": dict(surface_id=14, revision="10", encoding="sdr", lifetime=2,
                        commit=12, presented_commit=12, hdr_role_live=True),
        }

    def check(self):
        children = {"capabilities": self.cap, "surface": self.surface, "viewer": self.viewer}
        with patch.object(e2e, "run_child", side_effect=lambda _dir, name, *_: children[name]) as run:
            e2e.check(self.args, {}, {})
            return [call.args[1] for call in run.call_args_list]

    def test_hdr_then_sdr(self):
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])

    def test_window_churn_requires_each_presented_geometry(self):
        self.args.window_churn = True
        with self.assertRaisesRegex(ValueError, "window journey"):
            self.check()

    def test_seeded_window_plan_is_bounded_and_repeatable(self):
        for seed in (0, 8, 42, 20261115, 0xffffffff):
            plan = e2e.seeded_window_plan(seed)
            self.assertEqual(plan, e2e.seeded_window_plan(seed))
            self.assertEqual(len(plan), 100)
            self.assertEqual([entry["action"] for entry in plan[:5]], list(e2e.WINDOW_ACTIONS))
            for index, entry in enumerate(plan):
                self.assertIn(entry["delay_ms"], (0, 25, 50, 75))
                if entry["action"] == "hide":
                    self.assertEqual(plan[index+1]["action"], "restore")
        self.assertNotEqual(e2e.seeded_window_plan(8), e2e.seeded_window_plan(42))

    def test_window_seed_requires_churn_and_an_unsigned_value(self):
        for enabled, seed in ((False, 42), (True, -1), (True, 0x100000000)):
            self.args.window_churn = enabled
            self.args.window_seed = seed
            with self.subTest(enabled=enabled, seed=seed):
                with self.assertRaisesRegex(ValueError, "window seed"):
                    self.check()

    def seeded_receipt(self, hdr=True):
        self.window_receipt(hdr)
        self.args.window_seed = 42
        output, trace = self.viewer["report"], self.viewer["trace"]
        plan = e2e.seeded_window_plan(42)
        output.update(window_seed="42", window_plan=plan,
                      window_actions=[entry["action"] for entry in plan],
                      hidden_count=sum(entry["action"] == "hide" for entry in plan))
        base = copy.deepcopy(output["window_frames"][0])
        frames = []
        trace.update(actions=[], unmaps=[], retirements={})
        fullscreen, size, scale = True, [1920,1080], 1.0
        for index, entry in enumerate([None] + plan):
            line = 20 * (index+1)
            action = entry["action"] if entry else None
            if action:
                trace["actions"].append(dict(sequence=index, action=action, line=line-5))
            if action == "hide":
                trace["unmaps"].append(dict(lifetime=2, line=line-2))
                continue
            if action in ("fullscreen", "floating"):
                fullscreen = action == "fullscreen"
                size = [1920,1080] if fullscreen else [32,32]
            if action in ("zoom", "zoom-out"):
                factor = 1.15 if action == "zoom" else 1/1.15
                scale = max(0.02 if fullscreen else 1, scale * factor)
                if not fullscreen:
                    size = [round(value*factor) for value in size]
            frame = copy.deepcopy(base)
            phase = f"window_seed_{len(frames)}"
            frame.update(phase=phase, frame_serial=line, fullscreen=fullscreen,
                         visibility=5 if fullscreen else 2, logical_size=list(size),
                         pixel_size=list(size), photo_scale=scale)
            x, y = (size[0]//2, size[1]//2) if fullscreen else (4,4)
            if fullscreen:
                half_cell = int(scale/2 + 0.5)
                x, y = x - half_cell, y - half_cell
            frame["readback"].update(frame_serial=line, size=list(size))
            for offset, sample in zip((-2,0,2), frame["readback"]["samples"]):
                sample.update(x=x+offset, y=y)
            trace["phases"][phase] = dict(surface_id=14, revision=frame["revision"],
                encoding="linear" if hdr else "sdr", lifetime=2, live_lifetimes=[2],
                commit=line, presented_commit=line, observed=hdr)
            frames.append(frame)
        output["discarded_capture_serial"] = 30
        output["window_frames"] = frames
        trace["phases"]["hdr"].update(lifetime=2, commit=3000, presented_commit=3000)
        trace["phases"]["sdr"].update(lifetime=2, commit=4000, presented_commit=4000)

    def test_seeded_window_journey_rejects_false_receipts(self):
        for hdr in (True, False):
            self.setUp()
            self.seeded_receipt(hdr)
            self.check()
            original = copy.deepcopy(self.viewer)
            mutations = (
                lambda: self.viewer["report"].update(window_seed="8"),
                lambda: self.viewer["report"]["window_plan"][10].update(delay_ms=999),
                lambda: self.viewer["report"]["window_actions"].reverse(),
                lambda: self.viewer["report"]["window_frames"].pop(10),
                lambda: self.viewer["report"].update(hidden_count=0),
                lambda: self.viewer["trace"].update(unmaps=[]),
                lambda: self.viewer["report"]["window_frames"][10]["readback"]["samples"][0].update(rgba=[0,0,0,1]),
                lambda: self.viewer["trace"]["phases"]["window_seed_10"].update(presented_commit=0),
                lambda: self.viewer["report"]["window_frames"][10].update(frame_serial=1),
                lambda: self.viewer["report"]["window_frames"][10].update(window_token="foreign"),
                lambda: self.viewer["report"]["window_frames"][2].update(photo_scale=1.0),
            )
            for mutation in mutations:
                with self.subTest(hdr=hdr, mutation=mutation):
                    self.viewer = copy.deepcopy(original)
                    mutation()
                    with self.assertRaises(ValueError):
                        self.check()

    def window_receipt(self, hdr=True):
        self.args.window_churn = True
        self.args.expected_dpr = 1.0
        source = Path(__file__).resolve().parents[2] / "tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg"
        self.args.fixture.write_bytes(source.read_bytes())
        self.fixture_hash = hashlib.sha256(source.read_bytes()).hexdigest()
        output, trace = self.viewer["report"], self.viewer["trace"]
        output["fixture_sha256"] = self.fixture_hash
        for name in ("hdr_frame", "last_frame"):
            output[name].update(fixture_sha256=self.fixture_hash, window_token="12345678-1234-1234-1234-123456789abc")
        if not hdr:
            self.args.expect = "sdr"
            self.cap["exit_code"] = 77
            self.cap["report"].update(status="sdr_fallback", reason="sdr_preferred", target_nits=203)
            output["last_frame"]["code"] = "wayland-sdr"
            trace.update(linear_commits=0, other_color_commits=0)
        output.update(window_actions=list(e2e.WINDOW_ACTIONS), hidden_confirmed=True, discarded_capture_serial=3)
        trace["actions"] = [dict(sequence=i+1, action=name, line=line)
                            for i, (name, line) in enumerate(zip(e2e.WINDOW_ACTIONS, (12, 22, 32, 48, 62)))]
        trace["retirements"] = {2: 40}
        sizes = ([1920, 1080], [32, 32], [37, 37], [37, 37], [1920, 1080])
        frames = []
        for i, (phase, size, commit) in enumerate(zip(e2e.WINDOW_PHASES, sizes, (10, 20, 30, 60, 80))):
            frame = dict(output["hdr_frame" if hdr else "last_frame"], phase=phase,
                         logical_size=list(size), pixel_size=list(size), device_pixel_ratio=1.0,
                         fullscreen=i in (0, 4), visibility=5 if i in (0, 4) else 2, frame_serial=4*i+1)
            gray = ((192/255+0.055)/1.055)**2.4*10 if hdr else 192/255
            x, y = (size[0]//2, size[1]//2) if i in (0, 4) else (4, 4)
            frame["readback"] = dict(format="rgba16f" if hdr else "rgba8", size=list(size),
                frame_serial=frame["frame_serial"], revision=frame["revision"], samples=[
                dict(x=x+offset, y=y, rgba=[gray, gray, gray, 1]) for offset in (-2, 0, 2)])
            frames.append(frame)
            lifetime = 2 if i < 3 else 50
            trace["phases"][phase] = dict(surface_id=14, revision=frame["revision"], encoding="linear" if hdr else "sdr",
                lifetime=lifetime, live_lifetimes=[lifetime], commit=commit, presented_commit=commit, observed=hdr)
        output["window_frames"] = frames
        trace["phases"]["hdr"].update(lifetime=50, commit=90, presented_commit=90)
        trace["phases"]["sdr"].update(lifetime=100, commit=120, presented_commit=120, hdr_role_live=False)

    def test_window_journey_rejects_wrong_pixels_geometry_source_and_lineage(self):
        self.window_receipt()
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])
        valid = copy.deepcopy(self.viewer)
        mutations = [
            lambda: self.viewer["report"]["window_frames"].pop(2),
            lambda: self.viewer["report"]["window_actions"].reverse(),
            lambda: self.viewer["report"].update(hidden_confirmed=False),
            lambda: self.viewer["report"].update(discarded_capture_serial=0),
            lambda: self.viewer["report"]["window_frames"][1].update(frame_serial=1),
            lambda: self.viewer["report"]["window_frames"][1].update(fullscreen=True),
            lambda: self.viewer["report"]["window_frames"][1].update(window_token="wrong"),
            lambda: self.viewer["report"]["window_frames"][1].update(fixture_sha256="a"*64),
            lambda: self.viewer["report"]["window_frames"][1].update(device_pixel_ratio=1.25),
            lambda: self.viewer["report"]["window_frames"][1]["readback"]["samples"][0].update(rgba=[0, 0, 0, 1]),
            lambda: self.viewer["report"]["window_frames"][1]["readback"]["samples"][0].update(rgba=[1, 1, 1, 1]),
            lambda: self.viewer["report"]["window_frames"][1]["readback"].update(size=[31, 31]),
            lambda: self.viewer["report"]["window_frames"][1]["readback"].update(frame_serial=1),
            lambda: self.viewer["report"]["window_frames"][1]["readback"].update(revision="old"),
            lambda: self.viewer["report"]["window_frames"][1]["readback"].update(format="rgba8"),
            lambda: self.viewer["trace"]["phases"]["window_floating"].update(presented_commit=0),
            lambda: self.viewer["trace"]["phases"]["window_floating"].update(commit=11),
            lambda: self.viewer["trace"]["phases"]["window_restored"].update(live_lifetimes=[2, 50]),
            lambda: self.viewer["trace"].update(retirements={}),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                self.viewer = copy.deepcopy(valid)
                mutate()
                with self.assertRaises(ValueError):
                    self.check()

    def test_sdr_window_journey_checks_its_pixels_and_presentations(self):
        self.window_receipt(hdr=False)
        self.assertEqual(self.check(), ["capabilities", "viewer"])
        self.viewer["report"]["window_frames"][3]["readback"]["samples"][1]["rgba"] = [1, 1, 1, 1]
        with self.assertRaisesRegex(ValueError, "Wrong window journey pixels"):
            self.check()

    def calibration_receipt(self, app_tag=False):
        self.args.calibrate = True
        if app_tag:
            self.args.expect = "hdr-app-tag"
            self.surface["report"].update(vk_color_space=1000104013, app_surface_tagged=True)
            self.surface["trace"]["linear_presented"] = 1
            self.viewer["report"]["hdr_frame"].update(vk_color_space=1000104013, app_surface_tagged=True)
            self.viewer["trace"]["sdr_presented_after_hdr"] = 1
        frame = dict(self.viewer["report"]["hdr_frame"], revision="11", paper_white_nits=300,
                     peak_nits=1200, ui_white_scale=3.75)
        proof = dict(self.viewer["trace"]["phases"]["hdr"], revision="11", commit=10, presented_commit=10)
        self.viewer["report"].update(calibration_frame=frame, used_calibration_controls=True)
        self.viewer["trace"]["phases"]["calibrated"] = proof

    def test_calibration_requires_controls_and_an_ordered_current_hdr_frame(self):
        self.calibration_receipt()
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])
        valid = copy.deepcopy(self.viewer)
        mutations = [
            lambda: self.viewer["report"].pop("calibration_frame"),
            lambda: self.viewer["report"].update(used_calibration_controls=False),
            lambda: self.viewer["report"]["calibration_frame"].update(ui_white_scale=2.5),
            lambda: self.viewer["report"]["calibration_frame"].update(photo_rendition="sdr"),
            lambda: self.viewer["report"]["calibration_frame"].update(fixture_sha256="b" * 64),
            lambda: self.viewer["report"]["calibration_frame"].update(revision="9"),
            lambda: self.viewer["trace"]["phases"]["calibrated"].update(lifetime=3),
            lambda: self.viewer["trace"]["phases"]["calibrated"].update(commit=13),
            lambda: self.viewer["trace"]["phases"]["calibrated"].update(encoding="sdr"),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(mutation=index):
                self.viewer = copy.deepcopy(valid)
                mutate()
                with self.assertRaises(ValueError):
                    self.check()

    def test_tagged_calibration_requires_its_own_presentation(self):
        self.calibration_receipt(app_tag=True)
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])
        for commit in (0, 9, 12):
            with self.subTest(presented_commit=commit):
                self.viewer["trace"]["phases"]["calibrated"]["presented_commit"] = commit
                with self.assertRaisesRegex(ValueError, "did not present the calibration frame"):
                    self.check()

    def test_app_tag_requires_both_receipt_and_wire_tag(self):
        self.args.expect = "hdr-app-tag"
        self.surface["report"].update(vk_color_space=1000104013, app_surface_tagged=True)
        self.surface["trace"]["linear_presented"] = 1
        active = self.viewer["report"]["hdr_frame"]
        active.update(vk_color_space=1000104013, app_surface_tagged=True)
        self.viewer["trace"]["sdr_presented_after_hdr"] = 1
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])
        self.viewer["report"]["last_frame"]["app_surface_tagged"] = True
        with self.assertRaisesRegex(ValueError, "expected SDR image"):
            self.check()
        self.viewer["report"]["last_frame"]["app_surface_tagged"] = False
        active["app_surface_tagged"] = False
        with self.assertRaisesRegex(ValueError, "did not use linear FP16"):
            self.check()
        active["app_surface_tagged"] = True
        self.surface["trace"]["last_toplevel_encoding"] = "sdr"
        with self.assertRaisesRegex(ValueError, "No accepted linear image description"):
            self.check()

    def test_sdr_controls(self):
        self.cap["exit_code"] = 77
        self.cap["report"].update(status="sdr_fallback", reason="sdr_preferred", target_nits=203)
        self.viewer["report"]["last_frame"]["code"] = "wayland-sdr"
        self.viewer["trace"]["linear_commits"] = 0
        self.args.expect = "sdr"
        self.assertEqual(self.check(), ["capabilities", "viewer"])
        self.cap["report"].update(color_manager=False, reason="no_color_manager")
        self.args.expect = "no-color-manager"
        self.assertEqual(self.check(), ["capabilities", "viewer"])

    def test_fallback_cannot_qualify_hdr(self):
        self.cap["exit_code"] = 77
        with self.assertRaisesRegex(ValueError, "exit 77"):
            self.check()

    def test_hdr_passthrough_requires_probe_refusal_and_an_eight_bit_viewer_frame(self):
        self.args.expect = "hdr-passthrough"
        self.surface["exit_code"] = 77
        self.surface["report"].update(status="sdr_fallback", reason="unverified_passthrough_color_space",
                                      vk_color_space=1000104013)
        self.surface["trace"]["linear_commits"] = 0
        self.viewer["report"]["last_frame"]["code"] = "unsupported"
        self.viewer["trace"]["linear_commits"] = 0
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])
        self.viewer["report"]["last_frame"].update(vk_format=97, vk_color_space=1000104013)
        with self.assertRaisesRegex(ValueError, "did not use an SDR Vulkan pair"):
            self.check()
        self.surface["report"]["vk_color_space"] = 1000104002
        with self.assertRaisesRegex(ValueError, "did not refuse unverified FP16 pass-through"):
            self.check()

    def test_matching_swapchain_without_commit_is_insufficient(self):
        self.surface["trace"]["last_toplevel_has_feedback"] = False
        with self.assertRaisesRegex(ValueError, "No accepted linear image description"):
            self.check()

    def test_hdr_requires_paper_white_and_a_subsequent_sdr_commit(self):
        self.viewer["report"]["hdr_frame"]["ui_white_scale"] = 1.0
        with self.assertRaisesRegex(ValueError, "paper-white scale"):
            self.check()
        self.viewer["report"]["hdr_frame"]["ui_white_scale"] = 2.5
        self.viewer["trace"]["last_toplevel_commit_follows_hdr"] = False
        with self.assertRaisesRegex(ValueError, "No SDR buffer commit followed"):
            self.check()
        self.viewer["trace"]["last_toplevel_encoding"] = "other"
        with self.assertRaisesRegex(ValueError, "expected SDR image"):
            self.check()

    def test_viewer_receipt_requires_the_fixture_and_the_build(self):
        report = self.viewer["report"]
        report["fixture"] = "/tmp/wrong.jpg"
        with self.assertRaisesRegex(ValueError, "different fixture"):
            self.check()
        report["fixture"] = str(self.args.fixture)
        report["build"]["generation"] = "foreign-executable"
        with self.assertRaisesRegex(ValueError, "different executable"):
            self.check()

    def test_final_phase_rejects_hdr_frames_and_stale_requests(self):
        final = self.viewer["report"]["last_frame"]
        final["photo_visible_hdr_frames"] = 9
        with self.assertRaisesRegex(ValueError, "expected SDR image"):
            self.check()
        final["photo_visible_hdr_frames"] = 0
        final["fixture_sha256"] = "wrong"
        with self.assertRaisesRegex(ValueError, "missing the current fixture request"):
            self.check()

    def test_phase_rejects_recreated_window_without_lineage(self):
        final = self.viewer["report"]["last_frame"]
        final["surface_id"] = 15
        proof = self.viewer["trace"]["phases"]["sdr"]
        proof.update(surface_id=15, lifetime=1, hdr_role_live=False)
        with self.assertRaisesRegex(ValueError, "same HDR window"):
            self.check()
        proof.update(lifetime=10, hdr_role_live=True)
        with self.assertRaisesRegex(ValueError, "same HDR window"):
            self.check()
        proof["hdr_role_live"] = False
        self.assertEqual(self.check(), ["capabilities", "surface", "viewer"])

    def test_viewer_receipts_require_actual_native_formats(self):
        self.viewer["report"]["hdr_frame"]["vk_color_space"] = 1000104013
        with self.assertRaisesRegex(ValueError, "did not use linear FP16"):
            self.check()
        self.viewer["report"]["last_frame"].update(vk_format=97, vk_color_space=1000104002)
        with self.assertRaisesRegex(ValueError, "did not use an SDR Vulkan pair"):
            self.check()


if __name__ == "__main__":
    unittest.main()
