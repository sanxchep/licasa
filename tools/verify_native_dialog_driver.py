#!/usr/bin/env python3
"""Drive real GTK file choosers through the installed production Qt application."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from verify_snap_desktop import pattern_present

REQUIRED_SCENARIOS = {
    "gtk_native_picker_confirmed", "native_keyboard_open", "native_escape_restores_focus",
    "native_keyboard_reopen_cancel", "native_picker_blocks_background_shortcuts",
    "native_selection_displays_photo", "native_save_cancel_preserves_edits",
    "native_save_output_decodes",
}


def gtk_picker_identity(*, toolkit: str, role: str, process_id: int, expected_pid: int,
                        mapped: list[str],
                        expected_plugin: str = "/usr/lib/x86_64-linux-gnu/qt6/plugins/platformthemes/libqgtk3.so",
                        gtk_library_root: str | None = None) -> dict:
    """GTK must own the chooser tree, in the same process as the GTK Qt theme."""
    themes = [path for path in mapped if path == expected_plugin]
    gtk = [path for path in mapped if "/libgtk-3.so." in path]
    if ("gtk" not in toolkit.lower() or role not in {"file chooser", "dialog"}
            or process_id != expected_pid or not themes or not gtk
            or (gtk_library_root and any(not Path(path).is_relative_to(gtk_library_root) for path in gtk))):
        raise RuntimeError("The visible chooser is not proven to be the native GTK picker")
    return {"toolkit": toolkit, "role": role, "process_id": process_id,
            "platform_theme_plugin": themes[0], "gtk_libraries": gtk}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bundle-root", type=Path, help="Require GTK and Qt libraries from this extracted AppImage.")
    args = parser.parse_args()
    import pyatspi
    from gi.repository import GLib
    from PIL import Image

    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"kind": "native-dialogs", "status": "failed", "desktop": "MATE", "protocol": "x11",
              "dialog_backend": "native-gtk", "scenarios": [], "native_confirmations": []}
    snapshots = []
    app = None
    log = args.output.with_suffix(".application.log").open("w")

    def pump():
        context = GLib.MainContext.default()
        for _ in range(30):
            if not context.pending():
                break
            context.iteration(False)

    def nodes(root):
        pending = [root]
        count = 0
        while pending and count < 3000:
            current = pending.pop(0)
            count += 1
            try:
                yield current
                pending.extend(current[index] for index in range(current.childCount))
            except Exception:
                continue

    def desktop():
        pump()
        return pyatspi.Registry.getDesktop(0)

    def owned_nodes():
        for application in desktop():
            if application:
                try:
                    if app and application.get_process_id() == app.pid:
                        yield from nodes(application)
                except Exception:
                    continue

    def showing(node):
        return node.getState().contains(pyatspi.STATE_SHOWING)

    def find(name, role=None):
        for node in owned_nodes():
            try:
                if node.name == name and showing(node) and (role is None or node.getRole() == role):
                    return node
            except Exception:
                continue
        return None

    def picker():
        for node in owned_nodes():
            try:
                role = node.getRoleName()
                if role in {"file chooser", "dialog"} and showing(node) and "gtk" in node.get_toolkit_name().lower():
                    return node
            except Exception:
                continue
        return None

    def wait(description, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if app and app.poll() is not None:
                raise RuntimeError(f"Application exited {app.returncode} during {description}")
            value = predicate()
            if value:
                return value
            time.sleep(0.1)
        raise RuntimeError(f"Timed out waiting for {description}")

    def action(node, wanted="press"):
        actions = node.queryAction()
        accepted = {"press", "click", "activate"} if wanted == "press" else {wanted}
        for index in range(actions.nActions):
            if any(name in actions.getName(index).lower() for name in accepted):
                if not actions.doAction(index):
                    raise RuntimeError(f"Action rejected by {node.name}")
                return
        raise RuntimeError(f"Missing {wanted} action on {node.name}")

    def key(value):
        subprocess.run(["xdotool", "key", "--clearmodifiers", value], check=True, timeout=4)
        time.sleep(0.2)

    def record(name, detail=""):
        report["scenarios"].append({"name": name, "status": "passed", "detail": detail})

    def snapshot(label):
        rows = []
        for node in owned_nodes():
            try:
                rows.append({"name": node.name, "role": node.getRoleName(),
                             "toolkit": node.get_toolkit_name(), "showing": showing(node),
                             "focused": node.getState().contains(pyatspi.STATE_FOCUSED)})
            except Exception:
                continue
        snapshots.append({"label": label, "nodes": rows})

    def confirm_native():
        chooser = wait("native GTK chooser", picker)
        mapped = []
        for line in Path(f"/proc/{app.pid}/maps").read_text().splitlines():
            fields = line.split(maxsplit=5)
            if len(fields) == 6 and fields[-1].startswith("/"):
                mapped.append(fields[-1])
        identity = gtk_picker_identity(toolkit=chooser.get_toolkit_name(), role=chooser.getRoleName(),
            process_id=chooser.get_process_id(), expected_pid=app.pid, mapped=sorted(set(mapped)),
            **({"expected_plugin": str(args.bundle_root / "usr/plugins/platformthemes/libqgtk3.so"),
                "gtk_library_root": str(args.bundle_root / "usr/lib")} if args.bundle_root else {}))
        report["process"] = {"mapped_qt_libraries": sorted({path for path in mapped if "/libQt6" in path})}
        if args.bundle_root and any(not Path(path).is_relative_to(args.bundle_root)
                                   for path in report["process"]["mapped_qt_libraries"]):
            raise RuntimeError("Native AppImage chooser loaded a host Qt library")
        report["native_confirmations"].append(identity)
        snapshot("native GTK chooser")
        subprocess.run(["import", "-window", "root", str(args.output.with_name(
            args.output.stem + f".picker-{len(report['native_confirmations'])}.png"))], check=True, timeout=8)
        return chooser

    def enter_path(path, button, *, overwrite=False):
        key("ctrl+l")
        # GTK Save As can select only the stem, leaving its existing extension.
        # Replace the complete entry so the automation cannot append it twice.
        key("ctrl+a")
        # Xvfb's US keymap cannot type every Unicode character through
        # xdotool. Use GTK's real editable-text interface for the path entry.
        entry = wait("focused native path entry", lambda: next((node for node in owned_nodes()
            if showing(node) and node.getRole() == pyatspi.ROLE_TEXT
            and node.getState().contains(pyatspi.STATE_FOCUSED)
            and "gtk" in node.get_toolkit_name().lower()), None))
        if not entry.queryEditableText().setTextContents(str(path)):
            raise RuntimeError("Native path entry rejected its accessible text input")
        key("Return")
        if overwrite:
            replace = wait("native overwrite confirmation", lambda: find("Replace", pyatspi.ROLE_PUSH_BUTTON))
            snapshot("overwrite confirmation")
            action(replace)
            record("native_overwrite_confirmation")
        # GTK sometimes validates the location before enabling the response.
        deadline = time.monotonic() + 3
        while picker() and time.monotonic() < deadline:
            time.sleep(0.1)
        if picker():
            selected = find(button, pyatspi.ROLE_PUSH_BUTTON)
            if selected:
                action(selected)
        wait("native GTK chooser acceptance", lambda: picker() is None)

    try:
        if (os.environ.get("QT_QPA_PLATFORMTHEME") != "gtk3"
                and not (args.bundle_root and not os.environ.get("QT_QPA_PLATFORMTHEME"))
                or os.environ.get("XDG_CURRENT_DESKTOP") != "MATE"
                or os.environ.get("LICASA_VIRTUAL_DESKTOP") != "1"
                or subprocess.run(["pgrep", "-x", "mate-session"], capture_output=True).returncode):
            raise RuntimeError("Use the disposable MATE qualification container with the matching GTK Qt platform theme")
        if args.destination.exists():
            raise RuntimeError("Refusing an existing native Save As output")
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        environment = dict(os.environ, QT_LINUX_ACCESSIBILITY_ALWAYS_ON="1")
        app = subprocess.Popen([str(args.app.resolve())], env=environment, stdout=log, stderr=subprocess.STDOUT)
        wait("default image", lambda: find("licasa-4096x4096.png", pyatspi.ROLE_IMAGE))
        window = subprocess.check_output(["xdotool", "search", "--onlyvisible", "--pid", str(app.pid), "--name", "^Licasa$"], text=True).splitlines()[-1]
        subprocess.run(["xdotool", "windowactivate", "--sync", window], check=True, timeout=5)
        key("ctrl+o")
        confirm_native()
        record("gtk_native_picker_confirmed")
        record("native_keyboard_open")
        if not any(node.getRole() == pyatspi.ROLE_COMBO_BOX and node.name.strip() == "Images"
                   for node in owned_nodes() if showing(node)):
            raise RuntimeError("Native Open picker does not expose its Images filter")
        record("native_open_filter_accessible")
        before = subprocess.check_output(["xprop", "-id", window, "_NET_WM_STATE"], text=True)
        key("F11")
        if subprocess.check_output(["xprop", "-id", window, "_NET_WM_STATE"], text=True) != before:
            raise RuntimeError("F11 changed the background application while the native picker was open")
        record("native_picker_blocks_background_shortcuts")
        key("Escape")
        wait("native Escape closes picker", lambda: picker() is None)
        wait("viewer focus restored", lambda: subprocess.check_output(
            ["xdotool", "getactivewindow"], text=True, timeout=3).strip() == window)
        record("native_escape_restores_focus")
        key("ctrl+o")
        confirm_native()
        cancel = wait("GTK Cancel button", lambda: find("Cancel", pyatspi.ROLE_PUSH_BUTTON))
        action(cancel)
        wait("native Cancel closes picker", lambda: picker() is None)
        record("native_keyboard_reopen_cancel")
        key("ctrl+o")
        confirm_native()
        enter_path(args.sample.resolve(), "Open")
        def visible_photo():
            image = args.output.with_suffix(".photo.png")
            subprocess.run(["import", "-window", "root", str(image)], check=True, timeout=8)
            with Image.open(image) as captured:
                return pattern_present(captured)
        wait("accepted native selection visibly displays the fixture", visible_photo)
        record("native_selection_displays_photo")
        key("e")
        exposure = wait("Exposure slider", lambda: find("Exposure", pyatspi.ROLE_SLIDER))
        initial = exposure.queryValue().currentValue
        action(exposure, "increase")
        wait("exposure edit", lambda: find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue > initial)
        edited = find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue
        key("ctrl+shift+s")
        confirm_native()
        key("Escape")
        wait("native Save As cancels", lambda: picker() is None)
        if args.destination.exists() or find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue != edited:
            raise RuntimeError("Native Save As cancellation changed the edit or created output")
        record("native_save_cancel_preserves_edits")
        key("ctrl+shift+s")
        confirm_native()
        enter_path(args.destination.absolute(), "Save")
        wait("native Save As output", lambda: args.destination.is_file() and args.destination.stat().st_size)
        with Image.open(args.destination) as saved, Image.open(args.sample) as original:
            saved.load()
            if saved.size != original.size or saved.convert("RGB").tobytes() == original.convert("RGB").tobytes():
                raise RuntimeError("Native Save As did not preserve the edited photo")
        report["saved_output"] = {"path": str(args.destination), "sha256": hashlib.sha256(args.destination.read_bytes()).hexdigest()}
        record("native_save_output_decodes")
        if args.bundle_root:
            # File creation precedes the viewer's save-completion handoff. Wait
            # for the new source and clean edit baseline before the next edit.
            wait("saved copy becomes the active clean image", lambda:
                 find(args.destination.name, pyatspi.ROLE_IMAGE)
                 and find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue == 0)
            record("native_saved_copy_ready")
            # Save As a copy deliberately refuses the active source, even after
            # GTK confirms replacement. Exercise overwrite on a distinct file.
            overwrite_destination = args.destination.with_name(
                args.destination.stem + " overwritten" + args.destination.suffix)
            overwrite_destination.write_bytes(args.destination.read_bytes())
            previous_digest = hashlib.sha256(overwrite_destination.read_bytes()).hexdigest()
            source_digest = hashlib.sha256(args.destination.read_bytes()).hexdigest()
            exposure = wait("Exposure after save", lambda: find("Exposure", pyatspi.ROLE_SLIDER))
            before_overwrite = exposure.queryValue().currentValue
            action(exposure, "increase")
            wait("edit before overwrite", lambda: find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue > before_overwrite)
            report["overwrite_edit"] = {"before": before_overwrite,
                "after": find("Exposure", pyatspi.ROLE_SLIDER).queryValue().currentValue}
            key("ctrl+shift+s")
            confirm_native()
            enter_path(overwrite_destination.absolute(), "Save", overwrite=True)
            wait("overwritten edited output", lambda: hashlib.sha256(overwrite_destination.read_bytes()).hexdigest() != previous_digest)
            with Image.open(overwrite_destination) as saved, Image.open(args.sample) as original:
                saved.load()
                if saved.size != original.size:
                    raise RuntimeError("Native overwrite produced an unexpected image")
            if hashlib.sha256(args.destination.read_bytes()).hexdigest() != source_digest:
                raise RuntimeError("Save As overwrite changed the active source")
            report["overwrite_output"] = {"path": str(overwrite_destination),
                "sha256": hashlib.sha256(overwrite_destination.read_bytes()).hexdigest(),
                "active_source_preserved": True}
            record("native_overwrite_output_decodes", str(overwrite_destination))
        report["status"] = "passed"
        return 0
    except Exception as error:
        report["error"] = str(error)
        snapshot("failure")
        return 1
    finally:
        report["atspi_snapshots"] = snapshots
        # Picker scenarios can leave intentional drafts. Capture their outcome
        # before attempting cleanup; a busy Quit is not a failed picker test.
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        if app and app.poll() is None:
            report["cleanup"] = {"qualifies_graceful_shutdown": False}
            try:
                quit_result = subprocess.run([str(args.app.resolve()), "--quit"], env=os.environ,
                                             stdout=log, stderr=subprocess.STDOUT, timeout=12)
                report["cleanup"]["quit_exit_code"] = quit_result.returncode
                try:
                    app.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    if os.environ.get("LICASA_VIRTUAL_DESKTOP") == "1":
                        app.kill()  # Owned virtual fixture only, after outcome capture.
                        app.wait(timeout=4)
                        report["cleanup"]["forced_owned_virtual_fixture"] = True
                    else:
                        report["cleanup"]["deferred_to_preserve_drafts"] = True
            except (OSError, subprocess.SubprocessError) as error:
                report["cleanup"]["error"] = str(error)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        log.close()


if __name__ == "__main__":
    raise SystemExit(main())
