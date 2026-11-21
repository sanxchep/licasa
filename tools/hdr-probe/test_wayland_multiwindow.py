"""Reject incorrect window identity, calibration, lifetime and pixel receipts."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_multiwindow as check


class MultiwindowReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.fixture = root / "photo.jpg"
        self.fixture.write_bytes((Path(__file__).resolve().parents[2] /
                                  "tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg").read_bytes())
        self.payload = root / "viewer"
        self.payload.write_bytes(b"test executable")
        self.seed = 42
        self.plan = check.action_plan(self.seed)
        report = dict(status="passed", drained=True, completed_actions=100, seed=self.seed, plan=self.plan,
                      remaining_bytes=dict(decode=0,retained=0,upload=0,targets=0),
                      fixture=str(self.fixture), fixture_sha256=check.GRAY_FIXTURE_SHA256,
                      build=dict(generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                                 executablePath=str(self.payload),sourceReceipt="a"*64),frames=[],soak=[])
        trace = dict(phases={},multi_actions=[],unmaps=[],retirements={})
        self.run = dict(exit_code=0,report=report,trace=trace)
        states = [dict(white=200,peak=800,hdr=True,high=False,revision=3,lifetime=1,generation=1),
                  dict(white=100,peak=200,hdr=True,high=False,revision=3,lifetime=2,generation=2)]
        for step in range(101):
            entry = self.plan[step-1] if step else None
            if entry:
                slot, action = entry["slot"],entry["action"]
                state = states[slot]
                line = step*100
                trace["multi_actions"].append(dict(step=step,slot=slot,action=action,line=line))
                if action == "toggle":
                    state["hdr"] = not state["hdr"]
                    state["revision"] += 1
                if action == "calibrate":
                    state["high"],state["hdr"] = not state["high"],True
                    state["white"] = (300 if state["high"] else 200) if slot == 0 else (150 if state["high"] else 100)
                    state["peak"] = state["white"]*(4 if slot == 0 else 2)
                    state["revision"] += 1
                if action == "hide":
                    trace["unmaps"].append(dict(lifetime=state["lifetime"],line=line+1))
                if action == "reopen":
                    trace["retirements"][state["lifetime"]] = line+1
                    state["lifetime"] = line+3
                    state["generation"] = step+2
                    state["revision"] = 3
                self.frame(states[1-slot],step,1-slot,"during",line+10)
            for slot in (0,1):
                self.frame(states[slot],step,slot,"settled",step*100+20+slot)
            if step % 50 == 0:
                windows = sum(1 for state in states if state["hdr"])
                report["soak"].append(dict(step=step, threads=54 if step == 0 else 52, rss_kib=400000.0,
                    malloc_live_kib=100000.0 + step * 20, receipt_bytes=step * 20 * 1024, windows=4,
                    outputs=[state["hdr"] for state in states],
                    descriptors=dict(total=29, socket=3, eventfd=9, dmabuf=1, file=2),
                    pool=dict(decode=0, retained=8192*windows, upload=8192*windows, targets=263946240*windows)))

    def frame(self, state, step, slot, stage, line):
        hdr = state["hdr"]
        revision = str(state["revision"])
        phase = f"multi_{step}_{slot}_{stage}"
        token = f"00000000-0000-0000-0000-{state['generation']:012d}"
        gray = ((192/255+0.055)/1.055)**2.4*(state["peak"]/80) if hdr else 192/255
        frame = dict(phase=phase,slot=slot,step=step,window_token=token,surface_id=state["lifetime"],
            frame_serial=line, revision=revision,output="headless",image_ready=True,fullscreen=False,
            can_remember=False,code="hdr" if hdr else "disabled",photo_rendition="hdr" if hdr else "sdr",
            photo_visible_hdr_frames=3 if hdr else 0,app_surface_tagged=hdr,
            vk_format=97 if hdr else 44,vk_color_space=1000104013,
            paper_white_nits=state["white"] if hdr else 0,peak_nits=state["peak"] if hdr else 0,
            ui_white_scale=state["white"]/80 if hdr else 0,
            fixture_sha256=check.GRAY_FIXTURE_SHA256,photo_label="Photo",display_label="Display",
            current_request=f"image://licasa/1/{quote(str(self.fixture),safe='')}?licasa_stage=full",
            logical_size=[32,32],pixel_size=[32,32],device_pixel_ratio=1)
        frame["readback"] = dict(size=[32,32],revision=revision,frame_serial=line,
            format="rgba16f" if hdr else "rgba8", samples=[
                dict(x=x,y=4,rgba=[gray,gray,gray,1]) for x in (2,4,6)])
        self.run["report"]["frames"].append(frame)
        self.run["trace"]["phases"][phase] = dict(surface_id=state["lifetime"],revision=revision,
            lifetime=state["lifetime"],commit=line,presented_commit=line,
            encoding="linear" if hdr else "sdr",observed=hdr,live_lifetimes=[state["lifetime"]])

    def validate(self):
        check.validate(self.run,self.seed,self.fixture,self.payload,"headless")

    def test_complete_receipt(self):
        self.validate()

    def soak_at(self, step):
        return next(item for item in self.run["report"]["soak"] if item["step"] == step)

    def rejects(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.validate()

    def test_longer_plan_extends_the_standard_one(self):
        self.assertEqual(check.action_plan(self.seed, 1000)[:100], self.plan)
        self.assertEqual(len(check.action_plan(self.seed, 1000)), 1000)

    def test_missing_measurement(self):
        del self.run["report"]["soak"][1]
        self.rejects("Missing or reordered resource measurements")

    def test_reservations_must_follow_hdr_windows(self):
        self.soak_at(100)["pool"]["targets"] += 4096
        self.rejects("HDR reservations at action 100")

    def test_thread_kept_per_window(self):
        self.soak_at(100)["threads"] = 55
        self.rejects("Threads grew from 52 to 55 by action 100")

    def test_photo_kept_open_per_window(self):
        item = self.soak_at(100)
        item["descriptors"]["photo"] = 3
        item["descriptors"]["total"] += 3
        self.rejects("Open photo descriptors grew from 0 to 3")

    def test_live_growth_beyond_the_receipt(self):
        self.soak_at(100)["malloc_live_kib"] += 20 * 1024
        self.rejects("beyond the receipt grew")

    def test_receipt_growth_is_not_a_leak(self):
        item = self.soak_at(100)
        item["malloc_live_kib"] += 20 * 1024
        item["receipt_bytes"] += 20 * 1024 * 1024
        self.validate()

    def test_extra_window(self):
        self.soak_at(50)["windows"] = 5
        self.rejects("Top-level windows changed")

    def test_self_consistent_peer_revision_change_is_rejected(self):
        phase = "multi_1_1_during"
        frame = next(frame for frame in self.run["report"]["frames"] if frame["phase"] == phase)
        frame["revision"] = frame["readback"]["revision"] = "999"
        self.run["trace"]["phases"][phase]["revision"] = "999"
        with self.assertRaisesRegex(ValueError, "Peer window changed revision"):
            self.validate()

    def test_wrong_window_pixels_policy_and_order_are_rejected(self):
        original = copy.deepcopy(self.run)
        mutations = (
            lambda: self.run["report"].update(drained=False),
            lambda: self.run["report"]["remaining_bytes"].update(targets=1),
            lambda: self.run["report"].update(seed=8),
            lambda: self.run["report"]["frames"].pop(30),
            lambda: self.run["trace"]["multi_actions"].reverse(),
            lambda: self.run["report"]["frames"][10].update(window_token="foreign"),
            lambda: self.run["report"]["frames"][10].update(paper_white_nits=450),
            lambda: self.run["report"]["frames"][10]["readback"]["samples"][0].update(rgba=[0,0,0,1]),
            lambda: self.run["report"]["frames"][10]["readback"].update(frame_serial=1),
            lambda: self.run["trace"]["phases"]["multi_3_1_during"].update(presented_commit=0),
            lambda: self.run["report"]["frames"][10].update(revision="999"),
            lambda: self.run["trace"].update(unmaps=[]),
            lambda: self.run["trace"].update(retirements={}),
            lambda: self.run["report"]["build"].update(generation="b"*64),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.run = copy.deepcopy(original)
                mutation()
                with self.assertRaises(ValueError):
                    self.validate()

    def test_wire_markers_keep_both_windows_distinct(self):
        from check_wayland_e2e import inspect_trace
        from test_wayland_e2e import TOPLEVEL, FEEDBACK, LINEAR_TAG, BUFFER_COMMIT
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory)/"trace.log"
            trace.write_text(TOPLEVEL+FEEDBACK+LINEAR_TAG+BUFFER_COMMIT+
                             "LICASA_WAYLAND_FRAME multi_0_0_settled 14 3\n"+
                             "LICASA_MULTI_ACTION 1 1 toggle\n")
            observed = inspect_trace(trace)
        self.assertIn("multi_0_0_settled",observed["phases"])
        self.assertEqual(observed["multi_actions"][0]["slot"],1)
        self.assertEqual(observed["multi_actions"][0]["action"],"toggle")


if __name__ == "__main__":
    unittest.main()
