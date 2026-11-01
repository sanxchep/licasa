# This native target deliberately rebuilds every private runtime against Debian's
# JPEG and FFmpeg ABIs; never reuse an Ubuntu build or codec prefix here.
FROM debian:13

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++ python3 pkg-config patch git meson nasm dpkg-dev file \
    binutils patchelf clang-format-19 \
    qt6-base-dev qt6-declarative-dev qt6-tools-dev qt6-tools-dev-tools qt6-svg-dev qt6-image-formats-plugins qt6-wayland \
    libopenjp2-7-dev libjpeg62-turbo-dev libtiff-dev libpng-dev libwebp-dev libxext-dev libxrandr-dev \
    qt6-base-private-dev qt6-declarative-private-dev qt6-shadertools-dev \
    qt6-multimedia-dev libpulse-dev libavcodec-dev libavformat-dev \
    libavutil-dev libswresample-dev libswscale-dev libavdevice-dev libavfilter-dev \
    libssl-dev libva-dev libvulkan-dev \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-models \
    qml6-module-qtqml-workerscript qml6-module-qtqml qml6-module-qtqml-xmllistmodel \
    qml6-module-qttest qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-dialogs qml6-module-qtquick-layouts \
    qml6-module-qt-labs-folderlistmodel \
    && rm -rf /var/lib/apt/lists/*

# Qt Multimedia requires EGL headers for DMA buffers and VAAPI. Keep this
# explicit: an optional CMake probe must not silently remove graphics support.
RUN apt-get update && apt-get install -y --no-install-recommends libegl-dev libdrm-dev qt6-svg-plugins \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
