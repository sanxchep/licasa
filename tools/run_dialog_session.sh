#!/bin/sh
# Run a package interaction harness in an isolated virtual desktop session.
set -eu
protocol=$1
shift
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
export DISPLAY=:93
log_directory=${LICASA_DIALOG_LOG_DIRECTORY:-/tmp}
Xvfb "$DISPLAY" -ac -screen 0 1280x800x24 -nolisten tcp >"$log_directory/dialog-xvfb.log" 2>&1 &
x11_pid=$!
wayland_pid=
window_manager_pid=
cleanup() {
    [ -z "$wayland_pid" ] || kill "$wayland_pid" 2>/dev/null || true
    [ -z "$window_manager_pid" ] || kill "$window_manager_pid" 2>/dev/null || true
    kill "$x11_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
attempt=0
while [ ! -S /tmp/.X11-unix/X93 ] && [ "$attempt" -lt 50 ]; do
    sleep 0.1
    attempt=$((attempt + 1))
done
if [ ! -S /tmp/.X11-unix/X93 ]; then
    cat "$log_directory/dialog-xvfb.log" >&2
    exit 1
fi
case "$protocol" in
    x11)
        export QT_QPA_PLATFORM=xcb
        # Qt 6.8 uses separate popup windows. A real window manager must
        # complete activation and focus changes before keyboard tests run.
        export LICASA_DIALOG_WM_READY="$XDG_RUNTIME_DIR/licasa-window-manager-ready"
        openbox --sm-disable --startup "touch \"$LICASA_DIALOG_WM_READY\"" \
            >"$log_directory/dialog-window-manager.log" 2>&1 &
        window_manager_pid=$!
        attempt=0
        # EWMH properties appear before Openbox finishes its initial window
        # scan. Its startup callback prevents fast software clients racing it.
        while [ ! -f "$LICASA_DIALOG_WM_READY" ] && [ "$attempt" -lt 100 ]; do
            sleep 0.1
            attempt=$((attempt + 1))
        done
        if [ ! -f "$LICASA_DIALOG_WM_READY" ] \
                || ! xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id #'; then
            cat "$log_directory/dialog-window-manager.log" >&2
            exit 1
        fi
        ;;
    wayland)
        export WAYLAND_DISPLAY=wayland-dialog-test QT_QPA_PLATFORM=wayland
        (
            if weston --help 2>&1 | grep -q -- '--renderer'; then
                set -- --backend=x11 --renderer=pixman
            else
                set -- --backend=x11-backend.so --use-pixman
            fi
            exec weston "$@" --socket="$WAYLAND_DISPLAY" --idle-time=0
        ) >"$log_directory/dialog-weston.log" 2>&1 &
        wayland_pid=$!
        attempt=0
        while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ "$attempt" -lt 50 ]; do
            sleep 0.1
            attempt=$((attempt + 1))
        done
        if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            cat "$log_directory/dialog-weston.log" >&2
            exit 1
        fi
        ;;
    *) echo "Unsupported dialog protocol: $protocol" >&2; exit 2 ;;
esac
"$@"
