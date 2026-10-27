#!/usr/bin/env bash
set -euo pipefail

apt-get update
apt-get install -y --no-install-recommends ca-certificates git cmake make ninja-build g++ python3 ccache \
    pkg-config patch meson nasm clang-format-18 binutils libjpeg-turbo8-dev libtiff-dev libpng-dev libwebp-dev \
    libx11-dev libxext-dev libopenjp2-7-dev libegl-dev libdrm-dev libva-dev \
    qt6-multimedia-dev libpulse-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libswscale-dev libavdevice-dev libavfilter-dev \
    qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-image-formats-plugins \
    qt6-base-private-dev qt6-declarative-private-dev qt6-shadertools-dev \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml qml6-module-qtqml-workerscript \
    qml6-module-qttest qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-dialogs qml6-module-qtquick-layouts qml6-module-qt-labs-folderlistmodel
