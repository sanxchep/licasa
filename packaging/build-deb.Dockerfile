FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++ python3 pkg-config patch git meson nasm dpkg-dev \
    binutils clang-format-18 \
    qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-image-formats-plugins \
    libopenjp2-7-dev libjpeg-turbo8-dev libtiff-dev libxext-dev \
    qt6-base-private-dev qt6-declarative-private-dev qt6-shadertools-dev \
    qt6-multimedia-dev libpulse-dev libavcodec-dev libavformat-dev \
    libavutil-dev libswresample-dev libswscale-dev libavdevice-dev libavfilter-dev \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    qml6-module-qttest qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-dialogs qml6-module-qtquick-layouts \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
