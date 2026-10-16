<p align="center">
<img src="assets/licasa.png" width="512" alt="Licasa logo"/>
</p>

[![Build and verify](https://github.com/sanxchep/licasa/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sanxchep/licasa/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/sanxchep/licasa?label=release)](https://github.com/sanxchep/licasa/releases)
[![Commit activity](https://img.shields.io/github/commit-activity/m/sanxchep/licasa?label=commit%20activity)](https://github.com/sanxchep/licasa/graphs/commit-activity)
[![License](https://img.shields.io/github/license/sanxchep/licasa)](LICENSE)

# Licasa

Licasa is an image viewer and editor for the Linux desktop. It opens a photo from your file manager in an image-sized fullscreen window or in a floating mode.

**Licasa is an independent open-source project and is not affiliated with or endorsed by Google.**

## Try Licasa

**[Download the latest published release](https://github.com/sanxchep/licasa/releases/latest)** · [0.5.0 release notes](packaging/release-notes/v0.5.0.md)

- View photos fullscreen or in a floating window, then crop, adjust, and export them.
- Open JPEG, PNG, TIFF, WebP, HEIC/HEIF, JPEG XL, AVIF, JPEG 2000, and RAW previews.
- Play Motion Photos and animated APNG files.

If you try it, [report a bug or suggest a feature](https://github.com/sanxchep/licasa/issues/new).

## Install

- **AppImage** (`Licasa-0.5.0-x86_64.AppImage`) and `install-licasa-appimage.sh`: for recent x86-64 Linux desktops. The package is built on Ubuntu 24.04, so older systems may not run it.
- **Debian package** (`licasa_0.5.0-1_amd64.deb`): for Ubuntu 24.04 on amd64. APT installs the required system libraries and QML modules.
- **Snap beta**: available through the Snap Store on systems with Snapd.

The GitHub prerelease and final release include `licasa_0.5.0_amd64.snap` and a checksum file. If you want to install that file directly, check its checksum and run `sudo snap install --dangerous ./licasa_0.5.0_amd64.snap`. A directly installed Snap does not follow Store updates; use a Store channel for automatic updates.

Download the AppImage and installer script into the same folder, then run:

```sh
bash install-licasa-appimage.sh ./Licasa-0.5.0-x86_64.AppImage
```

To install the Debian package:

```sh
sudo apt install ./licasa_0.5.0-1_amd64.deb
```

To install the Snap beta:

```sh
sudo snap install licasa --beta
```

In **Settings**, turn on **Background process** if you want faster repeat opens. Licasa then stays ready when you close its window and starts in the background at your next desktop login, including after a reboot. This is off by default. Turning it off removes the login startup entry; closing Licasa then exits it. This works through your desktop session, so it does not start before you log in.

If Licasa has trouble, its private error log is at `$XDG_STATE_HOME/licasa/errors.log` (normally `~/.local/state/licasa/errors.log`; inside the Snap's user data for Snap installs). It records warnings and errors, keeps one rotated copy, and never uploads anything. Log messages can include photo paths, so review the file before attaching it to a bug report.

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
(cd build/deb-output && sha256sum -c licasa_0.5.0-1_amd64.deb.sha256)
(cd build/deb-output && sha256sum -c Licasa-0.5.0-x86_64.AppImage.sha256)
(cd build/deb-output && sha256sum -c install-licasa-appimage.sh.sha256)
```

Before releasing an AppImage, run its [installation checks](docs/linux-package-qualification.md). They install and launch the actual file without FUSE on clean Ubuntu and Fedora, then test virtual X11 and Wayland displays on Arch.

The [release process](docs/release-process.md) builds and verifies all Linux packages in GitHub Actions, uploads the Snap to Store edge, and publishes a GitHub prerelease. A manually started approval run promotes that exact Snap revision to beta and marks the same GitHub release as Latest.

## Update

Download the newer package and its `.sha256` file from [Releases](https://github.com/sanxchep/licasa/releases/latest). To update a Debian installation, replace the version in these commands:

```sh
sha256sum -c licasa_0.5.0-1_amd64.deb.sha256
sudo apt install ./licasa_0.5.0-1_amd64.deb
```

To update an AppImage installation, close Licasa (AppImage), then download the new AppImage, `install-licasa-appimage.sh`, and both `.sha256` files into the same folder. Replace the version in these commands:

```sh
sha256sum -c Licasa-0.5.0-x86_64.AppImage.sha256
sha256sum -c install-licasa-appimage.sh.sha256
bash install-licasa-appimage.sh ./Licasa-0.5.0-x86_64.AppImage
```

The installer replaces the existing per-user AppImage installation and refreshes its application-menu launcher.

## Uninstall

```sh
sudo apt remove licasa
```

To remove an AppImage installation, delete `~/.local/opt/licasa-appimage`, `~/.local/share/applications/licasa-appimage.desktop`, and `~/.local/share/icons/hicolor/512x512/apps/licasa-appimage.png`.

## License

Licasa's first-party source is licensed under the [BSD 2-Clause License](LICENSE). Third-party components and test fixtures have their own licenses, which are retained with the distributed materials.
