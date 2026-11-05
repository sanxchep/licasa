#!/usr/bin/env python3
"""Qualify installed Licasa dialogs without borrowing developer QML imports.

The companion diagnostic executes the package's production Main.qml, actual Qt
fallback dialogs, image provider and asynchronous save service. It asserts visible
content and decoded output, beyond process survival. All runs are bounded and use
isolated user state; callers may provide Xvfb/Weston via DISPLAY/WAYLAND_DISPLAY.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

REQUIRED_SCENARIOS = {
    "keyboard_open_visible_fallback", "picker_blocks_background_shortcuts", "cancel_returns_focus",
    "ctrl_o_reopens_visible_fallback", "accepted_photo_displayed",
    "save_as_visible_fallback", "save_cancel_creates_no_output", "save_as_output_decodes",
    "lazy_FrameSaveDialog_visible", "lazy_MotionExportDialog_visible",
    "lazy_AnimationExportDialog_visible",
}
RECOVERY_SCENARIOS = {
    "missing_module_recoverable_error", "missing_module_preserves_photo_and_edits",
    "missing_module_retry_preserves_state",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def application_executable(app: Path) -> Path:
    """Resolve the native launch gate's payload without bypassing its launch."""
    payload = Path(str(app) + ".bin")
    return payload if payload.is_file() else app


def stop(process: subprocess.Popen | None) -> None:
    if process is not None and process.poll() is None:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)


def parse_report(stdout: str) -> dict:
    # Qt writes diagnostics to stderr; allow preceding non-JSON information
    # while requiring one complete report at the end of stdout.
    decoder = json.JSONDecoder()
    for position, character in enumerate(stdout):
        if character == "{":
            try:
                report, end = decoder.raw_decode(stdout[position:])
            except json.JSONDecodeError:
                continue
            if not stdout[position + end:].strip() and isinstance(report, dict):
                return report
    raise RuntimeError("Installed diagnostic did not emit a complete dialog report")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--diagnostic", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--protocol", choices=("x11", "wayland"), default="x11")
    parser.add_argument("--mode", choices=("default", "software"), default="software")
    parser.add_argument("--dialog-backend", choices=("fallback", "native"), default="fallback")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--expect-unavailable", action="store_true")
    parser.add_argument("--work-directory", type=Path, help="Visible working parent directory (needed for confined packages).")
    parser.add_argument("--preserve-desktop-runtime", action="store_true", help="Keep the caller desktop runtime directory, including on X11.")
    args = parser.parse_args()
    args.output = args.output.absolute()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "status": "failed", "protocol": args.protocol,
              "mode": args.mode, "dialog_backend": args.dialog_backend, "scenarios": [],
              "coverage_limits": [
                  "Virtual displays do not qualify a physical GPU or a real MATE/GNOME/Plasma login.",
                  "Lazy frame/video/animation picker checks prove imports and cancellation; media output correctness is covered by the separate export suites.",
                  "Screen-reader usability requires the separate AT-SPI/Orca desktop acceptance run."]}
    app_process = display_process = None
    try:
        for path in (args.app, args.diagnostic, args.sample):
            if not path.is_file():
                raise RuntimeError(f"Missing qualification input: {path}")
        report.update(app_sha256=sha256(application_executable(args.app)),
                      app_launcher_sha256=sha256(args.app),
                      app_executable_path=str(application_executable(args.app)),
                      diagnostic_sha256=sha256(args.diagnostic),
                      sample_sha256=sha256(args.sample))
        if args.dialog_backend == "native":
            report.update(status="unsupported", error="Native picker qualification requires an interactive desktop driver; fallback is never accepted as native.")
            return 2
        if not 10 <= args.timeout <= 300:
            raise RuntimeError("--timeout must be between 10 and 300 seconds")
        with tempfile.TemporaryDirectory(prefix="licasa-dialogs-", dir=args.work_directory) as temporary:
            root = Path(temporary)
            environment = os.environ.copy()
            for name in ("QT_QUICK_BACKEND", "QSG_RHI_BACKEND", "QML_IMPORT_PATH", "QML2_IMPORT_PATH",
                         "QT_PLUGIN_PATH", "QT_QPA_PLATFORMTHEME"):
                environment.pop(name, None)
            for name, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                    ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"),
                                    ("XDG_RUNTIME_DIR", "runtime")):
                path = root / directory
                path.mkdir(mode=0o700)
                environment[name] = str(path)
            # An invalid host hint must not be needed for any package import.
            environment["QML_IMPORT_PATH"] = str(root / "no-host-qml")
            environment["QML2_IMPORT_PATH"] = str(root / "no-host-qml2")
            environment["QT_QPA_PLATFORM"] = "xcb" if args.protocol == "x11" else "wayland"
            environment["QT_QPA_PLATFORMTHEME"] = "licasa-qualification-no-native-theme"
            if args.mode == "software":
                environment["QT_QUICK_BACKEND"] = "software"
            if args.protocol == "wayland" and not environment.get("WAYLAND_DISPLAY"):
                report.update(status="unsupported", error="WAYLAND_DISPLAY is required; run in the bounded Weston qualification session.")
                return 2
            if args.protocol == "x11" and not environment.get("DISPLAY"):
                xvfb = shutil.which("Xvfb")
                if not xvfb:
                    raise RuntimeError("Xvfb or an existing DISPLAY is required")
                display_process = subprocess.Popen([xvfb, "-displayfd", "1", "-screen", "0", "1280x900x24", "-nolisten", "tcp"],
                                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                                   text=True, start_new_session=True)
                import select
                ready, _, _ = select.select([display_process.stdout], [], [], 10)
                if not ready:
                    raise RuntimeError("Xvfb did not become ready")
                display = display_process.stdout.readline().strip()
                if not display.isdigit():
                    raise RuntimeError("Xvfb did not allocate a display")
                environment["DISPLAY"] = ":" + display
            # Wayland socket lives in the caller's runtime directory. Keep that
            # one directory while still isolating application config/state.
            if args.protocol == "wayland" or args.preserve_desktop_runtime:
                environment["XDG_RUNTIME_DIR"] = os.environ["XDG_RUNTIME_DIR"]
            app_log = args.output.with_suffix(".application.log")
            with app_log.open("w") as log:
                app_process = subprocess.Popen([str(args.app.absolute())], env=environment,
                                               stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                deadline = time.monotonic() + min(8, args.timeout / 4)
                window = None
                while time.monotonic() < deadline:
                    if app_process.poll() is not None:
                        raise RuntimeError(f"Application exited during launch: {app_process.returncode}; see {app_log}")
                    if args.protocol == "x11" and shutil.which("xdotool"):
                        found = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^Licasa$"], env=environment,
                                               capture_output=True, text=True, timeout=3)
                        if found.returncode == 0:
                            window = found.stdout.splitlines()[0]
                            break
                    elif time.monotonic() > deadline - min(5, args.timeout / 5):
                        break
                    time.sleep(0.1)
                if window:
                    subprocess.run(["xdotool", "windowfocus", "--sync", window], env=environment, check=True, timeout=3)
                    subprocess.run(["xdotool", "key", "--window", window, "ctrl+o"], env=environment, check=True, timeout=3)
                    time.sleep(1)
                    if app_process.poll() is not None:
                        raise RuntimeError(f"Application crashed opening picker: {app_process.returncode}; see {app_log}")
                    subprocess.run(["xdotool", "key", "--window", window, "Escape"], env=environment, check=True, timeout=3)
                report["scenarios"].append({"name": "application_launch", "status": "passed", "detail": str(app_log)})
                # Request normal application shutdown first, including when
                # confinement denies host-side kill signals.
                subprocess.run([str(args.app.absolute()), "--quit"], env=environment,
                               stdout=log, stderr=subprocess.STDOUT, timeout=8)
                try:
                    app_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    stop(app_process)
                app_process = None
            outputs = root / "output"
            command = [str(args.diagnostic.absolute()), "--viewer", "--dialog-interactions",
                       "--dialog-sample", str(args.sample.absolute()), "--dialog-output-dir", str(outputs)]
            if args.expect_unavailable:
                command.append("--dialog-expect-unavailable")
            result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=args.timeout)
            log = args.output.with_suffix(".runtime.log")
            log.write_text(result.stdout + "\n--- Qt diagnostics ---\n" + result.stderr)
            report.update(exit_code=result.returncode, runtime_log=str(log))
            runtime = parse_report(result.stdout)
            report["runtime"] = runtime
            scenarios = runtime.get("scenarios", [])
            report["scenarios"].extend(scenarios)
            required = RECOVERY_SCENARIOS if args.expect_unavailable else REQUIRED_SCENARIOS
            passed = {case.get("name") for case in scenarios if case.get("status") == "passed"}
            if result.returncode or runtime.get("status") != "passed" or not required <= passed:
                raise RuntimeError(f"Dialog qualification failed; missing scenarios {sorted(required - passed)}; {runtime.get('error', '')}; see {log}")
            if not args.expect_unavailable:
                saved = outputs / "saved.png"
                if not saved.is_file() or not saved.stat().st_size:
                    raise RuntimeError("Production Save As did not create output")
                retained = args.output.with_suffix(".saved.png")
                shutil.copyfile(saved, retained)
                report["saved_image"] = {"path": str(retained), "sha256": sha256(retained)}
            report["status"] = "passed"
            return 0
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        return 1
    finally:
        stop(app_process)
        stop(display_process)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report))


if __name__ == "__main__":
    raise SystemExit(main())
