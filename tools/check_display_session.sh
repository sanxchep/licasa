#!/bin/sh
# Exercise an installed Licasa viewer and its production-QML diagnostic in a
# virtual X11 or Wayland session. Called from clean-package verification images.
set -eu

# Keep the final graphics decision usable against saved logs as well as a live
# run. An OpenGL API can still be backed by llvmpipe, so it is not hardware proof.
graphics_api() {
    sed -n 's/.*Qt Quick backend (sceneGraphInitialized): graphicsApi=\([A-Za-z0-9]*\).*/\1/p' "$1" | tail -1
}
graphics_renderer() {
    sed -n 's/.*OpenGL VENDOR: .* RENDERER: \(.*\) VERSION: .*/\1/p' "$1" | tail -1
}
check_graphics_results() {
    diagnostic_log=$1
    application_log=$2
    diagnostic_fallback=$3
    graphics_policy=$4
    case "$diagnostic_fallback:$graphics_policy" in
        true:allow-fallback|false:allow-fallback|true:require-accelerated|false:require-accelerated) ;;
        *) echo "Invalid graphics result policy" >&2; return 2 ;;
    esac
    diagnostic_api=$(graphics_api "$diagnostic_log")
    application_api=$(graphics_api "$application_log")
    diagnostic_renderer=$(graphics_renderer "$diagnostic_log")
    application_renderer=$(graphics_renderer "$application_log")
    application_retry=false
    if grep -Fq 'retrying once with the Qt software renderer' "$application_log"; then
        application_retry=true
    fi
    for actual_api in "$diagnostic_api" "$application_api"; do
        case "$actual_api" in
            OpenGL|Vulkan|Software|OpenVG|Direct3D11|Direct3D12|Metal) ;;
            *) echo "Missing or unusable initialized graphics API: ${actual_api:-unreported}" >&2; return 1 ;;
        esac
    done
    if [ "$diagnostic_fallback" = true ] && [ "$diagnostic_api" != Software ]; then
        echo "Diagnostic fallback did not initialize the software renderer" >&2
        return 1
    fi
    if [ "$application_retry" = true ] && [ "$application_api" != Software ]; then
        echo "Application retry did not initialize the software renderer" >&2
        return 1
    fi
    echo "Diagnostic graphics: graphicsApi=$diagnostic_api; renderer=${diagnostic_renderer:-unreported}; fallback=$diagnostic_fallback"
    echo "Application graphics: graphicsApi=$application_api; renderer=${application_renderer:-unreported}; retry=$application_retry"
    if [ "$graphics_policy" = require-accelerated ]; then
        if [ "$diagnostic_fallback" = true ] || [ "$application_retry" = true ]; then
            echo "Accelerated qualification rejected a software fallback" >&2
            return 1
        fi
        for renderer_pair in "$diagnostic_api:$diagnostic_renderer" "$application_api:$application_renderer"; do
            case "$renderer_pair" in
                OpenGL:?*) ;;
                *) echo "Accelerated qualification requires an initialized API and a verifiable hardware renderer" >&2; return 1 ;;
            esac
            if printf '%s\n' "$renderer_pair" | grep -Eiq 'llvmpipe|softpipe|lavapipe|software[ _-]*(rasterizer|renderer)|swrast|openswr|swiftshader'; then
                echo "Accelerated qualification rejected a software graphics driver: $renderer_pair" >&2
                return 1
            fi
        done
    fi
}

# Read-only assessment of retained qualification artifacts, also used by the
# regression tests. This route does not start a display or an application.
if [ "${1:-}" = --check-graphics-results ]; then
    if [ "$#" -ne 5 ]; then
        echo "Usage: $0 --check-graphics-results DIAGNOSTIC_LOG APPLICATION_LOG true|false allow-fallback|require-accelerated" >&2
        exit 2
    fi
    shift
    check_graphics_results "$@"
    exit $?
fi

if [ "$#" -lt 5 ] || [ "$#" -gt 6 ]; then
    echo "Usage: $0 APP DIAGNOSTIC PHOTO x11|wayland default|software [allow-fallback|require-accelerated]" >&2
    exit 2
fi
app=$1
diagnostic=$2
photo=$3
protocol=$4
mode=$5
graphics_policy=${6:-allow-fallback}

case "$protocol:$mode" in
    x11:default|x11:software|wayland:default|wayland:software) ;;
    *) echo "Unsupported display check: $protocol:$mode" >&2; exit 2 ;;
esac
case "$mode:$graphics_policy" in
    default:allow-fallback|default:require-accelerated|software:allow-fallback) ;;
    *) echo "Unsupported graphics policy: $mode:$graphics_policy" >&2; exit 2 ;;
esac

mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "${XDG_CONFIG_HOME:-$HOME/.config}"
chmod 700 "$XDG_RUNTIME_DIR"
unset QT_QUICK_BACKEND QSG_RHI_BACKEND LIBGL_ALWAYS_SOFTWARE LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
# The API and real driver must be captured for both the diagnostic and normal
# launcher. QSG_INFO alone does not enable Qt 6.4's RHI driver information.
export QSG_INFO=1
export QT_LOGGING_RULES="${QT_LOGGING_RULES:+$QT_LOGGING_RULES;}qt.rhi.general.debug=true"
if [ "$mode" = software ]; then
    export QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1
fi

display_pid=
x11_pid=
viewer_pid=
cleanup() {
    [ -z "$viewer_pid" ] || kill "$viewer_pid" 2>/dev/null || true
    [ -z "$display_pid" ] || kill "$display_pid" 2>/dev/null || true
    [ -z "$x11_pid" ] || kill "$x11_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

if [ "$protocol" = x11 ]; then
    export DISPLAY=:90 QT_QPA_PLATFORM=xcb
    Xvfb :90 -ac -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
    display_pid=$!
    x11_socket=/tmp/.X11-unix/X90
else
    export DISPLAY=:91 WAYLAND_DISPLAY=wayland-licasa QT_QPA_PLATFORM=wayland
    Xvfb :91 -ac -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
    x11_pid=$!
    x11_socket=/tmp/.X11-unix/X91
fi

attempt=0
while [ ! -S "$x11_socket" ] && [ "$attempt" -lt 40 ]; do
    sleep 0.25
    attempt=$((attempt + 1))
done
if [ ! -S "$x11_socket" ]; then
    cat /tmp/xvfb.log >&2
    echo "X11 test display did not start" >&2
    exit 1
fi

if [ "$protocol" = wayland ]; then
    if weston --help 2>&1 | grep -q -- '--renderer'; then
        set -- --backend=x11 --renderer=pixman
    else
        set -- --backend=x11-backend.so --use-pixman
    fi
    weston "$@" --socket="$WAYLAND_DISPLAY" \
        --idle-time=0 >/tmp/weston.log 2>&1 &
    display_pid=$!
    attempt=0
    while [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && [ "$attempt" -lt 40 ]; do
        sleep 0.25
        attempt=$((attempt + 1))
    done
    if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
        cat /tmp/weston.log >&2
        echo "Wayland test display did not start" >&2
        exit 1
    fi
fi

# The diagnostic must resolve its libraries with the same deployed ELF search
# paths as the production launcher. A diagnostic-only LD_LIBRARY_PATH can hide
# broken plugin RUNPATHs, so only the package's Qt plugin/QML paths are selected.
diagnostic_usr=$(dirname "$(dirname "$diagnostic")")
run_diagnostic() (
    export QT_PLUGIN_PATH="$diagnostic_usr/plugins:$diagnostic_usr/lib/licasa/plugins:$diagnostic_usr/lib/qt6/plugins"
    export QML2_IMPORT_PATH="$diagnostic_usr/qml:$diagnostic_usr/lib/qt6/qml"
    export QML_IMPORT_PATH="$QML2_IMPORT_PATH"
    if [ "$1" = software ]; then
        export QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1
    fi
    timeout 45 "$diagnostic" --viewer "$photo"
)
find_live_licasa_child() {
    for comm_file in /proc/[0-9]*/comm; do
        [ -r "$comm_file" ] || continue
        pid_dir=${comm_file%/comm}
        candidate_pid=${pid_dir##*/}
        [ "$candidate_pid" != "$viewer_pid" ] || continue
        IFS= read -r candidate_name < "$comm_file" || continue
        # Native packages keep the viewer ELF beside the pre-loader launcher.
        case "$candidate_name" in licasa|licasa.bin) ;; *) continue ;; esac
        read -r stat_pid stat_comm stat_state stat_rest < "$pid_dir/stat" || continue
        if [ "$stat_state" != Z ]; then
            echo "$candidate_pid"
            return 0
        fi
    done
    return 1
}
diagnostic_fallback=false
if run_diagnostic "$mode" >/tmp/licasa-display-report.json \
    2>/tmp/licasa-display-diagnostic.log; then
    :
else
    status=$?
    cp /tmp/licasa-display-report.json /tmp/licasa-display-report.initial.json
    cp /tmp/licasa-display-diagnostic.log /tmp/licasa-display-diagnostic.initial.log
    if [ "$mode" = default ] && [ "$status" -eq 43 ]; then
        diagnostic_fallback=true
        if [ "$graphics_policy" = require-accelerated ]; then
            cat /tmp/licasa-display-diagnostic.log >&2
            echo "Accelerated diagnostic failed; software fallback is not allowed in this qualification" >&2
            exit 1
        fi
        if ! run_diagnostic software >/tmp/licasa-display-report.json \
            2>/tmp/licasa-display-diagnostic.log; then
            cat /tmp/licasa-display-diagnostic.log >&2
            cat /tmp/licasa-display-report.json >&2
            exit 1
        fi
    else
        cat /tmp/licasa-display-diagnostic.log >&2
        cat /tmp/licasa-display-report.json >&2
        exit 1
    fi
fi
for field in first_window_frame_ms first_preview_presented_ms window_exposed_ms; do
    if ! grep -Fq "\"$field\"" /tmp/licasa-display-report.json; then
        cat /tmp/licasa-display-report.json >&2
        echo "Viewer did not report $field" >&2
        exit 1
    fi
done
if grep -Fq '"image_failed": true' /tmp/licasa-display-report.json; then
    cat /tmp/licasa-display-report.json >&2
    echo "Viewer failed to open its first photo" >&2
    exit 1
fi
if ! grep -Fq 'Qt Quick backend (sceneGraphInitialized): graphicsApi=' \
    /tmp/licasa-display-diagnostic.log; then
    cat /tmp/licasa-display-diagnostic.log >&2
    echo "Qt Quick scene graph did not initialize" >&2
    exit 1
fi

"$app" "$photo" >/tmp/licasa-display-app.log 2>&1 &
viewer_pid=$!
app_retried=false
attempt=0
while [ "$attempt" -lt 60 ]; do
    if ! kill -0 "$viewer_pid" 2>/dev/null; then
        if [ "$mode" = default ] && [ "$app_retried" = false ] &&
            grep -Fq 'retrying once with the Qt software renderer' \
                /tmp/licasa-display-app.log; then
            child_pid=
            child_attempt=0
            while [ -z "$child_pid" ] && [ "$child_attempt" -lt 20 ]; do
                child_pid=$(find_live_licasa_child || true)
                [ -n "$child_pid" ] || sleep 0.25
                child_attempt=$((child_attempt + 1))
            done
            if [ -n "$child_pid" ]; then
                viewer_pid=$child_pid
                app_retried=true
            fi
        fi
        if ! kill -0 "$viewer_pid" 2>/dev/null; then
            cat /tmp/licasa-display-app.log >&2
            echo "Installed viewer exited during launch" >&2
            exit 1
        fi
    fi
    if [ "$protocol" = x11 ]; then
        if xwininfo -root -tree 2>/dev/null | grep -q '"Licasa"'; then
            break
        fi
    elif [ "$app_retried" = true ] ||
        grep -Fq 'Qt Quick backend (sceneGraphInitialized): graphicsApi=' \
            /tmp/licasa-display-app.log; then
        break
    fi
    sleep 0.25
    attempt=$((attempt + 1))
done
if [ "$attempt" -ge 60 ]; then
    cat /tmp/licasa-display-app.log >&2
    echo "Installed viewer did not show a window" >&2
    exit 1
fi
sleep 2
if ! kill -0 "$viewer_pid" 2>/dev/null; then
    cat /tmp/licasa-display-app.log >&2
    echo "Installed viewer did not remain open" >&2
    exit 1
fi
check_graphics_results /tmp/licasa-display-diagnostic.log /tmp/licasa-display-app.log \
    "$diagnostic_fallback" "$graphics_policy"
if [ "$mode" = software ] &&
    { [ "$diagnostic_api" != Software ] || [ "$application_api" != Software ]; }; then
    echo "Explicit software qualification did not use the software backend in both processes" >&2
    exit 1
fi
echo "Display session passed: $protocol $mode; diagnostic_fallback=$diagnostic_fallback; app_retry=$application_retry"
