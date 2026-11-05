#!/usr/bin/env python3
"""Run candidate qualifiers and bind retained evidence to source and package hashes."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from check_support_manifest import MANIFEST, ROOT, load_manifest
from package_dialog_recovery import RECOVERY_ROOT, validate_recovery
from package_motion_checks import REQUIRED_CONTROLS, make_fixture, validate_controls
from package_reader_checks import validate_readers

OUTCOMES = {"pass", "fail", "skipped", "unsupported"}
REQUIRED_DIALOG_SCENARIOS = {
    "application_launch", "keyboard_open_visible_fallback", "cancel_returns_focus", "picker_blocks_background_shortcuts",
    "ctrl_o_reopens_visible_fallback", "accepted_photo_displayed", "save_as_visible_fallback",
    "save_cancel_creates_no_output", "save_as_output_decodes", "lazy_FrameSaveDialog_visible",
    "lazy_MotionExportDialog_visible", "lazy_AnimationExportDialog_visible",
}
REQUIRED_ACCESSIBILITY_SCENARIOS = {
    "default_image_named_and_keyboard_open", "picker_dialog_role", "cancel_restores_viewer_focus",
    "keyboard_open_cancel_reopen", "toggle_role_state_action", "adjustment_role_range_step_and_action",
    "adjustment_keyboard_focus_and_value", "save_as_dialog_name_and_role", "save_as_keyboard_cancel",
    "crop_keyboard_instructions_and_apply", "notification_alert_name_and_detail", "orca_processes_production_events",
}
REQUIRED_NATIVE_DIALOG_SCENARIOS = {
    "gtk_native_picker_confirmed", "native_keyboard_open", "native_escape_restores_focus",
    "native_keyboard_reopen_cancel", "native_picker_blocks_background_shortcuts",
    "native_selection_displays_photo", "native_save_cancel_preserves_edits", "native_save_output_decodes",
}
REQUIRED_READER_FORMATS = {"jpeg", "png", "avif", "jxl", "heic", "dng", "apng", "svg", "qoi", "tiff", "webp", "jp2"}


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def checked_path(root: Path, relative: str) -> Path:
    path = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts or path.is_symlink():
        raise ValueError(f"Unsafe evidence path: {relative}")
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Evidence escapes its bundle: {relative}")
    if not path.is_file():
        raise ValueError(f"Missing evidence file: {relative}")
    return path


def package_hashes(manifest: dict, packages: Path, version: str) -> dict:
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Expected an exact release version")
    return {name: {"path": template.format(version=version),
                   "sha256": digest(checked_path(packages, template.format(version=version)))}
            for name, template in manifest["artifacts"].items()}


def check_identity(source_sha: str, build_run_id: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha) or not re.fullmatch(r"[1-9][0-9]*", build_run_id):
        raise ValueError("Expected a full source SHA and positive build run ID")


def ensure_candidate_source(source_sha: str, verification_sha: str | None = None) -> dict | None:
    if verification_sha:
        from retry_ci_verification import validate_verifier
        changed = validate_verifier(source_sha, verification_sha)
        return {"source_sha": verification_sha, "changed_files": changed}
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if actual != source_sha:
        raise ValueError(f"Qualification tooling must be checked out at candidate {source_sha}, got {actual}")
    result = subprocess.run(["git", "diff", "--quiet", "HEAD", "--"], cwd=ROOT, check=False)
    if result.returncode:
        raise ValueError("Qualification candidate source has uncommitted tracked changes")


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def validate_interactions(payload: dict) -> None:
    """A launch-only, skipped, duplicated, or partial interaction matrix cannot pass."""
    interactions = payload.get("dialog_interactions", [])
    expected = {(protocol, mode) for protocol in ("x11", "wayland") for mode in ("default", "software")}
    actual = {(case.get("protocol"), case.get("mode")) for case in interactions}
    if len(interactions) != 4 or actual != expected:
        raise ValueError("Missing or duplicated required dialog interaction matrix")
    for case in interactions:
        if case.get("status") != "passed" or case.get("dialog_backend") != "fallback":
            raise ValueError("Required fallback dialog interaction was failed, skipped, or unsupported")
        scenarios = case.get("scenarios", [])
        names = [row.get("name") for row in scenarios]
        if (len(set(names)) != len(names) or not REQUIRED_DIALOG_SCENARIOS.issubset(names)
                or any(row.get("status") != "passed" for row in scenarios
                       if row.get("name") in REQUIRED_DIALOG_SCENARIOS or row.get("required", True))):
            raise ValueError("Missing or non-passing required dialog scenarios")


def validate_target_runtime(payload: dict, target: dict, manifest: dict) -> None:
    """The dialog must execute the declared Qt profile inside its package boundary."""
    expected_version = manifest["profiles"][target["profile"]]["qt"]
    if payload.get("qt_version", expected_version) != expected_version:
        raise ValueError("Primary qualifier report used a different Qt runtime profile")
    artifact = target["artifact"]
    if artifact in {"ubuntu-deb", "debian-deb"}:
        roots = (Path("/usr/lib/x86_64-linux-gnu"), Path("/usr/lib/licasa/lib"))
    elif artifact in {"rpm", "arch"}:
        roots = (Path("/opt/licasa/usr/lib"),)
    elif artifact == "appimage":
        roots = (Path("/tmp/squashfs-root/usr/lib"),)
    elif artifact == "snap":
        roots = (Path("/snap/licasa"),)
    else:
        raise ValueError("No Qt runtime origin policy for this artifact")
    for case in payload["dialog_interactions"]:
        runtime = case.get("runtime", {})
        if runtime.get("qt_version") != expected_version:
            raise ValueError("Required dialog loaded a different Qt runtime profile")
        mapped = runtime.get("process", {}).get("mapped_qt_libraries", [])
        if (not mapped or any(not isinstance(path, str) or not Path(path).is_absolute()
                              or ".." in Path(path).parts
                              or not any(Path(path).is_relative_to(root) for root in roots)
                              for path in mapped)
                or any(not any(f"libQt6{name}.so." in path for path in mapped)
                       for name in ("Core", "Qml", "Quick"))):
            raise ValueError("Required dialog escaped its package Qt runtime or lacks Core/Qml/Quick mappings")


def validate_accessibility(payload: dict, target: dict, manifest: dict) -> None:
    if (payload.get("kind") != "accessibility" or payload.get("target_id") != target["id"]
            or payload.get("status") != "passed" or payload.get("desktop") != "MATE"
            or payload.get("protocol", payload.get("session_type")) != "x11"):
        raise ValueError("Required real MATE/AT-SPI accessibility report is missing or does not pass")
    scenarios = payload.get("scenarios", [])
    names = [row.get("name") for row in scenarios]
    if (len(names) != len(set(names)) or not REQUIRED_ACCESSIBILITY_SCENARIOS.issubset(names)
            or any(row.get("status") != "passed" for row in scenarios
                   if row.get("name") in REQUIRED_ACCESSIBILITY_SCENARIOS or row.get("required", True))):
        raise ValueError("Required MATE keyboard, role, state, value, focus, and dialog scenarios did not pass")
    expected_version = manifest["profiles"][target["profile"]]["qt"]
    if payload.get("qt_version") != expected_version:
        raise ValueError("MATE accessibility used a different Qt runtime profile")
    validate_target_runtime({**payload, "dialog_interactions": [{"runtime": payload.get("runtime", {})}]},
                            target, manifest)


def validate_unsupported_report(payload: dict, target: dict) -> None:
    """Expected installation rejection is a distinct, explicit contract."""
    qualifier = target.get("qualifier", [])
    if (target.get("artifact") != "ubuntu-deb" or not qualifier
            or qualifier[0] != "tools/verify_unsupported_ubuntu.py"):
        raise ValueError("Unknown unsupported installation contract")
    try:
        version = qualifier[qualifier.index("--ubuntu-version") + 1]
    except (ValueError, IndexError) as error:
        raise ValueError("Unsupported installation contract lacks its userspace") from error
    legacy_appimage = "--appimage" in qualifier
    if version not in {"22.04", "26.04"} or (legacy_appimage and version != "22.04"):
        raise ValueError("Unsupported installation contract has an invalid userspace or artifact selection")
    if (payload.get("ubuntu_version") != version
            or payload.get("native_deb") != "unsupported_dependency_rejection_confirmed"
            or not re.fullmatch(r"[a-f0-9]{64}", payload.get("package_sha256", "") or "")):
        raise ValueError("Missing explicit native dependency rejection for the requested candidate and userspace")
    if legacy_appimage and (
            payload.get("appimage") != "unsupported_glibc_rejection_confirmed"
            or not re.fullmatch(r"[a-f0-9]{64}", payload.get("appimage_sha256", "") or "")):
        raise ValueError("Ubuntu 22.04 rejection must prove the AppImage glibc baseline too")


def validate_rejected_artifacts(payload: dict, target: dict, packages: dict) -> None:
    if payload.get("package_sha256") != packages[target["artifact"]]["sha256"]:
        raise ValueError("Rejection receipt describes a different native candidate package")
    if ("--appimage" in target["qualifier"] and
            payload.get("appimage_sha256") != packages.get("appimage", {}).get("sha256")):
        raise ValueError("Rejection receipt describes a different candidate AppImage")


def validate_report(payload: dict, target: dict, manifest: dict) -> None:
    if payload.get("status") in {"failed", "skipped", "unsupported"}:
        raise ValueError("Primary package qualifier report does not pass")
    if target["status"] == "unsupported":
        validate_unsupported_report(payload, target)
        return
    if target.get("contract", "dialogs") == "accessibility":
        validate_accessibility(payload, target, manifest)
    elif target.get("contract", "dialogs") == "native-dialogs":
        validate_native_dialogs(payload, target, manifest)
    else:
        validate_interactions(payload)
        validate_target_runtime(payload, target, manifest)
        if target["artifact"] != "snap":
            positive = next(case for case in payload["dialog_interactions"]
                            if case["protocol"] == "x11" and case["mode"] == "software")
            recovery = positive.get("missing_module_recovery", {})
            try:
                validate_recovery(recovery, manifest["profiles"][target["profile"]]["qt"])
            except RuntimeError as error:
                raise ValueError(str(error)) from error
            roots = (["/usr/lib/x86_64-linux-gnu", "/usr/lib/licasa/lib"]
                     if target["artifact"] in {"ubuntu-deb", "debian-deb"} else [RECOVERY_ROOT + "/usr/lib"])
            if (recovery.get("qt_library_roots") != roots
                    or recovery.get("original_app_sha256") != positive.get("app_sha256")
                    or recovery.get("original_diagnostic_sha256") != positive.get("diagnostic_sha256")):
                raise ValueError("Missing-module recovery must exercise these exact installed executables and runtime")
        validate_motion_photo(payload, target, manifest)
        diagnostic_hashes = {case.get("diagnostic_sha256") for case in payload["dialog_interactions"]}
        if (len(diagnostic_hashes) != 1 or not re.fullmatch(r"[a-f0-9]{64}", next(iter(diagnostic_hashes), "") or "")
                or any(case.get("diagnostic_sha256") not in diagnostic_hashes
                       for case in payload["motion_photo"]["cases"])):
            raise ValueError("Dialog and Motion checks must exercise the same installed production diagnostic")
    if target["id"] in {"fedora44-appimage", "fedora44-rpm", "arch-package"}:
        validate_reader_smoke(payload, target, manifest)


def validate_motion_photo(payload: dict, target: dict, manifest: dict) -> None:
    motion = payload.get("motion_photo", {})
    expected_qt = manifest["profiles"][target["profile"]]["qt"]
    roots = {"ubuntu-deb": "/usr", "debian-deb": "/usr", "appimage": "/tmp/squashfs-root/usr",
             "rpm": "/opt/licasa/usr", "arch": "/opt/licasa/usr"}
    runtime_root = motion.get("runtime_root", "")
    if target["artifact"] == "snap":
        root = Path(runtime_root)
        valid_root = (root.name == "usr" and root.parent.parent == Path("/snap/licasa")
                      and re.fullmatch(r"x?[1-9][0-9]*", root.parent.name))
    else:
        valid_root = roots.get(target["artifact"]) == runtime_root
    if motion.get("status") != "passed" or motion.get("qt_version") != expected_qt or not valid_root:
        raise ValueError("Final package requires matching-profile Motion Photo runtime evidence")
    cases = motion.get("cases", [])
    actual = {(row.get("protocol"), row.get("mode"), row.get("rotation_degrees")) for row in cases}
    expected = {(protocol, mode, rotation) for protocol in ("x11", "wayland")
                for mode in ("default", "software") for rotation in (0, 90)}
    if len(cases) != 8 or actual != expected:
        raise ValueError("Motion Photo qualification requires all eight display, renderer, and rotation cases")
    with tempfile.TemporaryDirectory(prefix="licasa-motion-contract-") as temporary:
        fixtures = {rotation: make_fixture(Path(temporary) / f"{rotation}.jpg", rotation) for rotation in (0, 90)}
    for row in cases:
        rotation = row["rotation_degrees"]
        probe = row.get("runtime_probe", {})
        if (probe.get("qt_version") != expected_qt
                or not re.fullmatch(r"[a-f0-9]{64}", probe.get("report_sha256", ""))):
            raise ValueError("Packaged Motion lacks its retained actual Qt runtime version probe")
        validate_target_runtime({"dialog_interactions": [{"runtime": {
            "qt_version": probe.get("qt_version"), "process": {
                "mapped_qt_libraries": probe.get("mapped_qt_libraries", [])}}}]}, target, manifest)
        names = [case.get("name") for case in row.get("scenarios", [])]
        if (row.get("status") != "passed" or row.get("qt_version") != expected_qt
                or len(names) != len(set(names)) or not REQUIRED_CONTROLS.issubset(names)
                or any(case.get("status") != "passed" for case in row.get("scenarios", []))
                or row.get("fixture") != fixtures[rotation]):
            raise ValueError("Packaged Motion playback, custom input, frame saving, or teardown did not pass")
        try:
            runtime = validate_controls(row.get("controls", {}), runtime_root, expected_qt, rotation,
                {"qt_version": probe["qt_version"], "before_decode": {"mapped_qt_libraries": probe["mapped_qt_libraries"]}})
        except RuntimeError as error:
            raise ValueError(str(error)) from error
        if any(row.get("runtime", {}).get(key) != value for key, value in runtime.items()):
            raise ValueError("Motion runtime inventory does not match the executed production controls")
        if not re.fullmatch(r"[a-f0-9]{64}", row.get("runtime", {}).get("private_ffmpeg_sha256", "")):
            raise ValueError("Motion private FFmpeg runtime has no retained hash")
        if runtime_root != "/usr":
            reference = str(Path(runtime_root) / "lib/licasa/plugins/multimedia/libffmpegmediaplugin.so")
            if (row["runtime"].get("private_ffmpeg_reference") != reference
                    or row["runtime"].get("private_ffmpeg_reference_sha256") != row["runtime"]["private_ffmpeg_sha256"]):
                raise ValueError("Portable Motion loaded a different FFmpeg backend from its qualified private runtime")
        saved = row.get("saved_frame", {})
        dimensions = (240, 320) if rotation == 90 else (320, 240)
        if ((saved.get("width"), saved.get("height")) != dimensions
                or not re.fullmatch(r"[a-f0-9]{64}", saved.get("sha256", ""))):
            raise ValueError("Motion captured-frame output is missing, unrotated, or unverified")


def validate_reader_smoke(payload: dict, target: dict, manifest: dict) -> None:
    smoke = payload.get("reader_smoke", {})
    expected_qt = manifest["profiles"][target["profile"]]["qt"]
    formats = smoke.get("expected_formats", [])
    samples = smoke.get("samples", [])
    names = [row.get("expected_format") for row in samples]
    if (smoke.get("status") != "passed" or smoke.get("qt_version") != expected_qt
            or smoke.get("runtime", {}).get("qt_version") != expected_qt
            or not REQUIRED_READER_FORMATS.issubset(smoke.get("runtime", {}).get("read_formats", []))
            or len(formats) != len(set(formats)) or set(formats) != REQUIRED_READER_FORMATS
            or len(names) != len(set(names)) or set(names) != REQUIRED_READER_FORMATS):
        raise ValueError("Final package requires the complete 12-format reader matrix")
    prefix = Path("/tmp/squashfs-root/usr/lib") if target["artifact"] == "appimage" else Path("/opt/licasa/usr/lib")
    try:
        checked = validate_readers(smoke["runtime"], str(prefix), expected_qt)
    except RuntimeError as error:
        raise ValueError(str(error)) from error
    if samples != checked:
        raise ValueError("Reader summary does not match the executed production decode and codec mappings")


def validate_native_dialogs(payload: dict, target: dict, manifest: dict) -> None:
    if (payload.get("kind") != "native-dialogs" or payload.get("target_id") != target["id"]
            or payload.get("status") != "passed" or payload.get("desktop") != "MATE"
            or payload.get("protocol") != "x11" or payload.get("dialog_backend") != "native-gtk"
            or payload.get("runtime_dependencies_checked_before_desktop_tools") is not True):
        raise ValueError("Required native GTK picker report is missing or does not pass")
    scenarios = payload.get("scenarios", [])
    names = [row.get("name") for row in scenarios]
    if (len(names) != len(set(names)) or not REQUIRED_NATIVE_DIALOG_SCENARIOS.issubset(names)
            or any(row.get("status") != "passed" for row in scenarios)):
        raise ValueError("Required native GTK selection, cancellation, focus, edit preservation, and saving did not pass")
    confirmations = payload.get("native_confirmations", [])
    pids = {row.get("process_id") for row in confirmations}
    if len(confirmations) < 5 or len(pids) != 1 or not all(isinstance(pid, int) and pid > 0 for pid in pids):
        raise ValueError("Each native Open/Save cycle requires GTK chooser evidence from the application process")
    for row in confirmations:
        if ("gtk" not in row.get("toolkit", "").lower() or row.get("role") not in {"dialog", "file chooser"}
                or row.get("platform_theme_plugin") != "/usr/lib/x86_64-linux-gnu/qt6/plugins/platformthemes/libqgtk3.so"
                or not row.get("gtk_libraries")
                or any(not path.startswith("/usr/lib/x86_64-linux-gnu/libgtk-3.so.") for path in row["gtk_libraries"])):
            raise ValueError("Fallback Qt dialogs cannot stand in for verified native GTK chooser widgets")
    validate_target_runtime({**payload, "dialog_interactions": [{"runtime": payload.get("runtime", {})}]}, target, manifest)


def validate_stable_notes(notes: str) -> None:
    pending = r"\bpreview release\b|\bprerelease\b|\bstable follows\b|\b(?:qualification|acceptance)\s+(?:is\s+)?pending\b"
    if re.search(pending, notes, re.IGNORECASE):
        raise ValueError("Stable release notes still describe a preview or pending package acceptance")


def run_target(args: argparse.Namespace) -> None:
    manifest = load_manifest()
    check_identity(args.source_sha, args.build_run_id)
    verification = ensure_candidate_source(args.source_sha, getattr(args, "verification_sha", None))
    target = next((row for row in manifest["targets"] if row["id"] == args.target), None)
    if target is None:
        raise ValueError(f"Unknown target {args.target}")
    artifact = manifest["artifacts"][target["artifact"]].format(version=args.version)
    package = checked_path(args.packages.resolve(), artifact)
    output = args.evidence.resolve() / target["id"]
    if output.exists():
        raise ValueError("Use a fresh evidence directory; previous reports cannot qualify a new run")
    raw = output / "raw"
    raw.mkdir(parents=True)
    report = raw / "report.json"
    substitutions = {name: str(args.packages.resolve() / template.format(version=args.version))
                     for name, template in manifest["artifacts"].items()}
    substitutions.update(artifact=str(package), report=str(report))
    if target.get("desktop"):
        report = raw / "desktop-report.json"
        command = [sys.executable, str(ROOT / "tools/verify_snap_desktop.py"),
                   "--desktop", target["desktop"], "--version", args.version,
                   "--output", str(raw)]
    else:
        command = [sys.executable, *[part.format(**substitutions) for part in target["qualifier"]]]
        command[1] = str(ROOT / command[1])
    record = {"target": target["id"], "status": "fail", "source_sha": args.source_sha,
              "build_run_id": args.build_run_id, "version": args.version,
              "manifest_sha256": digest(MANIFEST), "artifact": target["artifact"],
              "profile": target["profile"],
              "package_sha256": digest(package), "started_at": utc_now(), "command": command}
    if verification:
        record["verification"] = verification
    failure = None
    try:
        with (raw / "qualifier.log").open("w") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout, check=False)
        record["exit_code"] = result.returncode
        if result.returncode:
            raise ValueError(f"Qualifier exited {result.returncode}; see {raw / 'qualifier.log'}")
        payload = json.loads(report.read_text())
        if not isinstance(payload, dict) or not payload or payload.get("status") in {"failed", "skipped", "unsupported"}:
            raise ValueError("Qualifier did not produce successful structured evidence")
        reported_hash = payload.get("package_sha256", payload.get("appimage_sha256", payload.get("rpm_sha256", payload.get("artifact_sha256"))))
        if reported_hash is not None and reported_hash != record["package_sha256"]:
            raise ValueError("Qualifier report describes a different package")
        reported_source = payload.get("package_source_commit", payload.get("source_commit"))
        if reported_source is not None and reported_source != args.source_sha:
            raise ValueError("Qualifier report describes different source")
        validate_report(payload, target, manifest)
        if target["status"] == "unsupported":
            rejected = {target["artifact"]: {"sha256": record["package_sha256"]}}
            if "--appimage" in target["qualifier"]:
                rejected["appimage"] = {"sha256": digest(checked_path(args.packages, manifest["artifacts"]["appimage"].format(version=args.version)))}
            validate_rejected_artifacts(payload, target, rejected)
        record["status"] = "unsupported" if target["status"] == "unsupported" else "pass"
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        failure = str(error)
        record["error"] = failure
    record["finished_at"] = utc_now()
    if shutil.which("docker"):
        images = subprocess.run(["docker", "image", "ls", "--no-trunc", "--digests"],
                                capture_output=True, text=True, timeout=30, check=False)
        (raw / "container-images.txt").write_text(images.stdout + images.stderr)
    if Path("/etc/os-release").is_file():
        shutil.copyfile("/etc/os-release", raw / "runner-os-release.txt")
    record["evidence"] = [{"path": str(path.relative_to(args.evidence.resolve())), "sha256": digest(path)}
                          for path in sorted(raw.rglob("*")) if path.is_file() and not path.is_symlink()]
    write_json(output / "outcome.json", record)
    if failure:
        raise ValueError(failure)
    print(f"{args.target}: {record['status']}")


def validate(attestation: dict, manifest: dict, *, source_sha: str, build_run_id: str,
             version: str, packages: Path, evidence: Path, manifest_path: Path = MANIFEST) -> None:
    check_identity(source_sha, build_run_id)
    if attestation.get("schema_version") != 1:
        raise ValueError("Unsupported qualification evidence version")
    expected = {"source_sha": source_sha, "build_run_id": build_run_id, "version": version,
                "manifest_sha256": digest(manifest_path)}
    for key, value in expected.items():
        if attestation.get(key) != value:
            raise ValueError(f"Qualification {key} does not match the selected candidate")
    if attestation.get("packages") != package_hashes(manifest, packages, version):
        raise ValueError("Qualification package hashes do not match the selected candidate")
    rows = attestation.get("outcomes", [])
    outcomes = {row.get("target"): row for row in rows}
    targets = {row["id"]: row for row in manifest["targets"]}
    if len(outcomes) != len(rows) or outcomes.keys() != targets.keys():
        raise ValueError("Qualification targets are missing, duplicated, or unexpected")
    for target_id, target in targets.items():
        row = outcomes[target_id]
        if "verification" in row:
            receipt = row["verification"]
            from retry_ci_verification import VERIFIER_FILES
            if (receipt != attestation.get("verification")
                    or not re.fullmatch(r"[a-f0-9]{40}", receipt.get("source_sha", ""))
                    or not isinstance(receipt.get("changed_files"), list)
                    or set(receipt["changed_files"]) - VERIFIER_FILES):
                raise ValueError(f"{target_id}: invalid separate verifier provenance")
        for key, value in expected.items():
            if row.get(key) != value:
                raise ValueError(f"{target_id}: stale or mismatched {key}")
        if (row.get("artifact") != target["artifact"] or row.get("profile") != target["profile"]
                or row.get("package_sha256") != attestation["packages"][target["artifact"]]["sha256"]):
            raise ValueError(f"{target_id}: package mismatch")
        if row.get("status") not in OUTCOMES:
            raise ValueError(f"{target_id}: unknown qualification outcome")
        required = "unsupported" if target["status"] == "unsupported" else "pass"
        # Development targets are still tested when shipped in a candidate. They
        # are never silently downgraded to skipped just to make promotion green.
        if row["status"] != required or row.get("exit_code") != 0:
            raise ValueError(f"{target_id}: expected {required}, got {row['status']}")
        files = row.get("evidence", [])
        if len(files) < 2 or not any(item["path"].endswith(".json") for item in files):
            raise ValueError(f"{target_id}: missing structured evidence and logs")
        seen = set()
        for item in files:
            if item["path"] in seen or not item["path"].startswith(f"{target_id}/raw/"):
                raise ValueError(f"{target_id}: invalid evidence inventory")
            seen.add(item["path"])
            if digest(checked_path(evidence, item["path"])) != item["sha256"]:
                raise ValueError(f"{target_id}: changed evidence file")
        report_name = "desktop-report.json" if target.get("desktop") else "report.json"
        report_path = f"{target_id}/raw/{report_name}"
        if report_path not in seen:
            raise ValueError(f"{target_id}: missing primary qualifier report")
        payload = json.loads(checked_path(evidence, report_path).read_text())
        if payload.get("status") in {"failed", "skipped", "unsupported"}:
            raise ValueError(f"{target_id}: primary qualifier report does not pass")
        validate_report(payload, target, manifest)
        if target["status"] == "unsupported":
            validate_rejected_artifacts(payload, target, attestation["packages"])


def assemble(args: argparse.Namespace) -> None:
    manifest = load_manifest()
    verification = ensure_candidate_source(args.source_sha, getattr(args, "verification_sha", None))
    attestation = {"schema_version": 1, "source_sha": args.source_sha, "build_run_id": args.build_run_id,
                   "version": args.version, "manifest_sha256": digest(MANIFEST), "created_at": utc_now(),
                   "packages": package_hashes(manifest, args.packages, args.version),
                   "outcomes": [json.loads(path.read_text()) for path in sorted(args.evidence.glob("*/outcome.json"))]}
    if verification:
        attestation["verification"] = verification
    # Always preserve failed or incomplete evidence before returning failure.
    write_json(args.evidence / "qualification.json", attestation)
    validate(attestation, manifest, source_sha=args.source_sha, build_run_id=args.build_run_id,
             version=args.version, packages=args.packages, evidence=args.evidence)
    print("All candidate qualification outcomes passed their support contract")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("run-target", "assemble", "check"))
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--verification-sha", help="Separate clean verifier commit; only approved harness files may differ")
    parser.add_argument("--build-run-id", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--target")
    parser.add_argument("--published", type=Path, help="Also check flattened downloaded release assets")
    parser.add_argument("--release-notes", type=Path, help="Reject stable notes describing preview or pending acceptance")
    parser.add_argument("--timeout", type=int, default=2400)
    args = parser.parse_args()
    if args.action == "run-target":
        if not args.target:
            parser.error("run-target requires --target")
        run_target(args)
    elif args.action == "assemble":
        assemble(args)
    else:
        validate(json.loads((args.evidence / "qualification.json").read_text()), load_manifest(),
                 source_sha=args.source_sha, build_run_id=args.build_run_id, version=args.version,
                 packages=args.packages, evidence=args.evidence)
        if args.published:
            hashes = package_hashes(load_manifest(), args.packages, args.version)
            for package in hashes.values():
                path = checked_path(args.published, Path(package["path"]).name)
                if digest(path) != package["sha256"]:
                    raise ValueError("Published release package differs from qualified candidate")
        if args.release_notes:
            validate_stable_notes(args.release_notes.read_text())
        print("Exact candidate source, packages, and qualification evidence verified")


if __name__ == "__main__":
    main()
