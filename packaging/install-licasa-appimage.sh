#!/usr/bin/env bash
# Install an AppImage as an extracted, FUSE-free per-user desktop application.
set -euo pipefail
# New per-user state/application parents must not inherit a shared writable
# umask. Existing directory permissions are inspected, never silently changed.
umask 077

# Shared with clean-package checks, before any desktop test tools are installed.
# Only the dynamic loader inspects these files; no viewer or display is started.
licasa_check_runtime_dependencies() (
  set -euo pipefail
  if [[ $# -lt 2 ]]; then
    echo "A library path and application files are required for the runtime check." >&2
    return 2
  fi
  local library_path=$1
  shift
  local check_work file magic output status checked=0 failed=0
  if ! check_work=$(mktemp -d); then
    echo "Could not create temporary files for the runtime library check." >&2
    return 1
  fi
  trap 'rm -rf -- "$check_work"' EXIT
  if ! find "$@" -type f -print0 > "$check_work/files"; then
    echo "Could not inspect all installed application files." >&2
    return 1
  fi
  while IFS= read -r -d '' file; do
    if [[ ! -r $file ]]; then
      printf 'Cannot read application file: %s\n' "$file" >&2
      failed=1
      continue
    fi
    magic=""
    LC_ALL=C read -r -N 4 magic < "$file" || true
    [[ $magic == $'\x7fELF' ]] || continue
    checked=$((checked + 1))
    status=0
    LC_ALL=C LD_PRELOAD= LD_AUDIT= LD_LIBRARY_PATH="$library_path" \
      ldd "$file" > "$check_work/ldd" 2>&1 || status=$?
    output=$(< "$check_work/ldd")
    if [[ $status -ne 0 || $output == *"=> not found"* || $output == *": version "*" not found"* ]]; then
      printf 'Missing or incompatible runtime libraries for %s:\n%s\n' "$file" "$output" >&2
      failed=1
    fi
  done < "$check_work/files"
  if [[ $checked -eq 0 ]]; then
    echo "No application binaries were found to check." >&2
    return 1
  fi
  if [[ $failed -ne 0 ]]; then
    echo "Licasa needs the libraries listed above before it can run." >&2
    echo "Use a glibc-based Linux desktop that provides the required library versions." >&2
    return 1
  fi
  printf 'Runtime library check passed (%s ELF files).\n' "$checked"
)

licasa_same_file() {
  [[ -f $1 && -f $2 ]] || return 1
  local first second
  first=$(sha256sum < "$1") || return 1
  second=$(sha256sum < "$2") || return 1
  [[ $first == "$second" ]]
}

# Read-only support reporting: no Qt, viewer, extraction runtime or network.
licasa_diagnose() (
  set +e
  local artifact=${1:-} tool value key available parent options output status findings=0
  printf '%s\n' 'Licasa pre-start support report v1' 'application_started=no' 'network_access=no'
  for tool in uname getconf ldd find cp chmod mkdir mktemp mv rm rmdir sed flock sha256sum dirname basename cat python3; do
    if command -v "$tool" >/dev/null 2>&1; then
      printf 'tool.%s=present\n' "$tool"
    else
      printf 'tool.%s=missing\n' "$tool"
      findings=1
    fi
  done
  if [[ -r /etc/os-release ]]; then
    while IFS='=' read -r key value; do
      case "$key" in ID|VERSION_ID)
        value=${value#\"}; value=${value%\"}
        printf 'os.%s=%s\n' "$key" "$value"
      esac
    done < /etc/os-release
  fi
  value=$(uname -m 2>/dev/null)
  printf 'architecture=%s\n' "${value:-unknown}"
  if [[ -n $value && $value != x86_64 ]]; then
    printf '%s\n' 'finding=This release requires x86-64 Intel/AMD Linux.'
    findings=1
  fi
  value=$(uname -s 2>/dev/null)
  printf 'kernel=%s\n' "${value:-unknown}"
  if [[ -n $value && $value != Linux ]]; then
    printf '%s\n' 'finding=This release requires Linux.'
    findings=1
  fi
  value=$(getconf GNU_LIBC_VERSION 2>/dev/null)
  printf 'libc=%s\n' "${value:-unknown}"
  if [[ $value != glibc\ * ]]; then
    printf '%s\n' 'finding=A glibc-based system and its inspection tools are required.'
    findings=1
  fi
  case "${XDG_CURRENT_DESKTOP:-}" in
    *GNOME*|*gnome*) value=GNOME ;;
    *KDE*|*Plasma*) value=KDE ;;
    *) value=other_or_unset ;;
  esac
  printf 'desktop=%s\n' "$value"
  case "${XDG_SESSION_TYPE:-}" in x11|wayland) value=$XDG_SESSION_TYPE ;; *) value=unknown ;; esac
  printf 'session=%s\n' "$value"
  for key in DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS QT_PLUGIN_PATH QML_IMPORT_PATH \
      QML2_IMPORT_PATH QT_QPA_PLATFORM QT_QPA_PLATFORMTHEME QT_QUICK_BACKEND LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT; do
    value=unset
    [[ -z ${!key:-} ]] || value=set
    printf 'environment.%s=%s\n' "$key" "$value"
  done
  # Keep subsequent loader/tool output from echoing private override values.
  # This function is a subshell, so the caller's environment is unchanged.
  unset LD_PRELOAD LD_AUDIT LD_LIBRARY_PATH
  if [[ -r /dev/fuse && -w /dev/fuse ]]; then
    printf '%s\n' 'fuse_device=accessible'
  else
    printf '%s\n' 'fuse_device=unavailable_or_inaccessible' \
      'hint=The per-user extracted installation does not need FUSE.'
  fi
  parent="${HOME:-}/.local/opt"
  while [[ ! -d $parent && $parent != / && $parent == */* ]]; do parent=${parent%/*}; done
  value=no
  [[ -d $parent && -w $parent ]] && value=yes
  printf 'installation_parent_writable=%s\n' "$value"
  if [[ $value == no ]]; then
    printf '%s\n' 'finding=The installation destination is not writable.'
    findings=1
  fi
  if command -v findmnt >/dev/null 2>&1; then
    options=$(findmnt -n -o OPTIONS -T "$parent" 2>/dev/null)
    case ",$options," in ,,)
      printf '%s\n' 'installation_filesystem=unknown' ;;
      *,noexec,*)
      printf '%s\n' 'installation_filesystem=noexec' 'finding=The installation destination blocks execution.'
      findings=1 ;;
      *) printf '%s\n' 'installation_filesystem=no_noexec_flag_detected' ;;
    esac
  else
    printf '%s\n' 'installation_filesystem=not_checked_findmnt_unavailable'
  fi
  if command -v df >/dev/null 2>&1; then
    while read -r key value output available options parent; do
      [[ $available =~ ^[0-9]+$ ]] && printf 'installation_available_kib=%s\n' "$available"
    done < <(df -Pk "${HOME:-/tmp}" 2>/dev/null)
  fi
  if [[ -n $artifact ]]; then
    if [[ ! -f $artifact || ! -r $artifact ]]; then
      printf '%s\n' 'artifact=missing_or_unreadable'
      findings=1
    else
      printf '%s\n' 'artifact=readable'
      value=no
      [[ ! -x $artifact ]] || value=yes
      printf 'artifact_executable_bit=%s\n' "$value"
      if command -v sha256sum >/dev/null 2>&1; then
        value=$(sha256sum < "$artifact")
        printf 'artifact_sha256=%s\n' "${value%% *}"
      fi
      printf '%s\n' 'artifact_executed=no' 'artifact_bundle_libraries=not_inspected_without_extraction'
    fi
  fi
  parent="${HOME:-}/.local/opt/licasa-appimage"
  if [[ ! -d $parent && -d /opt/licasa ]]; then parent=/opt/licasa; fi
  if [[ -d $parent/usr ]]; then
    printf '%s\n' 'installed_payload=present'
    output=$(licasa_check_runtime_dependencies "$parent/usr/lib/licasa/lib:$parent/usr/lib" "$parent/usr" 2>&1)
    status=$?
    if [[ -n ${HOME:-} ]]; then output=${output//"$HOME"/'<home>'}; fi
    printf '%s\n' "$output"
    if [[ $status -eq 0 ]]; then
      printf '%s\n' 'installed_libraries=passed'
    else
      printf '%s\n' 'installed_libraries=failed'
      findings=1
    fi
  else
    printf '%s\n' 'installed_payload=not_found'
  fi
  printf '%s\n' 'privacy=Environment values and photo paths are omitted. Review the report before sharing.'
  printf 'findings=%s\n' "$findings"
  return "$findings"
)

licasa_setup_logging() {
  local state=${XDG_STATE_HOME:-${HOME:-}/.local/state} log bytes
  [[ $state == /* && $state != *$'\n'* && $state != *$'\r'* ]] || return 1
  for log in mkdir chmod tee; do command -v "$log" >/dev/null 2>&1 || return 1; done
  state="$state/licasa"
  [[ ! -L $state ]] || return 1
  (umask 077; mkdir -p -- "$state") || return 1
  [[ -O $state ]] || return 1
  log="$state/installation.log"
  [[ ! -L $log && ( ! -e $log || ( -f $log && -O $log ) ) ]] || return 1
  if [[ -f $log ]] && command -v wc >/dev/null 2>&1; then
    bytes=$(wc -c < "$log")
    if [[ $bytes -gt 524288 ]]; then
      [[ ! -L $log.1 && ( ! -e $log.1 || ( -f $log.1 && -O $log.1 ) ) ]] || return 1
      mv -fT -- "$log" "$log.1" || return 1
    fi
  fi
  (umask 077; : >> "$log") && chmod 600 -- "$log" || return 1
  exec > >(tee -a -- "$log") 2> >(tee -a -- "$log" >&2)
  printf '\nLicasa %s output; private log: %s\n' "$operation" "$log"
}

# Package verification sources the checks without running the installer.
if [[ ${BASH_SOURCE[0]} != "$0" ]]; then
  return 0
fi

if [[ ${1:-} == --diagnose ]]; then
  if [[ $# -gt 2 ]]; then echo "Usage: $0 --diagnose [Licasa.AppImage]" >&2; exit 2; fi
  licasa_diagnose "${2:-}"
  exit $?
fi

operation=install
if [[ $# -eq 1 && $1 == --uninstall ]]; then
  operation=uninstall
elif [[ $# -ne 1 || ! -f $1 ]]; then
  echo "Usage: $0 Licasa-<version>-x86_64.AppImage | --uninstall | --diagnose [AppImage]" >&2
  echo "Download the AppImage and this installer from the same Licasa release." >&2
  echo "Run this installer as your normal desktop user; it does not need sudo or FUSE." >&2
  exit 2
fi

if ! licasa_setup_logging; then
  echo "Could not retain an installation log; this output is still available in the terminal." >&2
fi

commands=(cp chmod mkdir mktemp mv rm rmdir sed sha256sum flock dirname basename cat python3)
if [[ $operation == install ]]; then
  commands=(uname getconf ldd find "${commands[@]}")
fi
for required_command in "${commands[@]}"; do
  if ! command -v "$required_command" >/dev/null 2>&1; then
    echo "Cannot $operation Licasa: the required system tool '$required_command' is missing." >&2
    echo "Install your distribution's standard shell, file, util-linux, glibc tools and Python 3, then retry." >&2
    exit 1
  fi
done
if [[ $operation == install ]]; then
  if [[ $(uname -s) != Linux || $(uname -m) != x86_64 || $(getconf LONG_BIT) != 64 ]]; then
    echo "This AppImage requires 64-bit x86 Linux (Intel or AMD)." >&2
    echo "ARM and 32-bit systems cannot use this package." >&2
    exit 1
  fi
  if ! libc_version=$(getconf GNU_LIBC_VERSION 2>/dev/null) || [[ $libc_version != glibc\ * ]]; then
    echo "This AppImage requires a glibc-based Linux desktop." >&2
    echo "Systems using another C library, such as Alpine Linux with musl, need a compatible package." >&2
    exit 1
  fi
  appimage="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
fi

install_home="${HOME:?HOME is required}"
data_home="${XDG_DATA_HOME:-$install_home/.local/share}"
config_home="${XDG_CONFIG_HOME:-$install_home/.config}"
for location in "$install_home" "$data_home" "$config_home"; do
  if [[ $location != /* || $location == *$'\n'* || $location == *$'\r'* ]]; then
    echo "HOME and XDG data/config directories must be absolute paths without line breaks." >&2
    exit 2
  fi
done
apps="$install_home/.local/opt"
target="$apps/licasa-appimage"
marker='Installed by packaging/install-licasa-appimage.sh'
if [[ $operation == uninstall && ! -e $target && ! -L $target ]]; then
  echo "No per-user AppImage installation found in $target"
  exit 0
fi
mkdir -p "$apps"
# An intentionally symlinked home/application parent has one physical managed
# root. Use that same root in the viewer, its startup entry and controller.
apps=$(cd "$apps" && pwd -P)
target="$apps/licasa-appimage"
# Keep the lock file in place: removing it could let another process lock a new
# inode while this operation is still using the old one.
exec 9>"$apps/.licasa-appimage.lock"
if ! flock -n 9; then
  echo "Another Licasa installation or removal is in progress. Retry when it finishes." >&2
  exit 1
fi
if [[ -L $target || ( -e $target &&
      ( ! -f $target/.licasa-appimage-install || -L $target/.licasa-appimage-install ||
        $(< "$target/.licasa-appimage-install") != "$marker" ) ) ]]; then
  echo "Refusing to change an installation not created by this script: $target" >&2
  exit 1
fi

# Read data, never shell code. Remember integration locations across environment
# changes; older installations use the current XDG settings as a fallback.
if [[ -e $target/.licasa-appimage-paths || -L $target/.licasa-appimage-paths ]]; then
  paths=()
  if [[ -L $target/.licasa-appimage-paths || ! -f $target/.licasa-appimage-paths ]]; then
    echo "Invalid installation path record; no files were changed." >&2
    exit 1
  fi
  mapfile -d '' -t paths < "$target/.licasa-appimage-paths"
  if [[ ${#paths[@]} -ne 3 || ${paths[0]} != 1 || ${paths[1]} != /* || ${paths[2]} != /* ||
        ${paths[1]} == *$'\n'* || ${paths[2]} == *$'\n'* ||
        ${paths[1]} == *$'\r'* || ${paths[2]} == *$'\r'* ]]; then
    echo "Invalid installation path record; no files were changed." >&2
    exit 1
  fi
  data_home=${paths[1]}
  config_home=${paths[2]}
fi
desktop_dir="$data_home/applications"
icon_dir="$data_home/icons/hicolor/512x512/apps"
desktop="$desktop_dir/licasa-appimage.desktop"
icon="$icon_dir/licasa-appimage.png"
autostart="$config_home/autostart/licasa-background.desktop"

quote_exec() {
  local value=$1 tick=$'\x60'
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//\$/\\\$}"
  value="${value//$tick/\\$tick}"
  value="${value//%/%%}"
  # Desktop string decoding happens before command-line parsing. Preserve the
  # argument escapes through that first decoding step (e.g. a literal backslash
  # requires four backslashes in the desktop file).
  if [[ ${2:-} != legacy ]]; then value="${value//\\/\\\\}"; fi
  printf '"%s"' "$value"
}
write_desktop() {
  local mime prefix='bash '
  [[ ${2:-} != legacy ]] || prefix=''
  mime="$(sed -n 's/^MimeType=//p' "$1")"
  if [[ -z $mime || $mime == *$'\n'* ]]; then
    echo "The AppImage has no valid desktop MIME list" >&2
    return 1
  fi
  cat <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Licasa (AppImage)
Comment=Image viewer
Exec=$prefix$(quote_exec "$target/AppRun" "${2:-}") %f
Terminal=false
Categories=Graphics;Viewer;
MimeType=$mime
Icon=licasa-appimage
StartupNotify=false
EOF
}
refresh_desktop() {
  if command -v update-desktop-database >/dev/null 2>&1 && [[ -d $desktop_dir ]]; then
    if ! update-desktop-database "$desktop_dir"; then
      echo "Licasa was $1, but the desktop file cache could not be refreshed. Sign out and back in if needed." >&2
    fi
  fi
}

work="$(mktemp -d "$apps/.licasa-appimage.XXXXXXXX")"
backup="$work/previous-app"
lifecycle_helper=""
lifecycle_active=0
lifecycle_committed=0
legacy_uninstall=0
legacy_installation_id=""
committed=0
app_changed=0
integration_paths=()
integration_changed=()
integration_existed=()
desktop_new=""
icon_new=""
# The controller is shipped in new payloads. A source-tree copy also permits
# fixture regressions without constructing an installable Qt runtime.
find_lifecycle_helper() {
  local candidate
  for candidate in "${1:-}/usr/libexec/licasa/lifecycle_update.py" \
      "$target/usr/libexec/licasa/lifecycle_update.py" \
      "$(dirname "${BASH_SOURCE[0]}")/../tools/lifecycle_update.py"; do
    if [[ -f $candidate && ! -L $candidate ]]; then
      cp -- "$candidate" "$work/lifecycle_update.py"
      lifecycle_helper="$work/lifecycle_update.py"
      return 0
    fi
  done
  if [[ $operation == uninstall ]]; then
    legacy_uninstall=1
    return 0
  fi
  echo "This older installation needs the current AppImage for safe lifecycle coordination." >&2
  echo "Fully Quit Licasa, update using the current AppImage and installer, then retry removal." >&2
  return 1
}
# The downloaded remover must work against releases which predate the shared
# controller. This bridge checks its installation scope after full Quit; it never
# contacts, signals, or executes the old application and makes no seamless or
# durable-journal claim. Its existing rollback remains available.
legacy_full_quit_check() {
  python3 - "$target" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import socket
import stat
import sys

root = os.path.abspath(sys.argv[1])
expected = root + "/usr/bin/licasa"
backup_prefix = str(Path(root).parent / ".licasa-appimage.")
blocked = []
uninspectable = []
strict = os.environ.get("LICASA_STRICT_PROCESS_INSPECTION") == "1"
identity = hashlib.sha256(f"{os.geteuid()}\nappimage\n{root}".encode()).hexdigest()[:32]
receipts = {}
journal = Path(root).parent / ("." + Path(root).name + ".licasa-lifecycle") / "transaction.json"
if journal.exists() or journal.is_symlink():
    info = journal.lstat()
    parent = journal.parent.lstat()
    if (journal.is_symlink() or journal.parent.is_symlink() or not stat.S_ISREG(info.st_mode)
            or not stat.S_ISDIR(parent.st_mode) or info.st_uid != os.geteuid()
            or parent.st_uid != os.geteuid() or (info.st_mode | parent.st_mode) & 0o077
            or info.st_size > 1024 * 1024):
        raise SystemExit("Unsafe lifecycle receipt; no files changed.")
    record = json.loads(journal.read_text())
    if record.get("root") != root:
        raise SystemExit("Lifecycle receipt belongs to another installation; no files changed.")
    receipts = record.get("predecessors", {})
runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.geteuid()}"))
endpoint = runtime / f"licasa-lc-{identity}.sock"
if runtime.exists():
    info = runtime.lstat()
    if runtime.resolve() != runtime.absolute() or not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise SystemExit("Unsafe lifecycle runtime directory; no files changed.")
    if endpoint.exists() or endpoint.is_symlink():
        info = endpoint.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
            raise SystemExit("Unsafe lifecycle endpoint; no files changed.")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
            peer.settimeout(1)
            try:
                peer.connect(str(endpoint))
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            except OSError:
                blocked.append("unverifiable registered endpoint")
            else:
                # Without the installed controller, a registered peer always
                # needs full Quit. Never execute or signal the old runtime.
                blocked.append("registered endpoint")
for directory in Path("/proc").iterdir():
    if not directory.name.isdecimal():
        continue
    positive = False
    try:
        if directory.name in receipts:
            try:
                record = (directory / "stat").read_text()
                started = record[record.rfind(")") + 2:].split()[19]
                boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
                positive = f"{boot}:{started}" == receipts[directory.name]
            except (PermissionError, ValueError, IndexError):
                positive = True
        status = (directory / "status").read_text()
        credentials = next(line for line in status.splitlines() if line.startswith("Uid:"))
        if not positive and os.geteuid() not in {int(value) for value in credentials.split()[1:]}:
            continue
        record = (directory / "stat").read_text()
        if record[record.rfind(")") + 2:].split()[0] == "Z":
            continue
        executable = os.readlink(directory / "exe").removesuffix(" (deleted)")
        matches = positive or executable == expected
        positive = matches
        if not matches and executable.startswith(backup_prefix) and executable.endswith("/previous-app/usr/bin/licasa"):
            positive = True
            matches = os.fsencode("APPDIR=" + root) in (directory / "environ").read_bytes().split(b"\0")
        if matches:
            blocked.append(directory.name)
    except (FileNotFoundError, ProcessLookupError):
        continue
    except (PermissionError, StopIteration, ValueError, IndexError):
        try:
            positive = positive or os.fsencode(expected) in (directory / "cmdline").read_bytes().split(b"\0")
        except OSError:
            pass
        try:
            positive = positive or os.fsencode("APPDIR=" + root) in (directory / "environ").read_bytes().split(b"\0")
        except OSError:
            pass
        if positive:
            blocked.append(directory.name)
        else:
            uninspectable.append(directory.name)
print(json.dumps({"inspectionScope": "installation", "unknownProcessCount": len(uninspectable),
                  "globalAbsenceProven": False, "legacyFirstQuitRequired": True}, sort_keys=True), file=sys.stderr)
if uninspectable and strict:
    print("Process inspection is unavailable for PID " + ", ".join(uninspectable) +
          "; cannot prove that this installation has stopped. Resolve the inspection restriction "
          "before retrying removal. No application files were changed.", file=sys.stderr)
    raise SystemExit(1)
if blocked:
    print("An older Licasa process is still running or cannot be identified safely. "
          "Finish work and fully Quit Licasa, then retry removal. No application files were changed.", file=sys.stderr)
    raise SystemExit(1)
print(identity)
PY
}
lifecycle() {
  if [[ $legacy_uninstall -eq 1 ]]; then
    if [[ ${1:-} == installation-id ]]; then printf '%s\n' "$legacy_installation_id"; fi
    return 0
  fi
  python3 "$lifecycle_helper" "$@" --root "$target" --lock-fd 8
}
lock_lifecycle() {
  if [[ $legacy_uninstall -eq 1 ]]; then
    legacy_installation_id=$(legacy_full_quit_check)
    echo "Legacy removal: prior full Quit is required; scoped checks completed, global absence is not proven."
    return 0
  fi
  local gate="$apps/.licasa-appimage.licasa-lifecycle"
  # Refuse links/shared ownership before opening the lock; the Python
  # controller independently checks both the descriptor and linked inode.
  if [[ -L $gate || ( -e $gate && ! -d $gate ) ]]; then
    echo "Unsafe Licasa lifecycle gate: $gate" >&2
    return 1
  fi
  mkdir -m 700 -p "$gate"
  if [[ -L $gate/lock ]]; then
    echo "Unsafe Licasa lifecycle lock: $gate/lock" >&2
    return 1
  fi
  (umask 077; : >> "$gate/lock")
  exec 8<>"$gate/lock"
  if ! flock -n 8; then
    echo "Another Licasa launch or update is in progress; retry when it finishes." >&2
    return 1
  fi
  lifecycle recover
}
begin_lifecycle() {
  if [[ $legacy_uninstall -eq 1 ]]; then
    legacy_installation_id=$(legacy_full_quit_check)
    return 0
  fi
  local arguments=(prepare --recovery-root "$work") path
  for path in "${integration_paths[@]}"; do arguments+=(--integration "$path"); done
  lifecycle "${arguments[@]}"
  lifecycle_active=1
}
# Snapshot integration before any application or launcher mutation. The backup
# contains the original file/link itself, not the contents of a symlink target.
remember_integration() {
  local path=$1 index=${#integration_paths[@]}
  if [[ -d $path && ! -L $path ]]; then
    echo "Refusing to replace a directory at $path" >&2
    return 1
  fi
  integration_paths+=("$path")
  integration_changed+=(0)
  integration_existed+=(0)
  if [[ -e $path || -L $path ]]; then
    cp -a -- "$path" "$work/integration-$index"
    integration_existed[index]=1
  fi
}
cleanup() {
  local status=$? index path saved failed=0
  trap - EXIT HUP INT TERM
  set +e
  if [[ $committed -eq 0 ]]; then
    for ((index=${#integration_paths[@]}-1; index>=0; index--)); do
      [[ ${integration_changed[index]} -eq 1 ]] || continue
      path=${integration_paths[index]}
      saved="$work/integration-$index"
      if [[ ${integration_existed[index]} -eq 1 ]]; then
        # Skip a still-intact destination, e.g. a failed rename with a read-only
        # parent. Restore a symlink as a symlink, never overwrite its target.
        if [[ ! -L $path && ! -L $saved ]] && licasa_same_file "$saved" "$path"; then
          continue
        fi
        mv -fT -- "$saved" "$path" || failed=1
      else
        rm -f -- "$path" || failed=1
      fi
    done
    if [[ $app_changed -eq 1 ]]; then
      rm -rf -- "$target" || failed=1
    fi
    if [[ -e $backup ]]; then
      mv -- "$backup" "$target" || failed=1
    fi
  fi
  for path in "$desktop_new" "$icon_new"; do
    [[ -z $path ]] || rm -f -- "$path" || failed=1
  done
  if [[ $failed -eq 0 ]]; then
    if [[ $lifecycle_active -eq 1 && $lifecycle_committed -eq 0 ]]; then
      lifecycle mark --state verified || failed=1
    fi
  fi
  if [[ $failed -eq 0 ]]; then
    rm -rf -- "$work" || failed=1
  fi
  if [[ $failed -ne 0 ]]; then
    echo "Cleanup could not restore or remove every file. Recovery files are preserved in $work" >&2
    status=1
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ $operation == uninstall ]]; then
  find_lifecycle_helper
  lock_lifecycle
  expected_desktop="$target/.licasa-appimage.desktop"
  if [[ ! -f $expected_desktop ]]; then
    # Compatibility with 0.7.1: supply its original XDG_DATA_HOME if customized.
    expected_desktop="$work/legacy.desktop"
    write_desktop "$target/usr/share/applications/licasa.desktop" legacy > "$expected_desktop"
  fi
  if [[ -f $desktop && ! -L $desktop ]] && licasa_same_file "$expected_desktop" "$desktop"; then
    remember_integration "$desktop"
  elif [[ -e $desktop || -L $desktop ]]; then
    echo "Keeping a modified or unrelated desktop entry: $desktop" >&2
  fi
  if [[ -f $icon && ! -L $icon ]] &&
      licasa_same_file "$target/usr/share/icons/hicolor/512x512/apps/licasa.png" "$icon"; then
    remember_integration "$icon"
  elif [[ -e $icon || -L $icon ]]; then
    echo "Keeping a modified or unrelated icon: $icon" >&2
  fi
  installation_id=$(lifecycle installation-id)
  for startup_entry in "$autostart" "$config_home/autostart/licasa-background-$installation_id.desktop"; do
    if [[ -f $startup_entry && ! -L $startup_entry ]]; then
      owned=0
      exec_matches=0
      wrong_owner=0
      while IFS= read -r line; do
        [[ $line != X-Licasa-Background=1 ]] || owned=1
        if [[ $line == X-Licasa-Installation=* && $line != "X-Licasa-Installation=$installation_id" ]]; then
          wrong_owner=1
        fi
        for executable in "$target/AppRun" "$target/usr/bin/licasa"; do
          [[ $line != "Exec=/usr/bin/env $(quote_exec "$executable") --background" ]] || exec_matches=1
          [[ $line != "Exec=$(quote_exec "$executable") --background" ]] || exec_matches=1
          [[ $line != "Exec=$(quote_exec "$executable" legacy) --background" ]] || exec_matches=1
        done
      done < "$startup_entry"
      if [[ $owned -eq 1 && $exec_matches -eq 1 && $wrong_owner -eq 0 ]]; then
        remember_integration "$startup_entry"
      fi
    fi
  done
  begin_lifecycle
  lifecycle mark --state replacing
  for ((index=0; index<${#integration_paths[@]}; index++)); do
    integration_changed[index]=1
    rm -- "${integration_paths[index]}"
  done
  mv -- "$target" "$backup"
  lifecycle mark --state verified
  lifecycle_committed=1
  committed=1
  refresh_desktop removed
  echo "Removed the per-user Licasa AppImage installation. Settings and photos were kept."
  exit 0
fi

cp -- "$appimage" "$work/Licasa.AppImage"
chmod u+x "$work/Licasa.AppImage"
if ! (cd "$work" && ./Licasa.AppImage --appimage-extract >extract.log 2>&1); then
  echo "Could not extract the AppImage. It may be damaged, incompatible with this system, or stored on a filesystem that blocks execution." >&2
  echo "Download it again from the official release, check its SHA-256 file, and retry from a writable executable location." >&2
  while IFS= read -r line; do printf '%s\n' "$line" >&2; done < "$work/extract.log"
  exit 1
fi
extracted="$work/squashfs-root"
source_desktop="$extracted/usr/share/applications/licasa.desktop"
source_icon="$extracted/usr/share/icons/hicolor/512x512/apps/licasa.png"
for required in "$extracted/AppRun" "$extracted/usr/bin/licasa" "$source_desktop" "$source_icon"; do
  if [[ ! -f $required ]]; then
    echo "The AppImage is missing $required" >&2
    exit 1
  fi
done
if ! licasa_check_runtime_dependencies \
    "$extracted/usr/lib/licasa/lib:$extracted/usr/lib" "$extracted/usr"; then
  echo "Installation stopped before replacing any existing Licasa application." >&2
  echo "Detected C library: $libc_version. Qt and image codecs should come from the AppImage; FUSE is not required." >&2
  exit 1
fi
write_desktop "$source_desktop" > "$extracted/.licasa-appimage.desktop"
printf '%s\n' "$marker" > "$extracted/.licasa-appimage-install"
printf '%s\0' 1 "$data_home" "$config_home" > "$extracted/.licasa-appimage-paths"
cp -- "${BASH_SOURCE[0]}" "$extracted/install-licasa-appimage.sh"
find_lifecycle_helper "$extracted"
identity_check=()
if [[ -f $extracted/usr/libexec/licasa/lifecycle_update.py ]]; then
  identity_check=(--identity-check)
fi
# Retain the controller with legacy payloads too. It still refuses to stop an
# uncooperative old viewer and requires a one-time full Quit for that bridge.
if [[ ! -f $extracted/usr/libexec/licasa/lifecycle_update.py ]]; then
  mkdir -p "$extracted/usr/libexec/licasa"
  cp -- "$lifecycle_helper" "$extracted/usr/libexec/licasa/lifecycle_update.py"
fi
lock_lifecycle
mkdir -p "$desktop_dir" "$icon_dir"
remember_integration "$desktop"
remember_integration "$icon"
# Prepare desktop/icon files beside their destinations for atomic renames, even
# when XDG_DATA_HOME is on a different filesystem.
desktop_new=$(mktemp "$desktop_dir/.licasa-desktop.XXXXXXXX")
icon_new=$(mktemp "$icon_dir/.licasa-icon.XXXXXXXX")
# These paths are removed by the transaction's EXIT trap above.
cp -- "$extracted/.licasa-appimage.desktop" "$desktop_new"
cp -- "$source_icon" "$icon_new"
chmod 644 "$desktop_new" "$icon_new"
begin_lifecycle
lifecycle mark --state replacing
if [[ -e $target ]]; then
  mv -- "$target" "$backup"
fi
app_changed=1
mv -- "$extracted" "$target"
integration_changed[1]=1
mv -fT -- "$icon_new" "$icon"
integration_changed[0]=1
mv -fT -- "$desktop_new" "$desktop"
lifecycle mark --state committed
lifecycle verify "${identity_check[@]}"
lifecycle_committed=1
committed=1
refresh_desktop installed
echo "Installed Licasa in $target"
echo "Find 'Licasa (AppImage)' in your application menu or use Open With in your file manager."
echo "Remove it with: bash \"$target/install-licasa-appimage.sh\" --uninstall"
echo "A graphical desktop is needed to open windows; installation did not start the application."
echo "Background startup is deferred to the next desktop login."
