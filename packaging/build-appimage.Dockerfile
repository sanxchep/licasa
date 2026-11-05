FROM ubuntu:22.04

RUN apt-get -o APT::Update::Error-Mode=any update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++ make python3 pkg-config patch git meson nasm \
    autoconf automake libtool file binutils patchelf desktop-file-utils perl xz-utils \
    libopenjp2-7-dev libjpeg-turbo8-dev libtiff-dev libpng-dev libwebp-dev zlib1g-dev \
    libegl-dev libgl-dev libdrm-dev libpulse-dev libasound2-dev libv4l-dev \
    libdbus-1-dev libgtk-3-dev dconf-gsettings-backend libssl-dev libicu-dev \
    libfontconfig1-dev libfreetype-dev libharfbuzz-dev libpcre2-dev libdouble-conversion-dev \
    libx11-dev libx11-xcb-dev libxext-dev libxrender-dev libxi-dev libsm-dev libice-dev \
    libxcb1-dev libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev \
    libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev \
    libxcb-sync-dev libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xkb-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libwayland-dev libwayland-egl-backend-dev \
    wayland-protocols \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
