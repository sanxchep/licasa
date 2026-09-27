FROM ubuntu:24.04

# Deliberately omit libheif, libde265 and development packages. Modern native
# codecs must be provided by the installed Licasa package copied below.
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 libgomp1 libopenjp2-7 libqt6quick6 libqt6quickdialogs2-6 libqt6svg6 \
    qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    qml6-module-qtquick-controls qml6-module-qtquick-templates \
    qml6-module-qtquick-dialogs qml6-module-qtquick-layouts \
    && rm -rf /var/lib/apt/lists/*

COPY . /opt/licasa/
ENV QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software QSG_RENDER_LOOP=basic
ENV XDG_CONFIG_HOME=/tmp/config XDG_CACHE_HOME=/tmp/cache XDG_RUNTIME_DIR=/tmp/runtime
WORKDIR /tmp
