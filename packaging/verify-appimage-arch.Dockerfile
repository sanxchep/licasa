FROM archlinux:latest

# Resolve the bundle and test installation with only declared host dependencies.
RUN sed -i 's/^#ParallelDownloads = 5/ParallelDownloads = 10/' /etc/pacman.conf \
    && pacman -Syu --noconfirm --needed \
       bash libx11 libxext libxcb libxkbcommon fontconfig harfbuzz dbus mesa libglvnd \
    && pacman -Scc --noconfirm
COPY --chmod=755 Licasa.AppImage /opt/Licasa.AppImage
COPY --chmod=755 install-licasa-appimage.sh /opt/install-licasa-appimage.sh
COPY check_appimage_install.sh /opt/check_appimage_install.sh
RUN cd /tmp && /opt/Licasa.AppImage --appimage-extract >/dev/null \
    && bash -ec '. /opt/install-licasa-appimage.sh; licasa_check_runtime_dependencies \
       /tmp/squashfs-root/usr/lib/licasa/lib:/tmp/squashfs-root/usr/lib /tmp/squashfs-root/usr' \
    && rm -rf /tmp/squashfs-root
# Add managed-installer tools only after the raw bundle's loader check.
RUN pacman -Syu --noconfirm --needed \
       bash coreutils findutils glibc sed util-linux python \
    && pacman -Scc --noconfirm
USER 65534:65534
RUN HOME=/tmp/licasa-install-test bash /opt/check_appimage_install.sh \
    /opt/install-licasa-appimage.sh /opt/Licasa.AppImage
USER 0:0
# Graphical test tools are added only after the minimal runtime gates pass.
RUN pacman -Syu --noconfirm --needed \
       xorg-server-xvfb xorg-xwininfo xorg-xprop openbox weston desktop-file-utils xdg-utils \
    && pacman -Scc --noconfirm
