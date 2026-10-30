#!/usr/bin/env python3
"""Qualify final-package Motion Photo playback, frame saving and teardown."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
JPEG = "formats/core-matrix/jpeg/full-hd-1920x1080.jpg"
REQUIRED_CONTROLS = frozenset((
    "tray_visible_before_play", "multimedia_absent_before_play", "frame_step_disabled",
    "first_presentation_frame_visible", "play_from_visible_control", "pause_from_visible_control",
    "timeline_state_mirrored", "multimedia_loaded_after_play", "transport_interaction_enabled_during_active_motion",
    "scrub_from_visible_control", "scrub_presentation_frame_visible", "resume_from_visible_control",
    "floating_transition_preserves_playback", "floating_single_click_pauses", "floating_has_no_transport_object",
    "floating_pause_intent_observed", "floating_resume_intent_observed",
    "paused_double_click_preserves_position", "paused_floating_transition_preserves_position",
    "floating_single_click_resumes", "floating_double_click_restores_fullscreen_without_pause",
    "editor_transition_stops_playback", "editor_transition_removes_tray", "tray_returns_after_editor",
    "replay_after_editor", "replay_control_ready", "captured_frame_saved_and_decoded", "frame_save_releases_playback",
    "clear_removes_motion_authority", "clear_removes_tray", "clear_removes_motion_surface",
))
FFMPEG = {
    "6.4.2": ("libavcodec.so.60", "libavformat.so.60", "libavutil.so.58", "libswresample.so.4", "libswscale.so.7"),
    "6.8.2": ("libavcodec.so.61", "libavformat.so.61", "libavutil.so.59", "libswresample.so.5", "libswscale.so.8"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_fixture(destination: Path, rotation_degrees: int, assets: Path | None = None) -> dict:
    """Append a pinned playable MP4 behind valid modern Android Motion XMP."""
    if rotation_degrees not in (0, 90):
        raise ValueError("Only qualified 0/90 degree fixtures are available")
    assets = assets or ROOT / "tests/test-assets"
    source = assets / "modern/motion" / f"qt-runtime-{rotation_degrees}deg.mp4"
    hashes = json.loads((source.parent / "sha256.json").read_text())
    if digest(source) != hashes[source.name]:
        raise RuntimeError("Motion Photo source fixture hash mismatch")
    jpeg, video = (assets / JPEG).read_bytes(), source.read_bytes()
    if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
        raise RuntimeError("The Motion Photo still fixture must be a complete JPEG")
    xmp = f'''<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"><rdf:Description xmlns:GCamera="http://ns.google.com/photos/1.0/camera/" xmlns:Container="http://ns.google.com/photos/1.0/container/" xmlns:Item="http://ns.google.com/photos/1.0/container/item/" GCamera:MotionPhoto="1" GCamera:MotionPhotoVersion="1" GCamera:MotionPhotoPresentationTimestampUs="0"><Container:Directory><rdf:Seq><rdf:li rdf:parseType="Resource"><Container:Item Item:Mime="image/jpeg" Item:Semantic="Primary" Item:Length="0"/></rdf:li><rdf:li rdf:parseType="Resource"><Container:Item Item:Mime="video/mp4" Item:Semantic="MotionPhoto" Item:Length="{len(video)}"/></rdf:li></rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>'''.encode()
    payload = b"http://ns.adobe.com/xap/1.0/\0" + xmp
    if len(payload) + 2 > 65535:
        raise RuntimeError("Motion XMP exceeds the JPEG APP1 limit")
    still = jpeg[:2] + b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload + jpeg[2:]
    destination.write_bytes(still + video)
    return {"sha256": digest(destination), "video_offset": len(still), "video_bytes": len(video),
            "video_sha256": digest(source), "embedded_custom_io": True,
            "rotation_degrees": rotation_degrees}


def validate_controls(controls: dict, runtime_root: str, qt_version: str,
                      rotation_degrees: int, version_probe: dict | None = None) -> dict:
    missing = sorted(name for name in REQUIRED_CONTROLS if controls.get(name) is not True)
    if missing or controls.get("error") or controls.get("final_session_error"):
        raise RuntimeError(f"Incomplete packaged Motion lifecycle: {missing}; {controls.get('error', '')}")
    if any(controls.get(name) is not False for name in (
            "final_session_available", "final_session_active", "final_session_playing", "final_control_present")):
        raise RuntimeError("Motion authority or controls survived the final clear")
    dimensions = (240, 320) if rotation_degrees == 90 else (320, 240)
    if (controls.get("saved_frame_width"), controls.get("saved_frame_height")) != dimensions:
        raise RuntimeError("Packaged Motion frame has incorrect display orientation or dimensions")
    metrics = controls.get("process_after_clear", {})
    optional = metrics.get("mapped_optional_libraries", [])
    qt = metrics.get("mapped_qt_libraries", [])
    root = Path(runtime_root)
    allowed = (Path("/usr/lib/x86_64-linux-gnu"), Path("/usr/lib/licasa")) if root == Path("/usr") else (root,)
    for path in [*optional, *qt]:
        if (not isinstance(path, str) or not Path(path).is_absolute() or ".." in Path(path).parts
                or not any(Path(path).is_relative_to(prefix) for prefix in allowed)):
            raise RuntimeError(f"Motion runtime escaped the qualified package: {path}")
    private = [path for path in optional if Path(path).name == "libffmpegmediaplugin.so"]
    expected_private = {root / "lib/licasa/plugins/multimedia/libffmpegmediaplugin.so"}
    if root != Path("/usr"):
        # AppDir normalizes Qt plugins here; confined native-style installs
        # retain lib/licasa/plugins. Neither may borrow a system Qt backend.
        expected_private.add(root / "plugins/multimedia/libffmpegmediaplugin.so")
    if len(private) != 1 or Path(private[0]) not in expected_private:
        raise RuntimeError(f"Motion playback did not use the installed private FFmpeg plugin: {private}")
    portable = root != Path("/usr")
    probe_qt = []
    if portable:
        if not version_probe or version_probe.get("qt_version") != qt_version:
            raise RuntimeError("Portable Motion playback lacks an exact runtime Qt version probe")
        probe_qt = version_probe.get("before_decode", {}).get("mapped_qt_libraries", [])
        if not isinstance(probe_qt, list) or any(
                not isinstance(path, str) or not Path(path).is_absolute()
                or ".." in Path(path).parts or not Path(path).is_relative_to(root) for path in probe_qt):
            raise RuntimeError("Motion Qt version probe escaped the qualified package")
    for component in ("Core", "Qml", "Quick"):
        names = {f"libQt6{component}.so.{qt_version}"}
        if portable:
            names.add(f"libQt6{component}.so.6")
        selected = [path for path in qt if Path(path).name in names]
        if len(selected) != 1:
            raise RuntimeError(f"Motion playback loaded the wrong Qt {component} runtime")
        if portable and [path for path in probe_qt if Path(path).name in names] != selected:
            raise RuntimeError(f"Motion playback and its Qt {component} version probe loaded different libraries")
    multimedia_names = {f"libQt6Multimedia.so.{qt_version}"}
    if portable:
        multimedia_names.add("libQt6Multimedia.so.6")
    if len([path for path in optional if Path(path).name in multimedia_names]) != 1:
        raise RuntimeError("Motion playback loaded the wrong Qt Multimedia runtime")
    for soname in FFMPEG[qt_version]:
        if not any(Path(path).name == soname or Path(path).name.startswith(soname + ".") for path in optional):
            raise RuntimeError(f"Motion playback is missing qualified FFmpeg ABI {soname}")
    return {"mapped_optional_libraries": optional, "mapped_qt_libraries": qt,
            "private_ffmpeg_plugin": private[0]}


def installed_check(args, environment: dict | None = None) -> dict:
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ if environment is None else environment)
    environment.update(QT_MEDIA_BACKEND="ffmpeg", QT_FFMPEG_DECODING_HW_DEVICE_TYPES=",")
    # Direct diagnostics need AppRun's loader paths for the lazily loaded Motion
    # backend and its Qt Multimedia dependency, whose RUNPATH is not transitive.
    if Path(args.runtime_root) != Path("/usr"):
        inherited_libraries = environment.get("LD_LIBRARY_PATH")
        environment["LD_LIBRARY_PATH"] = ":".join(str(Path(args.runtime_root) / path)
                                                 for path in ("lib/licasa/lib", "lib"))
        if inherited_libraries:
            environment["LD_LIBRARY_PATH"] += ":" + inherited_libraries
    environment["QT_PLUGIN_PATH"] = ":".join(str(Path(args.runtime_root) / path) for path in (
        "plugins", "lib/licasa/plugins", "lib/qt6/plugins"))
    environment["QML_IMPORT_PATH"] = ":".join(str(Path(args.runtime_root) / path) for path in ("qml", "lib/qt6/qml"))
    environment["QML2_IMPORT_PATH"] = environment["QML_IMPORT_PATH"]
    if args.mode == "software":
        environment["QT_QUICK_BACKEND"] = "software"
    else:
        environment.pop("QT_QUICK_BACKEND", None)
    # AppDir stores Qt libraries by SONAME, removing their full-version realfile
    # names. Obtain qVersion() from the same diagnostic/environment, then require
    # its Core/Qml/Quick mappings to match the subsequent Motion process.
    probe = subprocess.run([str(args.diagnostic), "--formats", "--megapixels", "25", str(args.fixture)],
                           env=environment, text=True, capture_output=True, timeout=30)
    probe_output = output.with_suffix(".runtime-profile.json")
    probe_log = output.with_suffix(".runtime-profile.log")
    probe_output.write_text(probe.stdout)
    probe_log.write_text(probe.stderr)
    if probe.returncode:
        raise RuntimeError(f"Installed Motion runtime profile probe exited {probe.returncode}")
    version_probe = json.loads(probe.stdout)
    if (version_probe.get("qt_version") != args.qt_version
            or not version_probe.get("samples") or version_probe["samples"][0].get("decoded") is not True):
        raise RuntimeError("Installed Motion runtime profile probe did not prove its exact Qt version")
    frame = output.parent / "motion-frame.png"
    command = [str(args.diagnostic), "--viewer", "--motion-photo-controls", "--motion-frame-output",
               str(frame), "--megapixels", "25", str(args.fixture)]
    result = subprocess.run(command, env=environment, text=True, capture_output=True, timeout=50)
    output.with_suffix(".stdout.json").write_text(result.stdout)
    output.with_suffix(".stderr.log").write_text(result.stderr)
    if result.returncode:
        try:
            failure = json.loads(result.stdout)
        except json.JSONDecodeError:
            failure = None
        detail = ""
        if isinstance(failure, dict) and failure.get("error"):
            detail = str(failure["error"])[:500]
            state = failure.get("final_state")
            if isinstance(state, dict) and state.get("stage"):
                detail += f" (stage {str(state['stage'])[:80]})"
            detail += "; "
        raise RuntimeError(f"Installed Motion diagnostic exited {result.returncode}: {detail}{result.stderr[-500:]}")
    if "Attempting to create QWindow-based QOffscreenSurface outside the gui thread" in result.stderr:
        raise RuntimeError("Installed Motion conversion created a native surface outside the GUI thread")
    controls = json.loads(result.stdout)
    runtime = validate_controls(controls, args.runtime_root, args.qt_version, args.rotation_degrees, version_probe)
    if not frame.is_file() or frame.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("Motion diagnostic did not retain its decoded PNG output")
    runtime["private_ffmpeg_sha256"] = digest(Path(runtime["private_ffmpeg_plugin"]))
    if Path(args.runtime_root) != Path("/usr"):
        # linuxdeploy can also copy the distro's unpatched FFmpeg plugin into
        # canonical Qt plugin paths. Bind the actual loaded bytes to CMake's
        # retained, provenance-checked private runtime after package transforms.
        reference = Path(args.runtime_root) / "lib/licasa/plugins/multimedia/libffmpegmediaplugin.so"
        if not reference.is_file():
            raise RuntimeError("Portable Motion runtime omitted its qualified private FFmpeg reference")
        runtime["private_ffmpeg_reference"] = str(reference)
        runtime["private_ffmpeg_reference_sha256"] = digest(reference)
        if runtime["private_ffmpeg_sha256"] != runtime["private_ffmpeg_reference_sha256"]:
            raise RuntimeError("Motion loaded a different FFmpeg plugin from the qualified private runtime")
    report = {"status": "passed", "protocol": args.protocol, "mode": args.mode,
              "rotation_degrees": args.rotation_degrees, "qt_version": args.qt_version,
              "diagnostic_sha256": digest(args.diagnostic), "controls": controls,
              "runtime_probe": {"qt_version": version_probe["qt_version"],
                                "mapped_qt_libraries": version_probe.get("before_decode", {}).get("mapped_qt_libraries", []),
                                "report_path": str(probe_output), "report_sha256": digest(probe_output),
                                "log_path": str(probe_log)},
              "scenarios": [{"name": name, "status": "passed"} for name in sorted(REQUIRED_CONTROLS)],
              "runtime": runtime, "saved_frame": {"path": str(frame), "sha256": digest(frame),
              "width": controls["saved_frame_width"], "height": controls["saved_frame_height"]}}
    output.write_text(json.dumps(report, indent=2) + "\n")
    return report


def run_motion_checks(image: str, *, diagnostic: str, runtime_root: str, output_dir: Path,
                      setup: str = "", qt_version: str = "6.4.2") -> dict:
    output_dir.mkdir(parents=True, exist_ok=True)
    cases = []
    for protocol in ("x11", "wayland"):
        for mode in ("default", "software"):
            for rotation in (0, 90):
                artifacts = Path(tempfile.mkdtemp(prefix=f"{protocol}-{mode}-{rotation}-", dir=output_dir.resolve()))
                artifacts.chmod(0o777)
                fixture = make_fixture(artifacts / "motion-photo.jpg", rotation)
                command = ["python3", "/qualification/tools/package_motion_checks.py", "--run-installed",
                           "--diagnostic", diagnostic, "--fixture", "/reports/motion-photo.jpg",
                           "--output", "/reports/report.json", "--runtime-root", runtime_root,
                           "--qt-version", qt_version, "--protocol", protocol, "--mode", mode,
                           "--rotation-degrees", str(rotation)]
                session = ["sh", "/qualification/tools/run_dialog_session.sh", protocol, *command]
                script = setup + "\n" + shlex.join(session)
                name = "licasa-motion-" + uuid.uuid4().hex[:16]
                docker = ["docker", "run", "--name", name, "--rm", "--network", "none", "--read-only",
                          "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--shm-size", "256m", "--cap-drop", "ALL",
                          "--user", "65534:65534", "-e", "HOME=/tmp/home", "-e", "XDG_RUNTIME_DIR=/tmp/runtime",
                          "-e", "XDG_CONFIG_HOME=/tmp/config", "-e", "XDG_CACHE_HOME=/tmp/cache",
                          "-e", "XDG_STATE_HOME=/tmp/state", "-e", "LICASA_DIALOG_LOG_DIRECTORY=/reports",
                          "-v", f"{ROOT / 'tools'}:/qualification/tools:ro", "-v", f"{artifacts}:/reports:rw",
                          image, "sh", "-ec", script]
                try:
                    result = subprocess.run(docker, text=True, capture_output=True, timeout=75)
                finally:
                    try:
                        cleanup = subprocess.run(["docker", "rm", "-f", name], text=True, capture_output=True, timeout=20)
                        (artifacts / "cleanup.log").write_text(cleanup.stdout + cleanup.stderr)
                    except (OSError, subprocess.SubprocessError) as error:
                        (artifacts / "cleanup.log").write_text(str(error) + "\n")
                (artifacts / "harness.log").write_text(result.stdout + result.stderr)
                if result.returncode:
                    raise RuntimeError(f"Packaged Motion lifecycle failed ({image}, {protocol}, {mode}, {rotation}deg); see {artifacts}")
                report = json.loads(result.stdout)
                primary = artifacts / "report.json"
                if not primary.is_file() or primary.is_symlink() or json.loads(primary.read_text()) != report:
                    raise RuntimeError("Motion stdout and retained evidence do not agree")
                if (report.get("status") != "passed" or report.get("qt_version") != qt_version
                        or report.get("protocol") != protocol or report.get("mode") != mode
                        or report.get("rotation_degrees") != rotation):
                    raise RuntimeError("Incomplete packaged Motion session identity")
                saved_frame = artifacts / "motion-frame.png"
                if (not saved_frame.is_file() or saved_frame.is_symlink()
                        or digest(saved_frame) != report.get("saved_frame", {}).get("sha256")):
                    raise RuntimeError("Motion saved-frame evidence was not retained intact")
                profile_evidence = artifacts / "report.runtime-profile.json"
                if (not profile_evidence.is_file() or profile_evidence.is_symlink()
                        or digest(profile_evidence) != report.get("runtime_probe", {}).get("report_sha256")):
                    raise RuntimeError("Motion runtime profile evidence was not retained intact")
                report["fixture"] = fixture
                report["artifacts_directory"] = str(artifacts)
                report["saved_frame"]["path"] = str(saved_frame)
                for key in ("report_path", "log_path"):
                    report["runtime_probe"][key] = str(artifacts / Path(report["runtime_probe"][key]).name)
                cases.append(report)
    return {"status": "passed", "qt_version": qt_version, "runtime_root": runtime_root, "cases": cases,
            "coverage_limits": ["Software FFmpeg decoding on virtual X11 and Wayland displays; physical GPU decoding is separate.",
                                "Deterministic modern Android Motion Photos exercise bounded custom QIODevice input; vendor-specific real-photo corpora remain separate."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-installed", action="store_true", required=True)
    parser.add_argument("--diagnostic", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-version", choices=tuple(FFMPEG), required=True)
    parser.add_argument("--protocol", choices=("x11", "wayland"), required=True)
    parser.add_argument("--mode", choices=("default", "software"), required=True)
    parser.add_argument("--rotation-degrees", type=int, choices=(0, 90), required=True)
    print(json.dumps(installed_check(parser.parse_args())))


if __name__ == "__main__":
    main()
