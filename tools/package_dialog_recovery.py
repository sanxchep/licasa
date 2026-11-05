#!/usr/bin/env python3
"""Prove a deliberately incomplete final runtime preserves the user's work."""

from __future__ import annotations

import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import uuid

from verify_dialog_interactions import RECOVERY_SCENARIOS

ROOT = Path(__file__).resolve().parents[1]
RECOVERY_ROOT = "/tmp/licasa-dialog-recovery"
MISSING_MODULE = "Qt.labs.folderlistmodel"


def run_dialog_recovery(image: str, *, app: str, diagnostic: str, sample: Path,
                        setup: str, qt_version: str, qml_root: str,
                        library_root: str, output_dir: Path) -> dict:
    artifacts = Path(tempfile.mkdtemp(prefix="missing-module-", dir=output_dir.resolve()))
    artifacts.chmod(0o777)
    native = qml_root == "/usr/lib/x86_64-linux-gnu/qt6/qml"
    isolated_qml = RECOVERY_ROOT + "/usr/qml"
    broken_app = RECOVERY_ROOT + ("/usr/bin/licasa" if native else "/AppRun")
    broken_diagnostic = RECOVERY_ROOT + "/usr/bin/licasa_diagnostics"
    preparation = f'''import hashlib, json, shutil, sys
from pathlib import Path
sys.path.insert(0, '/qualification/tools')
from qt_runtime import validate_runtime
from verify_dialog_interactions import application_executable
root = Path({RECOVERY_ROOT!r})
qml = Path({isolated_qml!r})
if {native!r}:
    (root / 'usr/bin').mkdir(parents=True)
    (root / 'usr/lib').mkdir()
    (root / 'usr/lib/licasa').symlink_to('/usr/lib/licasa', target_is_directory=True)
    # Native packages keep their pre-exec transaction wrapper at the public
    # path. This diagnostic relocation needs the actual ELF and its local Qt
    # config, so it cannot follow the wrapper back into the working runtime.
    source_app = application_executable(Path({app!r}))
    shutil.copy2(source_app, {broken_app!r})
    shutil.copy2({diagnostic!r}, {broken_diagnostic!r})
    # Keep installed startup assets when relocating the native executables.
    shutil.copytree(Path({app!r}).parents[1] / 'share/licasa',
                    root / 'usr/share/licasa', symlinks=True)
    shutil.copytree({qml_root!r}, qml, symlinks=True)
    (root / 'usr/bin/qt.conf').write_text(
        '[Paths]\\nPrefix=..\\nLibraries=/usr/lib/x86_64-linux-gnu\\n'
        'Plugins=/usr/lib/x86_64-linux-gnu/qt6/plugins\\n'
        'QmlImports=qml\\nQml2Imports=qml\\n')
else:
    source_app = Path({app!r})
    shutil.copytree(str(Path({qml_root!r}).parents[1]), root, symlinks=True)
digest = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
inputs = {{'missing_module': {MISSING_MODULE!r}, 'isolated_qml_root': str(qml),
          'original_app_sha256': digest(source_app),
          'original_diagnostic_sha256': digest({diagnostic!r})}}
if not (inputs['original_app_sha256'] == digest({broken_app!r})):
    raise AssertionError
if not (inputs['original_diagnostic_sha256'] == digest({broken_diagnostic!r})):
    raise AssertionError
shutil.rmtree(qml / 'Qt/labs/folderlistmodel')
libraries = Path({library_root!r}) if {native!r} else root / 'usr/lib'
try:
    validate_runtime(qml, libraries, {qt_version!r})
except RuntimeError as error:
    if 'Missing QML runtime file' not in str(error) or 'folderlistmodel/qmldir' not in str(error):
        raise
    inputs['assembly_rejected'] = True
    inputs['assembly_error'] = str(error)
else:
    raise RuntimeError('Intentionally missing FolderListModel passed the assembly guard')
Path('/reports/recovery-inputs.json').write_text(json.dumps(inputs))
'''
    arguments = ["sh", "/qualification/tools/run_dialog_session.sh", "x11",
                 "python3", "/qualification/tools/verify_dialog_interactions.py",
                 "--app", broken_app, "--diagnostic", broken_diagnostic,
                 "--sample", "/samples/photo.jpg", "--output", "/reports/report.json",
                 "--protocol", "x11", "--mode", "software", "--dialog-backend", "fallback",
                 "--expect-unavailable", "--timeout", "90"]
    script = (setup + "\npython3 - <<'LICASA_RECOVERY'\n" + preparation + "\nLICASA_RECOVERY\n"
              + "if " + shlex.join(arguments) + " >/reports/harness.log 2>&1; then "
              "cat /reports/report.json; else cat /reports/harness.log >&2; "
              "test ! -f /reports/report.json || cat /reports/report.json >&2; exit 1; fi")
    name = "licasa-recovery-" + uuid.uuid4().hex[:16]
    command = ["docker", "run", "--name", name, "--rm", "--network", "none", "--read-only",
               "--tmpfs", "/tmp:rw,exec,nosuid,nodev,size=2g", "--shm-size", "256m",
               "--user", "65534:65534", "--cap-drop", "ALL",
               "-e", "HOME=/tmp/home", "-e", "XDG_RUNTIME_DIR=/tmp/runtime",
               "-e", "XDG_CONFIG_HOME=/tmp/config", "-e", "XDG_CACHE_HOME=/tmp/cache",
               "-e", "XDG_STATE_HOME=/tmp/state",
               "-e", "LICASA_DIALOG_LOG_DIRECTORY=/reports",
               "-v", f"{ROOT / 'tools'}:/qualification/tools:ro",
               "-v", f"{ROOT / 'packaging/qt-runtime-modules.json'}:/qualification/packaging/qt-runtime-modules.json:ro",
               "-v", f"{sample.resolve(strict=True)}:/samples/photo.jpg:ro",
               "-v", f"{artifacts}:/reports:rw", image, "sh", "-ec", script]
    try:
        result = subprocess.run(command, text=True, capture_output=True, timeout=150)
    finally:
        try:
            cleanup = subprocess.run(["docker", "rm", "-f", name], text=True,
                                     capture_output=True, timeout=20)
            (artifacts / "container-cleanup.log").write_text(cleanup.stdout + cleanup.stderr)
        except (OSError, subprocess.SubprocessError) as error:
            (artifacts / "container-cleanup.log").write_text(str(error) + "\n")
    if result.returncode:
        raise RuntimeError(f"Package missing-module recovery failed; evidence {artifacts}:\n"
                           f"{result.stdout}\n{result.stderr}")
    report = json.loads(result.stdout)
    primary = artifacts / "report.json"
    if not primary.is_file() or primary.is_symlink() or json.loads(primary.read_text()) != report:
        raise RuntimeError("Recovery stdout does not match retained primary evidence")
    inputs = json.loads((artifacts / "recovery-inputs.json").read_text())
    report.update(inputs)
    report["artifacts_directory"] = str(artifacts)
    report["runtime_log"] = str(artifacts / "report.runtime.log")
    report["qt_library_roots"] = ([library_root, "/usr/lib/licasa/lib"] if native
                                  else [RECOVERY_ROOT + "/usr/lib"])
    validate_recovery(report, qt_version)
    def retained(value):
        if isinstance(value, str):
            return value.replace("/reports/", str(artifacts) + "/")
        if isinstance(value, list):
            return [retained(item) for item in value]
        if isinstance(value, dict):
            return {key: retained(item) for key, item in value.items()}
        return value
    return retained(report)


def validate_recovery(report: dict, qt_version: str) -> None:
    scenarios = report.get("scenarios", [])
    names = [case.get("name") for case in scenarios]
    runtime = report.get("runtime", {})
    if (report.get("status") != "passed" or report.get("protocol") != "x11"
            or report.get("mode") != "software" or report.get("dialog_backend") != "fallback"
            or report.get("missing_module") != MISSING_MODULE or report.get("assembly_rejected") is not True
            or "Missing QML runtime file" not in report.get("assembly_error", "")
            or "Qt/labs/folderlistmodel/qmldir" not in report.get("assembly_error", "")
            or report.get("isolated_qml_root") != RECOVERY_ROOT + "/usr/qml"
            or report.get("app_sha256") != report.get("original_app_sha256")
            or report.get("diagnostic_sha256") != report.get("original_diagnostic_sha256")
            or any(not re.fullmatch(r"[a-f0-9]{64}", report.get(key, ""))
                   for key in ("app_sha256", "diagnostic_sha256"))
            or len(names) != len(set(names)) or not (RECOVERY_SCENARIOS | {"application_launch"}) <= set(names)
            or any(case.get("status") != "passed" for case in scenarios)
            or runtime.get("qt_version") != qt_version):
        raise RuntimeError("Incomplete or non-passing production missing-module recovery evidence")
    paths = runtime.get("qml_import_paths", [])
    if (report["isolated_qml_root"] not in paths
            or any(not isinstance(path, str) or ".." in Path(path).parts
                   or not (path.startswith("qrc:/") or path.startswith("/tmp/licasa-")) for path in paths)):
        raise RuntimeError(f"Recovery could borrow a non-isolated QML module: {paths}")
    roots = report.get("qt_library_roots", [])
    mapped = runtime.get("process", {}).get("mapped_qt_libraries", [])
    if (roots not in (["/usr/lib/x86_64-linux-gnu", "/usr/lib/licasa/lib"],
                      [RECOVERY_ROOT + "/usr/lib"]) or not mapped
            or any(not isinstance(path, str) or not Path(path).is_absolute() or ".." in Path(path).parts
                   or not any(Path(path).is_relative_to(root) for root in roots) for path in mapped)
            or any(not any(f"libQt6{name}.so." in path for path in mapped) for name in ("Core", "Qml", "Quick"))):
        raise RuntimeError("Recovery escaped the intended Qt runtime")
