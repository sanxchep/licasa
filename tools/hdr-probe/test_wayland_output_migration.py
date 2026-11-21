"""Reject stale output, hotplug, calibration and native pixel receipts."""

import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from urllib.parse import quote

import check_wayland_output_migration as check
from check_wayland_e2e import inspect_trace
from test_wayland_e2e import TOPLEVEL, BUFFER_COMMIT


class OutputReceipts(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        self.fixture, self.payload = root/"photo.jpg", root/"viewer"
        self.fixture.write_bytes((Path(__file__).resolve().parents[2]/
            "tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg").read_bytes())
        self.payload.write_bytes(b"test executable")
        self.seed = 42
        plan = check.action_plan(self.seed)
        report = dict(status="passed",drained=True,remaining_bytes=dict(decode=0,retained=0,upload=0,targets=0),
            completed_actions=100,seed=self.seed,plan=plan,fixture=str(self.fixture),
            fixture_sha256=check.GRAY_FIXTURE_SHA256,frames=[],soak=[],build=dict(
                executablePath=str(self.payload),generation=hashlib.sha256(self.payload.read_bytes()).hexdigest(),
                sourceReceipt="a"*64))
        trace = dict(phases={},output_actions=[],output_events=[],retirements={})
        self.run = dict(exit_code=0,report=report,trace=trace)
        bindings = dict(headless=1,sdr=2)
        for step in range(101):
            action = plan[step-1]["action"] if step else None
            line = step*100
            if action:
                trace["output_actions"].append(dict(step=step,action=action,line=line))
            if action and action.startswith(("on_","off_")):
                name = "headless" if action.endswith("hdr") else "sdr"
                if action.startswith("on_"):
                    bindings[name] = line+1
                trace["output_events"].append(dict(name=name,binding=bindings[name],
                    kind="add" if action.startswith("on_") else "remove",line=line+2))
            output, code, screens = check.expected_state(action)
            hdr = code == "hdr"
            gray = ((192/255+0.055)/1.055)**2.4*10 if hdr else 192/255
            phase, revision = f"output_{step}", str(step+3)
            frame = dict(phase=phase,revision=revision,old_revision=str(step+2),stale_rejected=True,
                output=output,screens=screens,code=code,image_ready=True,can_remember=False,fullscreen=False,
                window_token="00000000-0000-0000-0000-000000000001",frame_serial=line+20,surface_id=14,
                photo_rendition="hdr" if hdr else "sdr",photo_visible_hdr_frames=3 if hdr else 0,
                app_surface_tagged=hdr,vk_format=97 if hdr else 44,vk_color_space=1000104013,
                paper_white_nits=200 if hdr else 0,peak_nits=800 if hdr else 0,ui_white_scale=2.5 if hdr else 0,
                fixture_sha256=check.GRAY_FIXTURE_SHA256,photo_label="Photo",display_label="Display",
                current_request=f"image://licasa/1/{quote(str(self.fixture),safe='')}?licasa_stage=full",
                logical_size=[32,32],pixel_size=[32,32],device_pixel_ratio=1,photo_scale=1)
            frame["readback"] = dict(size=[32,32],revision=revision,frame_serial=line+20,
                format="rgba16f" if hdr else "rgba8",samples=[
                    dict(x=x,y=4,rgba=[gray,gray,gray,1]) for x in (2,4,6)])
            report["frames"].append(frame)
            members = [dict(name=output,binding=bindings[output],removed=0)]
            trace["phases"][phase] = dict(surface_id=14,revision=revision,lifetime=5,
                commit=line+20,presented_commit=line+19,encoding="linear" if hdr else "sdr",observed=hdr,
                entered_outputs=copy.deepcopy(members),presented_entered_outputs=copy.deepcopy(members),
                presented_on=copy.deepcopy(members),live_lifetimes=[5])
            if step <= 96 and step % 12 in (0, 9):
                hdr_phase = step % 12 == 9
                share = 8192 if hdr_phase else 0
                report["soak"].append(dict(step=step, phase="hdr" if hdr_phase else "sdr", threads=40,
                    rss_kib=300000.0, screens=2, windows=3,
                    malloc_live_kib=90000.0 + step * 10 + (4096 if hdr_phase else 0), receipt_bytes=step * 10 * 1024,
                    descriptors=dict(total=30, socket=3, eventfd=9, dmabuf=1, file=2),
                    pool=dict(decode=0, retained=share, upload=share, targets=share * 32222 if hdr_phase else 0)))

    def validate(self):
        check.validate(self.run,self.seed,self.fixture,self.payload)

    def reject_mutations(self, mutations):
        original = copy.deepcopy(self.run)
        for name, mutation in mutations:
            with self.subTest(name=name):
                self.run = copy.deepcopy(original)
                mutation()
                with self.assertRaises(ValueError):
                    self.validate()

    def soak_at(self, step):
        return next(item for item in self.run["report"]["soak"] if item["step"] == step)

    def rejects(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.validate()

    def test_longer_plan_repeats_the_cycle(self):
        long = check.action_plan(self.seed, 40)
        self.assertEqual(len(long), 12 * 40 + 4)
        self.assertEqual([entry["action"] for entry in long[:96]], [entry["action"] for entry in check.action_plan(self.seed)[:96]])
        self.assertEqual([entry["action"] for entry in long[-4:]], ["move_hdr", "calibrate", "move_sdr", "use_sdr"])

    def test_missing_measurement(self):
        del self.run["report"]["soak"][3]
        self.rejects("Missing or reordered resource measurements")

    def test_measurement_in_the_wrong_state(self):
        self.soak_at(21)["phase"] = "sdr"
        self.rejects("Missing measurements at action 21")

    def test_hdr_measurement_without_reservations(self):
        for item in self.run["report"]["soak"]:
            if item["phase"] == "hdr":
                item["pool"] = dict(decode=0, retained=0, upload=0, targets=0)
        self.rejects("holds no HDR reservations")

    def test_hdr_reservation_changed_in_a_later_cycle(self):
        self.soak_at(57)["pool"]["retained"] += 4096
        self.rejects("HDR reservations changed by action 57")

    def test_reservation_kept_after_a_cycle(self):
        self.soak_at(60)["pool"]["targets"] = 4096
        self.rejects("HDR reservations changed by action 60")

    def test_screen_left_after_hotplug(self):
        self.soak_at(48)["screens"] = 3
        self.rejects("Screens or top-level windows changed by action 48")

    def test_thread_kept_per_cycle(self):
        self.soak_at(96)["threads"] = 43
        self.rejects("Threads grew from 40 to 43 by action 96")

    def test_socket_kept_per_hotplug(self):
        item = self.soak_at(84)
        item["descriptors"]["socket"] = 6
        self.rejects("Open socket descriptors grew from 3 to 6 by action 84")

    def test_live_growth_beyond_the_receipt(self):
        self.soak_at(96)["malloc_live_kib"] += 20 * 1024
        self.rejects("beyond the receipt grew")

    def test_receipt_growth_is_not_a_leak(self):
        item = self.soak_at(96)
        item["malloc_live_kib"] += 20 * 1024
        item["receipt_bytes"] += 20 * 1024 * 1024
        self.validate()

    def test_complete_receipt(self):
        self.validate()

    def test_membership_and_presentation_are_independent_gates(self):
        self.reject_mutations((
            ("property only", lambda: self.run["trace"]["phases"]["output_3"].update(entered_outputs=[])),
            ("wrong output", lambda: self.run["trace"]["phases"]["output_3"]["entered_outputs"][0].update(name="headless")),
            ("no presentation", lambda: self.run["trace"]["phases"]["output_3"].update(presented_commit=0)),
            ("old presentation", lambda: self.run["trace"]["phases"]["output_3"].update(presented_commit=299)),
            ("other presentation", lambda: self.run["trace"]["phases"]["output_3"]["presented_on"][0].update(name="headless")),
            ("straddling", lambda: self.run["trace"]["phases"]["output_3"]["entered_outputs"].append(dict(name="headless"))),
        ))

    def test_hotplug_requires_native_removal_and_fresh_binding(self):
        self.reject_mutations((
            ("no hotplug", lambda: self.run["trace"].update(output_events=[])),
            ("remove wrong output", lambda: self.run["trace"]["output_events"][0].update(name="sdr")),
            ("late removal", lambda: self.run["trace"]["output_events"][0].update(line=699)),
            ("reused binding", lambda: self.run["trace"]["output_events"][1].update(binding=1)),
            ("old topology", lambda: self.run["report"]["frames"][6].update(screens=["headless","sdr"])),
        ))

    def test_calibration_source_and_pixels_cannot_be_stale(self):
        self.reject_mutations((
            ("old calibration", lambda: self.run["report"]["frames"][3].update(stale_rejected=False)),
            ("old revision", lambda: self.run["report"]["frames"][3].update(old_revision="1")),
            ("HDR review bypass", lambda: self.run["report"]["frames"][1].update(code="hdr")),
            ("stale white", lambda: self.run["report"]["frames"][3].update(ui_white_scale=2.5)),
            ("other window", lambda: self.run["report"]["frames"][3].update(window_token="00000000-0000-0000-0000-000000000002")),
            ("wrong sample", lambda: self.run["report"]["frames"][2]["readback"]["samples"][0].update(rgba=[0,0,0,1])),
            ("stale capture", lambda: self.run["report"]["frames"][2]["readback"].update(frame_serial=1)),
            ("wrong native SDR", lambda: self.run["report"]["frames"][3].update(vk_format=97)),
        ))

    def test_run_identity_order_and_drain_cannot_be_forged(self):
        self.reject_mutations((
            ("not drained", lambda: self.run["report"].update(drained=False)),
            ("target leak", lambda: self.run["report"]["remaining_bytes"].update(targets=1)),
            ("wrong executable", lambda: self.run["report"]["build"].update(generation="b"*64)),
            ("missing checkpoint", lambda: self.run["report"]["frames"].pop(30)),
            ("reordered actions", lambda: self.run["trace"]["output_actions"].reverse()),
            ("wrong seed", lambda: self.run["report"].update(seed=8)),
        ))


class OutputWireTrace(unittest.TestCase):
    def parse(self, content):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"trace"
            path.write_text(content)
            return inspect_trace(path)

    def test_output_binding_reuse_and_late_removal_preserve_past_frames(self):
        prefix = (' -> wl_registry#2.bind(21, "wl_output", 4, new id [unknown]#11)\n'
                  'wl_output#11.name("headless")\n'+TOPLEVEL+
                  'wl_surface#14.enter(wl_output#11)\n'+BUFFER_COMMIT+
                  'LICASA_WAYLAND_FRAME output_0 14 3\n')
        suffix = ('wl_registry#2.global_remove(21)\n'
                  ' -> wl_registry#2.bind(29, "wl_output", 4, new id [unknown]#11)\n'
                  'wl_output#11.name("headless")\n'+BUFFER_COMMIT+
                  'LICASA_WAYLAND_FRAME output_1 14 4\n'
                  'wl_surface#14.enter(wl_output#11)\n'+BUFFER_COMMIT+
                  'LICASA_WAYLAND_FRAME output_2 14 4\n')
        trace = self.parse(prefix+suffix)
        phases = trace["phases"]
        self.assertEqual(phases["output_0"]["entered_outputs"][0]["removed"],0)
        self.assertEqual(phases["output_1"]["entered_outputs"],[])
        self.assertNotEqual(phases["output_0"]["entered_outputs"][0]["binding"],
                            phases["output_2"]["entered_outputs"][0]["binding"])
        self.assertEqual([event["kind"] for event in trace["output_events"]],["add","remove","add"])

    def test_leave_and_sync_output_apply_to_the_correct_commit(self):
        trace = self.parse(' -> wl_registry#2.bind(21, "wl_output", 4, new id [unknown]#11)\n'
            'wl_output#11.name("headless")\n'+TOPLEVEL+
            'wl_surface#14.enter(wl_output#11)\n'
            ' -> wp_presentation#4.feedback(wl_surface#14, new id wp_presentation_feedback#30)\n'+BUFFER_COMMIT+
            'wp_presentation_feedback#30.sync_output(wl_output#11)\n'
            'wp_presentation_feedback#30.presented(0,0,0,0,0,0,0)\n'
            'wl_surface#14.leave(wl_output#11)\n'+BUFFER_COMMIT+
            'LICASA_WAYLAND_FRAME output_0 14 3\n')
        proof = trace["phases"]["output_0"]
        self.assertEqual(proof["entered_outputs"],[])
        self.assertEqual(proof["presented_on"][0]["name"],"headless")
        self.assertEqual(proof["presented_on"],proof["presented_entered_outputs"])

    def test_late_or_retired_output_feedback_cannot_prove_a_checkpoint(self):
        prefix = (' -> wl_registry#2.bind(21, "wl_output", 4, new id [unknown]#11)\n'
                  'wl_output#11.name("headless")\n'+TOPLEVEL+
                  'wl_surface#14.enter(wl_output#11)\n'
                  ' -> wp_presentation#4.feedback(wl_surface#14, new id wp_presentation_feedback#30)\n'+
                  BUFFER_COMMIT)
        late = self.parse(prefix+'LICASA_WAYLAND_FRAME output_0 14 3\n'
            'wp_presentation_feedback#30.sync_output(wl_output#11)\n'
            'wp_presentation_feedback#30.presented(0,0,0,0,0,0,0)\n')
        self.assertEqual(late["phases"]["output_0"]["presented_commit"],0)
        retired = self.parse(prefix+'wl_registry#2.global_remove(21)\n'
            'wl_surface#14.leave(nil)\nwp_presentation_feedback#30.sync_output(nil)\n'
            'wp_presentation_feedback#30.presented(0,0,0,0,0,0,0)\n'+BUFFER_COMMIT+
            'LICASA_WAYLAND_FRAME output_0 14 3\n')
        self.assertEqual(retired["errors"],[])
        self.assertEqual(retired["phases"]["output_0"]["presented_on"],[])
        self.assertEqual(retired["phases"]["output_0"]["entered_outputs"],[])


if __name__ == "__main__":
    unittest.main()
