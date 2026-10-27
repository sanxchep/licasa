#!/usr/bin/env python3
"""Exercise native lifecycle scriptlets with actual managers in disposable guests.

Fixtures use a package-owned ELF interpreter as a deterministic protocol peer.
They qualify the manager/admission boundary, not real GUI/media qualification.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import native_lifecycle as native
from package_rpm import render_spec
from package_deb import write_maintainer_scripts


ROOT = Path(__file__).resolve().parents[1]
IMAGES = {"deb": "licasa-deb-builder:post081-gtk", "rpm": "licasa-rpm-builder:fedora44",
          "arch": "licasa-arch-package-verify:local"}
VIEWER = r'''import hashlib,json,os,socket,time
from pathlib import Path
uid=os.geteuid()
binary=os.readlink('/proc/self/exe').removesuffix(' (deleted)')
assert not any(name in os.environ for name in ('APPIMAGE','SNAP','SNAP_NAME','SNAP_INSTANCE_NAME','SNAP_REVISION','SNAP_VERSION','SNAP_USER_DATA','SNAP_USER_COMMON'))
assert os.environ.get('APPDIR')==('/opt/licasa' if binary.startswith('/opt/licasa/') else None)
family,anchor=('appimage','/opt/licasa') if binary.startswith('/opt/licasa/') else ('filesystem',binary)
identity=hashlib.sha256(f'{uid}\n{family}\n{anchor}'.encode()).hexdigest()[:32]
record=Path('/proc/self/stat').read_text()
started=Path('/proc/sys/kernel/random/boot_id').read_text().strip()+':'+record[record.rfind(')')+2:].split()[19]
generation=hashlib.sha256(Path('/proc/self/exe').read_bytes()).hexdigest()
runtime=Path(os.environ['XDG_RUNTIME_DIR'])
endpoint=runtime/f'licasa-lc-{identity}.sock'
endpoint.unlink(missing_ok=True)
server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
server.bind(str(endpoint));os.chmod(endpoint,0o600);server.listen()
fd=os.environ.get('LICASA_LAUNCH_GATE_FD')
if fd:
    os.close(int(fd))
prepared=False
try:
    while True:
        connection,_=server.accept()
        with connection:
            data=b''
            while b'\n' not in data:
                data+=connection.recv(4096)
            request=json.loads(data.split(b'\n')[0]);op=request['operation']
            flags=json.loads(Path(f'/tmp/licasa-flags-{uid}.json').read_text())
            busy=flags.get('busy',False)
            result='ready'
            if op in ('prepare-update','stop-for-update') and busy:
                result='busy'
            elif op=='prepare-update':
                prepared=True
            elif op=='cancel-update':
                prepared=False
            elif op=='stop-for-update':
                result='accepted' if prepared else 'busy'
            reply={'protocol':1,'installationId':identity,'pid':os.getpid(),'startIdentity':started,
                   'generation':generation,'visibleWindows':1 if busy else 0,
                   'busyReason':'unsaved-edits' if busy else '', 'result':result}
            connection.sendall(json.dumps(reply).encode()+b'\n')
            if op=='stop-for-update' and result=='accepted' and not flags.get('hangStop',False):
                break
finally:
    server.close();endpoint.unlink(missing_ok=True)
'''


def write(path: Path, text: str, mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(mode)


def install_scripts(stage: Path, installation: str) -> None:
    control = stage / "DEBIAN"
    control.mkdir(parents=True, exist_ok=True)
    assert installation == "native"
    write_maintainer_scripts(control)


def fixture_stage(destination: Path, installation: str, version: int, broken: bool = False) -> None:
    config = native.CONFIGURATIONS[installation]
    binary_name = "/usr/bin/licasa" if installation == "native" and version == 0 else config["binary"]
    binary = destination / binary_name.lstrip("/")
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(sys.executable).resolve(), binary)
    with binary.open("ab") as stream:
        stream.write(f"\nlicasa-lifecycle-fixture-{version}\n".encode())
    binary.chmod(0o755)
    if installation == "native":
        script = destination / "usr/share/licasa/fixture-viewer.py"
    else:
        script = destination / "opt/licasa/usr/share/licasa/fixture-viewer.py"
        write(destination / "opt/licasa/AppRun.lifecycle-original",
              '#!/bin/sh\nexec /opt/licasa/usr/bin/licasa /opt/licasa/usr/share/licasa/fixture-viewer.py "$@"\n', 0o755)
    write(script, VIEWER)
    write(destination / config["receipt"].lstrip("/").replace("native-runtime.json", "fixture-version"), str(version))
    if version == 0:
        if installation == "appimage-native":
            shutil.copy2(destination / "opt/licasa/AppRun.lifecycle-original", destination / "opt/licasa/AppRun")
            write(destination / "usr/bin/licasa", '#!/bin/sh\nexec /opt/licasa/AppRun "$@"\n', 0o755)
    else:
        native.stage_payload(destination, installation)
    if broken:
        binary.write_bytes(binary.read_bytes() + b"unreceipted-change")


def build_package(manager: str, version: int, output: Path, broken: bool = False) -> Path:
    installation = "native" if manager == "deb" else "appimage-native"
    stage = output / f"stage-{manager}-{version}"
    fixture_stage(stage, installation, version, broken)
    if manager == "deb":
        if version != 0:
            install_scripts(stage, installation)
        write(stage / "DEBIAN/control", f"Package: licasa\nVersion: 0.0.{version}\nArchitecture: amd64\nMaintainer: Licasa qualification\nDescription: Disposable lifecycle fixture\nPre-Depends: python3\nDepends: bash, util-linux\n")
        package = output / f"licasa-0.0.{version}.deb"
        subprocess.run(["dpkg-deb", "--build", "--root-owner-group", str(stage), str(package)], check=True, capture_output=True)
        return package
    if manager == "rpm":
        top = output / f"rpm-{version}"
        for name in ("SOURCES", "SPECS", "BUILD", "RPMS", "SRPMS"):
            (top / name).mkdir(parents=True)
        spec = f"""%global debug_package %{{nil}}
%global __brp_strip %{{nil}}
%global __brp_strip_comment_note %{{nil}}
%global __brp_strip_static_archive %{{nil}}
%global __brp_mangle_shebangs %{{nil}}
%global __brp_python_bytecompile %{{nil}}
%global __requires_exclude_from ^/.*$
Name: licasa-appimage
Version: 0.0.{version}
Release: 1
Summary: Disposable lifecycle fixture
License: MIT
BuildArch: x86_64
AutoReqProv: no
Requires: python3
%description
Disposable lifecycle package-manager fixture.
%install
cp -a {stage}/. %{{buildroot}}/
"""
        if version != 0:
            # Exercise the production phases/dependency qualifiers and embedded
            # scripts instead of maintaining an independent scriptlet fixture.
            production = render_spec()
            dependencies = re.findall(r"^Requires\([^)]+\):.*$", production, re.MULTILINE)
            spec = spec.replace("%description\n", "\n".join(dependencies) + "\n%description\n", 1)
            scripts = re.findall(r"(?ms)^%(pretrans|pre|post|preun|postun)\n(.*?)(?=^%[a-z]|\Z)", production)
            assert len(scripts) == 4, "Expected all four production RPM lifecycle scripts"
            for name, source in scripts:
                spec += f"\n%{name}\n" + source
        spec += "\n%files\n/usr/bin/licasa\n/opt/licasa\n"
        spec_path = top / "SPECS/fixture.spec"
        spec_path.write_text(spec)
        subprocess.run(["rpmbuild", "-bb", "--define", f"_topdir {top}", str(spec_path)], check=True, capture_output=True)
        return next((top / "RPMS").rglob("*.rpm"))
    hook = ROOT / "packaging/arch/licasa-lifecycle.hook"
    if version != 0:
        write(stage / "usr/share/libalpm/hooks/licasa-lifecycle.hook", hook.read_text())
        # Include the hook in the receipt, just as the production PKGBUILD does.
        native.stage_payload(stage, installation)
    if broken:
        with (stage / native.CONFIGURATIONS[installation]["binary"].lstrip("/")).open("ab") as stream:
            stream.write(b"unreceipted-change")
    install = (ROOT / "packaging/arch/licasa.install.in").read_text().replace("@NATIVE_HELPER_SOURCE@", Path(native.__file__).read_text())
    if version != 0:
        write(stage / ".INSTALL", install)
    write(stage / ".PKGINFO", f"pkgname = licasa-appimage\npkgbase = licasa-appimage\npkgver = 0.0.{version}-1\npkgdesc = Disposable lifecycle fixture\nurl = https://example.invalid\nbuilddate = 1793026800\npackager = Licasa qualification\nsize = 1\narch = x86_64\nlicense = MIT\n")
    package = output / f"licasa-appimage-0.0.{version}-1-x86_64.pkg.tar.zst"
    members = [".PKGINFO", "usr", "opt"] + ([".INSTALL"] if version != 0 else [])
    subprocess.run(["bsdtar", "--zstd", "-cf", str(package), "-C", str(stage), *members], check=True, capture_output=True)
    return package


def inside(manager: str, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    installation = "native" if manager == "deb" else "appimage-native"
    config = native.CONFIGURATIONS[installation]
    trace = []
    peers = []
    report = {"schema": 1, "manager": manager, "helperSha256": native.digest(Path(native.__file__)),
              "peer": "deterministic package-owned ELF protocol fixture",
              "guiMediaCoverage": "not-tested", "firstArchUpgrade": "explicit-full-Quit-prerequisite",
              "result": "failed"}

    def event(name: str, **fields):
        trace.append({"action": name, **fields})
        (output / "trace.json").write_text(json.dumps(trace, indent=2) + "\n")

    def command(arguments: list[str], expect: bool = True):
        completed = subprocess.run(arguments, capture_output=True, text=True, timeout=60)
        event("manager", command=arguments, returncode=completed.returncode,
              stdout=completed.stdout, stderr=completed.stderr)
        if expect and completed.returncode:
            raise AssertionError(f"Command failed: {arguments}: {completed.stderr}")
        return completed

    def install(package: Path, expect: bool = True, extra: tuple[str, ...] = ()):
        arguments = {"deb": ["dpkg", "-i"], "rpm": ["rpm", "-U", "--oldpackage", "--replacepkgs"],
                     "arch": ["pacman", "-U", "--noconfirm"]}[manager]
        return command([*arguments, *extra, str(package)], expect)

    def flags(uid: int, **values):
        write(Path(f"/tmp/licasa-flags-{uid}.json"), json.dumps(values))

    def launch(uid: int, **values):
        runtime = Path(f"/run/user/{uid}")
        runtime.mkdir(parents=True, exist_ok=True)
        runtime.chmod(0o700)
        os.chown(runtime, uid, uid)
        flags(uid, **values)
        arguments = [config["launcher"]]
        if installation == "native":
            arguments.append("/usr/share/licasa/fixture-viewer.py")
        def user():
            os.setgroups([])
            os.setgid(uid)
            os.setuid(uid)
        environment = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), APPIMAGE="/tmp/foreign.AppImage",
                           APPDIR="/tmp/foreign", SNAP="/snap/foreign/1", SNAP_NAME="foreign",
                           SNAP_INSTANCE_NAME="foreign_instance", SNAP_REVISION="1", SNAP_VERSION="0.0.0",
                           SNAP_USER_DATA="/tmp/foreign-snap-data", SNAP_USER_COMMON="/tmp/foreign-snap-common")
        process = subprocess.Popen(arguments, env=environment,
                                   preexec_fn=user, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        peers.append(process)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            target = next((candidate for candidate in native.processes(installation) if candidate["pid"] == process.pid), None)
            if target and list(runtime.glob("licasa-lc-*.sock")):
                event("launch", pid=process.pid, uid=uid, generation=target["generation"], **values)
                return process
            if process.poll() is not None:
                raise AssertionError(f"Fixture launch failed: {process.stderr.read().decode()}")
            time.sleep(0.02)
        raise AssertionError("Fixture did not publish a private endpoint.")

    def state() -> str:
        return json.loads((Path("/var/lib/licasa/lifecycle") / installation / "transaction.json").read_text())["state"]

    try:
        packages = {version: build_package(manager, version, output, version == 3) for version in (0, 1, 2, 3)}
        report["artifacts"] = {str(version): {"path": str(path), "sha256": native.digest(path)} for version, path in packages.items()}
        install(packages[0])
        runtime = Path("/run/user/1000")
        runtime.mkdir(parents=True, exist_ok=True)
        runtime.chmod(0o700)
        os.chown(runtime, 1000, 1000)
        old_binary = "/usr/bin/licasa" if manager == "deb" else config["binary"]
        def legacy_user():
            os.setgroups([])
            os.setgid(1000)
            os.setuid(1000)
        legacy = subprocess.Popen([old_binary, "-c", "import time; time.sleep(600)"],
                                  env=dict(os.environ, XDG_RUNTIME_DIR=str(runtime)), preexec_fn=legacy_user)
        peers.append(legacy)
        time.sleep(0.1)
        assert legacy.poll() is None
        if manager != "arch":
            old_hash = native.digest(Path(old_binary))
            rejected = install(packages[1], expect=False)
            assert rejected.returncode != 0 and native.digest(Path(old_binary)) == old_hash
            assert legacy.poll() is None
            event("legacy-without-protocol-blocks-before-mutation")
        # The accepted first-Arch-upgrade policy requires the user to fully
        # Quit before invoking pacman. A new incoming hook cannot enforce it.
        legacy.terminate()
        legacy.wait(timeout=3)
        event("first-legacy-full-Quit-prerequisite-satisfied")
        install(packages[1])
        assert state() == "verified"
        event("first-install-verified")
        first = launch(1000)
        busy = launch(1001, busy=True)
        original = native.digest(Path(config["binary"]))
        rejected = install(packages[2], expect=False)
        assert rejected.returncode != 0 and native.digest(Path(config["binary"])) == original
        assert first.poll() is None and busy.poll() is None and state() == "verified"
        event("busy-all-users-blocked-before-mutation")
        if manager == "arch":
            rejected = install(packages[2], expect=False, extra=("--noscriptlet",))
            assert rejected.returncode != 0 and native.digest(Path(config["binary"])) == original
            event("arch-noscriptlet-still-enforces-prehook")
        flags(1001)
        install(packages[2])
        first.wait(timeout=3)
        busy.wait(timeout=3)
        assert native.digest(Path(config["binary"])) != original and state() == "verified"
        event("all-predecessors-exited-before-upgrade")
        hung = launch(1000, hangStop=True)
        original = native.digest(Path(config["binary"]))
        rejected = install(packages[1], expect=False)
        assert rejected.returncode != 0 and native.digest(Path(config["binary"])) == original
        assert hung.poll() is None
        hung.terminate()
        hung.wait(timeout=3)
        event("accepted-stop-timeout-preserves-payload")
        install(packages[1])
        assert state() == "verified" and native.digest(Path(config["binary"])) != original
        event("downgrade-verified")
        broken = install(packages[3], expect=False)
        assert state() == "recovery-needed"
        blocked = command([config["launcher"], "--lifecycle", "identity"], expect=False)
        assert blocked.returncode != 0
        event("failed-configuration-blocks-launch", managerReturncode=broken.returncode)
        install(packages[2])
        assert state() == "verified"
        event("package-manager-repair-verified")
        resident = launch(1000)
        removal = {"deb": ["dpkg", "--remove", "licasa"], "rpm": ["rpm", "-e", "licasa-appimage"],
                   "arch": ["pacman", "-R", "--noconfirm", "licasa-appimage"]}[manager]
        command(removal)
        resident.wait(timeout=3)
        assert not Path(config["binary"]).exists() and state() == "removed"
        event("running-uninstall-exits-owned-predecessor")
        if manager == "deb":
            command(["dpkg", "--purge", "licasa"])
            assert state() == "purged"
            journal = json.loads((Path("/var/lib/licasa/lifecycle") / installation / "transaction.json").read_text())
            assert set(journal) == {"schema", "installation", "state", "updatedAt"}
            event("purge-retires-root-history-keeps-user-data")
        install(packages[1])
        assert state() == "verified" and not native.processes(installation)
        event("remove-reinstall-verified-no-resurrection")
        report["result"] = "passed"
        report["cases"] = [item["action"] for item in trace if item["action"] != "manager"]
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        for peer in peers:
            if peer.poll() is None:
                peer.terminate()
                peer.wait(timeout=3)
        (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manager", choices=tuple(IMAGES), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inside", action="store_true")
    args = parser.parse_args()
    if args.inside:
        if os.geteuid() != 0 or os.environ.get("LICASA_NATIVE_TEST_GUEST") != "1" or not Path("/.dockerenv").is_file():
            parser.error("--inside is reserved for the disposable Docker guest started by this verifier")
        inside(args.manager, args.output)
        return
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(["docker", "run", "--rm", "--user", "0", "--network", "none", "--cap-add", "SYS_PTRACE",
                    "--env", "LICASA_NATIVE_TEST_GUEST=1",
                    "-v", f"{ROOT}:/source:ro", "-v", f"{output}:/output", IMAGES[args.manager],
                    "python3", "/source/tools/verify_native_lifecycle.py", "--inside",
                    "--manager", args.manager, "--output", "/output"], check=True)
    if args.manager == "rpm":
        # The lifecycle builder already has Python; it cannot establish that
        # DNF can bootstrap the interpreter in the package's own transaction.
        package = next((output / "rpm-1/RPMS").rglob("*.rpm"))
        script = (
            "test ! -e /usr/bin/python3; "
            "dnf install -y --setopt=install_weak_deps=False /input/fixture.rpm; "
            "/usr/bin/python3 -I /opt/licasa/usr/libexec/licasa/native_lifecycle.py "
            "--installation appimage-native status; "
            "test \"$(cat /var/lib/licasa/lifecycle/appimage-native/state)\" = verified; "
            "dnf remove -y --noautoremove licasa-appimage; "
            "test ! -e /opt/licasa && test ! -e /usr/bin/licasa"
        )
        result = subprocess.run(["docker", "run", "--rm", "--user", "0",
                                 "-v", f"{package}:/input/fixture.rpm:ro", "fedora:44",
                                 "sh", "-ec", script], capture_output=True, text=True)
        (output / "fresh-install.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f"Clean Fedora RPM dependency bootstrap failed:\n{result.stdout}\n{result.stderr}")
        # The inner verifier's report is root-owned. Keep host-written bootstrap
        # evidence separate so an ordinary CI runner can retain it too.
        report = {"result": "passed", "freshInstallWithPythonInSameTransaction": True,
                  "packageSha256": native.digest(package), "removalChecked": True}
        (output / "fresh-install.json").write_text(json.dumps(report, indent=2) + "\n")
        print("RPM lifecycle and fresh-install dependency ordering passed.")


if __name__ == "__main__":
    main()
