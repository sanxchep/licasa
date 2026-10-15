# Linux package checks

The release workflow installs its build dependencies in a fresh Ubuntu 24.04
container, copies only the source into that container, builds and runs CTest,
packages a `.deb`, then turns that `.deb` payload into an AppImage. Existing
local build outputs are excluded. It fails if any of these checks fail:

- Install the `.deb` through APT on a clean Ubuntu 24.04 image, inspect its
  launcher and dependencies, decode eight image formats, open a viewer, and
  verify its private warning log is writable.
- Install the downloaded AppImage without FUSE on a clean Ubuntu 24.04 image
  without Qt or codecs installed. Validate the desktop launcher, select it as
  the default JPEG viewer, launch it from the extracted installation, and
  decode eight formats including JPEG 2000. Check that the installed viewer
  writes its private warning log.
- Install the same AppImage and desktop launcher without FUSE on Fedora 44,
  select it as the default JPEG viewer, and launch a photo.
- Install and launch the same AppImage without FUSE as an unprivileged user on
  Arch Linux under X11 and Wayland. X11 must create a Licasa window; Wayland
  must initialize the Qt Quick scene graph and stay open. Decode the eight
  formats on Arch and check that every packaged ELF library and plugin
  resolves its dependencies there.

The Arch test uses Xvfb and a nested Weston compositor, including an input
seat. It tests the platform plugins and a current Arch userspace, but it does
not replace a hands-on test of KDE Plasma, hardware graphics drivers, desktop
integration on another person's computer. The supported installer extracts
the AppImage once and does not use FUSE.

To repeat the installation checks from the repository root with Docker installed
after following the README's package build commands:

```sh
python3 tools/verify_deb_container.py build/deb-output/licasa_0.5.0-1_amd64.deb
python3 tools/verify_appimage_container.py build/deb-output/Licasa-0.5.0-x86_64.AppImage
python3 tools/verify_appimage_fedora.py build/deb-output/Licasa-0.5.0-x86_64.AppImage
python3 tools/verify_appimage_desktops.py build/deb-output/Licasa-0.5.0-x86_64.AppImage
```

All four scripts use the packaged files. Their reports are saved under
`build/qualification/` and included with the release workflow's build artifact.
