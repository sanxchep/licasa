FROM debian:trixie-slim

# A real MATE session and AT-SPI bus, with no system Qt/QML installation that
# could accidentally repair the portable package's runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates python3 python3-pyatspi at-spi2-core dbus-x11 \
    mate-session-manager marco mate-settings-daemon mate-panel caja orca \
    speech-dispatcher speech-dispatcher-espeak-ng python3-speechd \
    xvfb xauth xdotool x11-utils fonts-dejavu-core libgomp1 \
    libgl1 libegl1 libopengl0 libglx-mesa0 libfontconfig1 libxkbcommon0 libx11-xcb1 \
    libxcb-cursor0 libxcb-icccm4 libxcb-keysyms1 libxcb-image0 \
    libxcb-render-util0 libxcb-randr0 libxcb-xinerama0 \
    && rm -rf /var/lib/apt/lists/*

# Speech Dispatcher also initializes its built-in eSpeak fallback when dummy
# output is selected. --no-install-recommends must not leave that binary absent.
RUN test -x /usr/lib/speech-dispatcher-modules/sd_dummy \
    && test -x /usr/lib/speech-dispatcher-modules/sd_espeak-ng
