<p align="center">
<img src="assets/licasa.png" width="512" alt="Licasa logo"/>
</p>

[![Build and verify](https://github.com/sanxchep/licasa/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sanxchep/licasa/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/sanxchep/licasa?label=release)](https://github.com/sanxchep/licasa/releases)
[![Commit activity](https://img.shields.io/github/commit-activity/m/sanxchep/licasa?label=commit%20activity)](https://github.com/sanxchep/licasa/graphs/commit-activity)
[![License](https://img.shields.io/github/license/sanxchep/licasa)](LICENSE)

# Licasa

Licasa is an image viewer and editor for the Linux desktop inspired by Google's Picasa Photo Viewer. It opens a photo from your file manager in an image-sized fullscreen window or in a floating mode.

**Licasa is an independent open-source project and is not affiliated with or endorsed by Google.**

## Try Licasa

**[Download published releases](https://github.com/sanxchep/licasa/releases)** · [0.9.0 release notes](packaging/release-notes/v0.9.0.md)

- View photos fullscreen or in a floating window, then crop, adjust, and export them.
- Open JPEG, PNG, TIFF, WebP, HEIC/HEIF, JPEG XL, AVIF, JPEG 2000, and RAW previews.
- Play Motion Photos and animated APNG files.

If you try it, [report a bug or suggest a feature](https://github.com/sanxchep/licasa/issues/new).

## In action

**Open a photo** — From the desktop into the fullscreen viewer.

![Opening a photo in Licasa at original speed](assets/demos/open-photo.gif)

**Resize a floating window** — Open a photo, move it, and resize it on the desktop.

![Opening, moving, and resizing a Licasa floating window at original speed](assets/demos/floating-resize.gif)

**Keep multiple windows open** — Open additional views and arrange floating windows.

![Opening additional Licasa floating windows and arranging four views at original speed](assets/demos/multiple-windows.gif)

## Install

Choose the package built for your distribution. All current packages target
**x86-64 Intel/AMD Linux**. The Ubuntu native package requires Ubuntu 24.04;
the Debian native package requires Debian 13 (Trixie). Their Qt, JPEG and FFmpeg
libraries differ, so editing dependency names cannot make one package work on
the other distribution.

### Before updating an existing installation

Finish edits, saves and exports, then **fully Quit Licasa** using
**Settings → Quit Licasa** or **Ctrl+Shift+Q**. On older versions, use
Ctrl+Shift+Q. Closing the last window can leave a background process running.
This full-Quit step is required for the first upgrade or removal of a legacy
build without the new lifecycle control, and whenever replacing a raw portable
AppImage. It is especially important for the first Arch upgrade: a new package
cannot install its protective hook before that same transaction starts.

Once the new lifecycle support is installed, its installer/package-manager
integration can stop a verified idle background viewer cleanly. A visible
window, unfinished edit, open dialog, save/export, or uncertain owned process
can block the operation. Finish the work and Quit, then retry the same command.
Keep using the package family you installed; moving between Snap, native,
managed AppImage and Local builds requires a separate ownership handoff.


### Snap (systems with Snapd)

Install the version currently published to the stable Store channel:

```sh
sudo snap install licasa
```

For an existing Store installation, check its tracked channel for updates:

```sh
sudo snap refresh licasa
snap list licasa
```

A release `.snap` file is a separate, manually installed artifact:

```sh
sha256sum -c licasa_0.9.0_amd64.snap.sha256
sudo snap install --dangerous ./licasa_0.9.0_amd64.snap
```

`--dangerous` bypasses Store assertion verification for a local file. Keep
checking the published checksum and obtain the file from the trusted release.
Local-file installations do not automatically follow Store updates.

### Ubuntu 24.04 (amd64)

Download the Ubuntu `.deb` and its checksum file, then run these commands in
the download directory. APT resolves the runtime and lifecycle dependencies.
On Ubuntu 26.04, use the AppImage instructions below.

```sh
sha256sum -c licasa_0.9.0-1_amd64.deb.sha256
sudo apt install ./licasa_0.9.0-1_amd64.deb
```

### Debian 13 / Trixie (amd64)

Use the Debian-specific `+deb13` package, built against Debian's Qt 6.8.2 and
multimedia libraries:

```sh
sha256sum -c licasa_0.9.0-1+deb13_amd64.deb.sha256
sudo apt install ./licasa_0.9.0-1+deb13_amd64.deb
```

### Fedora 44 (x86-64)

```sh
sha256sum -c licasa-appimage-0.9.0-1.fc44.x86_64.rpm.sha256
sudo dnf install ./licasa-appimage-0.9.0-1.fc44.x86_64.rpm
```

The RPM installs the bundled runtime through DNF and includes native transaction
coordination. It is a different installation from a per-user AppImage.

### Arch Linux (x86-64)

The ready-to-install package includes the lifecycle helper and pre-transaction
hook:

```sh
sha256sum -c licasa-appimage-0.9.0-1-x86_64.pkg.tar.zst.sha256
sudo pacman -U ./licasa-appimage-0.9.0-1-x86_64.pkg.tar.zst
```

To build the Arch package yourself, download `PKGBUILD`, `PKGBUILD.sha256`,
`PKGBUILD.sources.sha256`, `native_lifecycle.py`, `licasa.install`, and
`licasa-lifecycle.hook` from the **same release** into one directory. Review
the recipe and scripts before building. The helper, install script and hook are
required inputs; copying only `PKGBUILD` is insufficient.

```sh
sha256sum -c PKGBUILD.sha256
sha256sum -c PKGBUILD.sources.sha256
makepkg -si
```

Run `makepkg` as your regular user. It verifies/downloads the matching AppImage
and asks the package manager to install the result. Later transactions use the
installed hook to refuse unsafe replacement. The first legacy transaction
still requires full Quit beforehand.

### Managed AppImage (compatible x86-64 glibc desktops)

Download the AppImage, installer and both checksum files from the same release.
Run the installer as your regular desktop user, **without sudo**:

```sh
sha256sum -c Licasa-0.9.0-x86_64.AppImage.sha256
sha256sum -c install-licasa-appimage.sh.sha256
bash install-licasa-appimage.sh ./Licasa-0.9.0-x86_64.AppImage
```

This installs **Licasa (AppImage)** under `~/.local/opt/licasa-appimage`, with a
desktop entry and icon. It extracts the runtime, so FUSE is unnecessary for the
installed copy. Managed installation and its launcher require **Python 3**,
`flock` and the standard tools checked by the installer. Keep Python installed
afterward. The portable runtime uses a glibc 2.39 baseline and still needs
compatible system desktop libraries; it does not run on every Linux system.

For a pre-start support report:

```sh
bash install-licasa-appimage.sh --diagnose ./Licasa-0.9.0-x86_64.AppImage > licasa-support.txt
```

Omit the AppImage filename to inspect an existing installation. This reporting
mode does not launch/extract the AppImage or use the network. It reports system,
tool, FUSE, destination and installed-library checks. A zero exit status means
those checks passed, not that every graphical environment has been qualified.
Review reports before sharing them.

### Raw portable AppImage

To run the downloaded file directly:

```sh
sha256sum -c Licasa-0.9.0-x86_64.AppImage.sha256
chmod +x Licasa-0.9.0-x86_64.AppImage
./Licasa-0.9.0-x86_64.AppImage
```

### Build and install a Local development copy

For an already configured and built CMake tree:

```sh
python3 tools/deploy_local.py --build-dir ./build --launch
```

## Uninstall

Finish work and fully Quit first, especially when removing an older build.
Use the manager for the package actually installed:

```sh
sudo apt remove licasa             # Ubuntu or Debian native package
sudo dnf remove licasa-appimage    # Fedora RPM
sudo pacman -R licasa-appimage     # Arch package
sudo snap remove licasa            # Snap
```

For managed AppImage, use its saved removal script as your regular user:

```sh
bash "$HOME/.local/opt/licasa-appimage/install-licasa-appimage.sh" --uninstall
```

The managed remover coordinates with its own viewer and handles owned desktop,
icon and unmodified startup integration. Legacy standalone removal still
requires the prior full Quit. A raw portable file can be deleted after Quit;
remove any desktop entry you created manually. Local deployment can share
`~/.local` with unrelated software: do not remove that entire prefix to uninstall
Licasa.

## Troubleshooting and logs

The AppImage installer keeps a private log at
`$XDG_STATE_HOME/licasa/installation.log` (normally
`~/.local/state/licasa/installation.log`), with one rotated copy. The viewer's
warning/error log is `licasa/errors.log` under its writable state location;
Snap stores it in its own user data. Logs stay local and may contain photo or
installation paths. Review them before attaching them to an
[issue](https://github.com/sanxchep/licasa/issues/new).

Include the Licasa version, package filename, distribution, desktop and session
type, plus the command/error and support report when applicable. A package for
the wrong distribution, a lifecycle refusal and a missing system library need
different remedies; keep the original message.


<details>
<summary>Package support and runtime details</summary>

<!-- support-matrix:start -->
| Package / target | Architecture / libc baseline | Qt runtime | Candidate qualification |
| --- | --- | --- | --- |
| ubuntu-deb / Ubuntu 24.04 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| debian-deb / Debian 13 (Trixie) | x86-64 / glibc ≥ 2.41 | 6.8.2 | In development; not a published support claim |
| debian-deb / Debian 13 / MATE X11 / native GTK file picker | x86-64 / glibc ≥ 2.41 | 6.8.2 | In development; not a published support claim |
| appimage / Ubuntu 24.04 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| appimage / Ubuntu 26.04 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| appimage / Debian 13 (Trixie) | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| appimage / Debian 13 / MATE X11 / AT-SPI keyboard accessibility | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| appimage / Fedora 44 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| appimage / Arch Linux (rolling; tested image recorded in evidence) | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| rpm / Fedora 44 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| arch / Arch Linux (rolling; tested image recorded in evidence) | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| snap / Ubuntu 24.04 / GNOME X11 and Wayland | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| snap / Ubuntu 24.04 / Plasma X11 and Wayland | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
<!-- support-matrix:end -->

</details>

## Build the Linux packages

From the repository root, with Docker installed:

```sh
docker build -f packaging/build-deb.Dockerfile -t licasa-deb-builder packaging
mkdir -p build/deb-output
docker run --rm -v "$PWD:/source:ro" -v "$PWD/build/deb-output:/output" \
  licasa-deb-builder bash /source/packaging/verify-fresh-build.sh
```

The `.deb`, AppImage, and SHA-256 files appear in `build/deb-output/`. Docker is required only to build these packages from source. To check the local files:

```sh
(cd build/deb-output && sha256sum -c licasa_0.9.0-1_amd64.deb.sha256)
(cd build/deb-output && sha256sum -c Licasa-0.9.0-x86_64.AppImage.sha256)
(cd build/deb-output && sha256sum -c install-licasa-appimage.sh.sha256)
```

## License

Licasa's first-party source is licensed under the [BSD 2-Clause License](LICENSE). Third-party components and test fixtures have their own licenses, which are retained with the distributed materials.
