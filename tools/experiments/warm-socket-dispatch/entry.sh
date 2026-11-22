#!/usr/bin/env bash
set -euo pipefail
if [[ -n ${WARM08_JOB:-} ]]; then
    printf '%s\n' "$EPOCHREALTIME" > "$WARM08_JOB/wrapper.wall"
fi
[[ $# == 6 ]]
route=$1 archive=$2 receipt=$3 socket_name=$4 viewer_sha=$5 target=$6
if [[ -n ${WARM08_JOB:-} ]]; then
    [[ $target == "$(<"$WARM08_JOB/target")" ]]
    exec >"$WARM08_JOB/launcher.stdout" 2>"$WARM08_JOB/launcher.stderr"
    printf '%s %s\n' "${XDG_ACTIVATION_TOKEN+x}" "${DESKTOP_STARTUP_ID+x}" > "$WARM08_JOB/token-presence"
    export XDG_RUNTIME_DIR="$(<"$WARM08_JOB/runtime")"
    export XDG_CONFIG_HOME="$WARM08_JOB/viewer-config" XDG_CACHE_HOME="$WARM08_JOB/viewer-cache"
    export XDG_DATA_HOME="$WARM08_JOB/viewer-data" XDG_STATE_HOME="$WARM08_JOB/viewer-state"
    export WAYLAND_DISPLAY="$WARM08_WAYLAND" QT_QPA_PLATFORM=wayland
    export LD_PRELOAD="$WARM08_BOOT:$WARM08_PROBE" PERF05_PROBE="$WARM08_STAGE"
    export PYTHONDONTWRITEBYTECODE=1
fi
case "$route" in
    stock) direct=false ;;
    python) direct=true ;;
    fast)
        # A missing socket takes the ordinary AppImage path. A present socket
        # still has to pass all of the Python dispatcher's admission checks.
        direct=false
        if [[ -n ${XDG_RUNTIME_DIR:-} && -S "$XDG_RUNTIME_DIR/$socket_name" ]]; then
            direct=true
        fi
        ;;
    *) exit 2 ;;
esac
if "$direct"; then
    args=(/usr/bin/python3 "${0%/*}/dispatch.py" "$archive" "$receipt" "$viewer_sha" "$target")
    branch=python
else
    args=("$archive" "$target")
    branch=appimage
fi
if [[ -z ${WARM08_JOB:-} ]]; then
    exec "${args[@]}"
fi
printf '%s %s\n' "$EPOCHREALTIME" "$branch" > "$WARM08_JOB/branch.wall"
status=0
/usr/bin/time -f '%U %S %M %x' -o "$WARM08_JOB/launcher-resources.txt" "${args[@]}" || status=$?
printf '%s\n' "$status" > "$WARM08_JOB/launcher-exit"
exit "$status"
