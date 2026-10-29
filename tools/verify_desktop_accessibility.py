#!/usr/bin/env python3
"""Drive the installed application through AT-SPI in an existing desktop session.

Requires python3-pyatspi, xdotool, DBus and a running MATE session. This is a
bounded contract check for names, roles, actions, values and keyboard focus.
Orca's generated utterances can be checked by the container wrapper; listening
and human screen-reader acceptance still require an audio-enabled desktop.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def open_editor_after_settings(*, key, wait, find, atspi):
    """Exercise the keyboard transition using the actual desktop's state."""
    key("Escape")
    # The opposite drawer stays blocked until the closing animation finishes.
    # A fixed key delay can send E while the production shortcut is disabled.
    def editor_launcher_ready():
        launcher = find("Open editor", atspi.ROLE_PUSH_BUTTON)
        if launcher is None:
            return None
        state = launcher.getState()
        return launcher if (state.contains(atspi.STATE_ENABLED)
                            and state.contains(atspi.STATE_SENSITIVE)) else None
    wait("editor launcher after Settings closes", editor_launcher_ready)
    key("e")
    return wait("Exposure slider", lambda: find("Exposure", atspi.ROLE_SLIDER))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    import pyatspi
    from gi.repository import GLib
    report = {"status": "failed", "desktop": os.environ.get("XDG_CURRENT_DESKTOP", ""),
              "session": "virtual MATE X11" if os.environ.get("LICASA_VIRTUAL_DESKTOP") else "existing desktop",
              "scenarios": [], "coverage_limits": [
                  "AT-SPI contracts and Orca event processing are automated; audible speech order and usability require human acceptance.",
                  "Virtual display results do not qualify physical GPU, display-manager login, audio hardware or Wayland."]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    app = None
    log = args.output.with_suffix(".application.log").open("w")
    snapshots = []
    def pump():
        context = GLib.MainContext.default()
        for _ in range(20):
            if not context.pending(): break
            context.iteration(False)
    def nodes(root):
        pending = [root]; seen = 0
        while pending and seen < 2000:
            current = pending.pop(0); seen += 1
            try:
                yield current
                pending.extend(current[index] for index in range(current.childCount))
            except Exception:
                continue
    def desktop():
        pump()
        return pyatspi.Registry.getDesktop(0)
    def licasa():
        for candidate in desktop():
            if candidate and "licasa" in candidate.name.lower(): return candidate
        return None
    def owned_nodes():
        # GTK's native picker can register a second application root for the
        # same process, separate from the Qt Quick accessibility root.
        for candidate in desktop():
            if not candidate: continue
            try:
                if app is not None and candidate.get_process_id() == app.pid:
                    yield from nodes(candidate)
            except Exception: continue
    def find(name=None, role=None, showing=True):
        for candidate in owned_nodes():
            try:
                if ((name is None or candidate.name == name) and
                    (role is None or candidate.getRole() == role) and
                    (not showing or candidate.getState().contains(pyatspi.STATE_SHOWING))):
                    return candidate
            except Exception: continue
        return None
    def picker():
        return find(role=pyatspi.ROLE_FILE_CHOOSER) or find(role=pyatspi.ROLE_DIALOG)
    def wait(description, predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if app is not None and app.poll() is not None:
                raise RuntimeError(f"Application exited ({app.returncode}) during {description}")
            pump()
            value = predicate()
            if value: return value
            time.sleep(0.1)
        raise RuntimeError("Timed out: " + description)
    def action(control, wanted):
        interface = control.queryAction()
        names = [interface.getName(index) for index in range(interface.nActions)]
        for index, name in enumerate(names):
            if wanted.lower() in name.lower():
                if not interface.doAction(index): raise RuntimeError(f"Action {name} rejected by {control.name}")
                return
        raise RuntimeError(f"No {wanted} action on {control.name}: {names}")
    def key(keys):
        # Keep the active modal chooser focused. Activating its blocked parent
        # before every key can hang the window manager's activation handshake.
        subprocess.run(["xdotool", "key", "--clearmodifiers", keys], check=True, timeout=4)
        time.sleep(0.12)
    def record(name, detail=""):
        report["scenarios"].append({"name": name, "status": "passed", "detail": detail})
    def snapshot(label):
        result = []
        for control in owned_nodes():
            try:
                state = control.getState()
                result.append({"name": control.name, "role": control.getRoleName(),
                               "showing": state.contains(pyatspi.STATE_SHOWING),
                               "focused": state.contains(pyatspi.STATE_FOCUSED),
                               "checked": state.contains(pyatspi.STATE_CHECKED)})
            except Exception: continue
        snapshots.append({"label": label, "nodes": result})
    try:
        if "MATE" not in report["desktop"].upper():
            raise RuntimeError("This acceptance run requires a real running MATE session")
        session = subprocess.run(["pgrep", "-x", "mate-session"], capture_output=True, timeout=3)
        if session.returncode: raise RuntimeError("mate-session is not running")
        report["app_sha256"] = hashlib.file_digest(args.app.open("rb"), "sha256").hexdigest()
        env = os.environ.copy()
        env.update(QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1", QT_QUICK_BACKEND="software")
        app = subprocess.Popen([str(args.app.absolute())], env=env, stdout=log, stderr=subprocess.STDOUT)
        wait("AT-SPI application registration", licasa)
        wait("default image announced", lambda: find("licasa-4096x4096.png", pyatspi.ROLE_IMAGE))
        snapshot("default image")
        key("ctrl+o")
        chooser = wait("announced file picker", picker)
        record("default_image_named_and_keyboard_open")
        record("picker_dialog_role", chooser.name)
        snapshot("open picker")
        key("Escape")
        wait("picker closes", lambda: picker() is None)
        def viewer_has_focus():
            active = subprocess.check_output(["xdotool", "getactivewindow"], text=True, timeout=3).strip()
            return int(subprocess.check_output(["xdotool", "getwindowpid", active], text=True, timeout=3)) == app.pid
        wait("viewer focus returns", viewer_has_focus)
        record("cancel_restores_viewer_focus")
        key("ctrl+o")
        wait("keyboard reopen", picker)
        key("Escape")
        record("keyboard_open_cancel_reopen")
        # Single-instance command routes a real file-open request through the
        # installed app; it is never a substitute QML image source.
        subprocess.run([str(args.app.absolute()), str(args.sample.absolute())], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=8)
        wait("photo window", lambda: find(args.sample.name, pyatspi.ROLE_IMAGE))
        key("F11") if find("Open settings", pyatspi.ROLE_PUSH_BUTTON) is None else None
        settings = wait("settings button", lambda: find("Open settings", pyatspi.ROLE_PUSH_BUTTON))
        action(settings, "press")
        toggle = wait("Info overlay checkbox", lambda: find("Info overlay", pyatspi.ROLE_CHECK_BOX))
        previous = toggle.getState().contains(pyatspi.STATE_CHECKED)
        action(toggle, "toggle")
        wait("checkbox reports changed state", lambda: find("Info overlay", pyatspi.ROLE_CHECK_BOX).getState().contains(pyatspi.STATE_CHECKED) != previous)
        action(find("Info overlay", pyatspi.ROLE_CHECK_BOX), "toggle")
        record("toggle_role_state_action")
        opacity = wait("settings opacity slider", lambda: find("Background opacity", pyatspi.ROLE_SLIDER))
        opacity_values = opacity.queryValue()
        opacity_before = opacity_values.currentValue
        if (opacity_values.minimumValue != 0 or opacity_values.maximumValue != 100
                or opacity_values.minimumIncrement != 5 or "percent" not in opacity.description.lower()):
            raise RuntimeError("Settings slider does not expose readable displayed percent units")
        action(opacity, "increase" if opacity_before < 100 else "decrease")
        opacity_after = wait("settings value request", lambda:
            (value := find("Background opacity", pyatspi.ROLE_SLIDER).queryValue().currentValue)
            != opacity_before and (True, value))[1]
        if opacity_after != round(opacity_after):
            raise RuntimeError("Settings slider leaked a fractional model value")
        find("Background opacity", pyatspi.ROLE_SLIDER).queryValue().currentValue = opacity_before
        wait("settings direct value restores preference", lambda:
             find("Background opacity", pyatspi.ROLE_SLIDER).queryValue().currentValue == opacity_before)
        record("settings_readable_percent_values", f"{opacity_before} -> {opacity_after} -> {opacity_before}")
        snapshot("settings")
        exposure = open_editor_after_settings(key=key, wait=wait, find=find, atspi=pyatspi)
        record("settings_close_then_keyboard_editor_open",
               "Waited for the enabled editor launcher before pressing E.")
        values = exposure.queryValue()
        before = values.currentValue
        if values.minimumValue != -100 or values.maximumValue != 100 or values.minimumIncrement != 2:
            raise RuntimeError("Exposure numeric range or step missing from AT-SPI")
        if "percent" not in exposure.description.lower():
            raise RuntimeError("Exposure does not explain its accessible percent units")
        action(exposure, "increase")
        wait("screen-reader slider action updates actual value", lambda: find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue > before)
        record("adjustment_role_range_step_and_action")
        exposure = find("Exposure", pyatspi.ROLE_SLIDER)
        exposure.queryComponent().grabFocus()
        wait("slider keyboard focus", lambda: find("Exposure", pyatspi.ROLE_SLIDER).getState().contains(pyatspi.STATE_FOCUSED))
        before = exposure.queryValue().currentValue
        key("Right")
        wait("keyboard adjustment", lambda: find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue > before)
        record("adjustment_keyboard_focus_and_value")
        # Ten repeated additions of 0.02 reproduce the original binary tail
        # 0.19999999999999998. Reversing then exercises the other direction.
        # The interface must report whole displayed percent units,
        # matching the displayed +10%, without quantizing the edit model.
        samples = []
        def slider_sample(label, expected):
            actual = wait(label, lambda: (value := find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue)
                          == expected and (True, value))[1]
            samples.append({"action": label, "value": actual})
            # Give Orca's event queue a turn before the next intentional action.
            time.sleep(0.25)
        key("Home")
        slider_sample("reset to zero", 0)
        for _ in range(10): key("Right")
        slider_sample("ten positive steps", 20)
        for _ in range(5): key("Left")
        slider_sample("reverse positive steps", 10)
        key("Home")
        for _ in range(10): key("Left")
        slider_sample("ten negative steps", -20)
        for _ in range(5): key("Right")
        slider_sample("reverse negative steps", -10)
        key("Home")
        slider_sample("crossing returns to clean zero", 0)
        for expected in (100, -100):
            find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue = expected
            slider_sample("direct accessible endpoint " + str(expected), expected)
        reset = wait("named adjustment reset", lambda: find("Reset Exposure", pyatspi.ROLE_PUSH_BUTTON))
        action(reset, "press")
        slider_sample("accessible reset", 0)
        wait("reset returns focus to slider", lambda: find("Exposure", pyatspi.ROLE_SLIDER)
             .getState().contains(pyatspi.STATE_FOCUSED))
        report["slider_value_samples"] = samples
        record("adjustment_readable_percent_values_and_reset", "Positive, negative, zero, endpoints and direct value writes.")
        contrast = wait("second adjustment slider", lambda: find("Contrast", pyatspi.ROLE_SLIDER))
        contrast.queryComponent().grabFocus()
        key("Home")
        key("Right")
        wait("second adjustment reports displayed units", lambda:
             find("Contrast", pyatspi.ROLE_SLIDER).queryValue().currentValue == 2)
        record("second_adjustment_readable_value", "Contrast announces 2 percent after one step.")
        snapshot("editor")
        key("ctrl+shift+s")
        save_dialog = wait("Save As dialog", picker)
        if not save_dialog.name: raise RuntimeError("Save As dialog has no accessible name")
        record("save_as_dialog_name_and_role", save_dialog.name)
        key("Escape")
        wait("Save As closes", lambda: picker() is None)
        record("save_as_keyboard_cancel")
        key("c")
        crop = wait("named keyboard crop pane", lambda: find("Crop frame", pyatspi.ROLE_PANEL))
        if "arrow" not in crop.description.lower() or "escape" not in crop.description.lower():
            raise RuntimeError("Crop pane does not explain its keyboard controls")
        crop.queryComponent().grabFocus()
        key("alt+shift+Left")
        key("Return")
        wait("keyboard applies crop", lambda: find("Crop frame", pyatspi.ROLE_PANEL) is None)
        record("crop_keyboard_instructions_and_apply")
        # A real failed decode creates the production error notification. Check
        # the alert role/name/detail through the desktop accessibility bus.
        broken = Path(os.environ["HOME"]) / "accessibility-broken.png"
        broken.write_bytes(b"deliberately not a PNG")
        subprocess.run([str(args.app.absolute()), str(broken)], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=8)
        alert = wait("named image-error alert", lambda: find("Image could not be opened", pyatspi.ROLE_ALERT))
        if not alert.description:
            raise RuntimeError("Image-error alert has no actionable detail")
        record("notification_alert_name_and_detail", alert.description)
        snapshot("error alert")
        report["status"] = "passed"
        return 0
    except Exception as error:
        report["error"] = str(error)
        snapshot("failure")
        return 1
    finally:
        report["atspi_snapshots"] = snapshots
        report["application_log"] = str(args.output.with_suffix(".application.log"))
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        if app is not None:
            mappings = Path(f"/proc/{app.pid}/maps")
            try:
                report["process"] = {"mapped_qt_libraries": sorted({line.split(maxsplit=5)[5].strip()
                    for line in mappings.read_text().splitlines() if "libQt6" in line and len(line.split(maxsplit=5)) == 6})}
            except OSError as error:
                report["process_inspection_error"] = str(error)
        # These accessibility scenarios intentionally leave crop/edit drafts.
        # Persist their outcome before cleanup: safe Quit must preserve them.
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        if app is not None and app.poll() is None:
            report["cleanup"] = {"qualifies_graceful_shutdown": False}
            try:
                quit_result = subprocess.run([str(args.app.absolute()), "--quit"], stdout=log,
                                             stderr=subprocess.STDOUT, timeout=12)
                report["cleanup"]["quit_exit_code"] = quit_result.returncode
                try:
                    app.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    if os.environ.get("LICASA_VIRTUAL_DESKTOP") == "1":
                        app.kill()  # Only this owned disposable fixture, after outcome capture.
                        app.wait(timeout=4)
                        report["cleanup"]["forced_owned_virtual_fixture"] = True
                    else:
                        report["cleanup"]["deferred_to_preserve_drafts"] = True
            except (OSError, subprocess.SubprocessError) as error:
                report["cleanup"]["error"] = str(error)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        log.close()
        print(json.dumps({key: value for key, value in report.items() if key != "atspi_snapshots"}))

if __name__ == "__main__":
    raise SystemExit(main())
