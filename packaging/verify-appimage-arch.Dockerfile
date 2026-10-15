FROM archlinux:latest

# Exercise the downloaded AppImage with Arch's X11 and Wayland display stacks,
# without installing Qt or Licasa from pacman.
RUN sed -i 's/^#ParallelDownloads = 5/ParallelDownloads = 10/' /etc/pacman.conf \
    && pacman -Syu --noconfirm --needed \
       xorg-server-xvfb xorg-xwininfo weston mesa \
       libx11 libxext libxcb libxkbcommon fontconfig dbus \
       desktop-file-utils xdg-utils \
    && pacman -Scc --noconfirm
