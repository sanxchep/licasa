<p align="center">
<img src="assets/licasa.png" width="512" alt="Licasa logo"/>
</p>

[![Build and verify](https://github.com/sanxchep/licasa/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sanxchep/licasa/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/sanxchep/licasa?label=release)](https://github.com/sanxchep/licasa/releases)
[![License](https://img.shields.io/github/license/sanxchep/licasa)](LICENSE)

# Licasa

Licasa is an image-first viewer with a simple editor for the Linux desktop, inspired by Google’s Picasa Photo Viewer. Open a photo from your file manager in fullscreen or a floating window. Licasa aims to show the photo quickly, across formats ranging from JPEG and PNG to HEIC, JPEG XL, and camera RAW and Motion Photos.

**Licasa is an independent open-source project and is not affiliated with or endorsed by Google.**

## In action

**Open a photo** — From the desktop into the fullscreen viewer.

![Opening a photo in Licasa at original speed](assets/demos/open-photo.gif)

**Resize a floating window** — Open a photo, move it, and resize it on the desktop.

![Opening, moving, and resizing a Licasa floating window at original speed](assets/demos/floating-resize.gif)

**Keep multiple windows open** — Open additional views and arrange floating windows.

![Opening additional Licasa floating windows and arranging four views at original speed](assets/demos/multiple-windows.gif)

## Install

Choose a package for your **x86-64 Intel or AMD** desktop. The AppImage is a good starting point if you want a per-user install.

| Desktop | Package |
| --- | --- |
| Ubuntu 22.04, 24.04, 26.04 · Debian 13 · Fedora 44 · Arch | AppImage |
| Ubuntu 24.04 | Ubuntu DEB |
| Debian 13 (Trixie) | Debian DEB with `+deb13` in the filename (experimental) |
| Fedora 44 | Fedora RPM |
| Arch Linux | Arch `.pkg.tar.zst` |
| Systems with snapd | Snap Store, with automatic updates on the tracked channel |

Download the [latest release](https://github.com/sanxchep/licasa/releases/latest) or browse [all releases](https://github.com/sanxchep/licasa/releases). The GitHub download commands below use 0.9.8.

<details>
<summary>Installation Steps for AppImage</summary>

1. From the [release page](https://github.com/sanxchep/licasa/releases/latest), download the AppImage, `install-licasa-appimage.sh`, and the matching `.sha256` file for each.
2. Open a terminal in the download folder. Run these commands as your regular user, without `sudo`.

   ```sh
   sha256sum -c Licasa-0.9.8-x86_64.AppImage.sha256 &&
     sha256sum -c install-licasa-appimage.sh.sha256 &&
     bash install-licasa-appimage.sh ./Licasa-0.9.8-x86_64.AppImage
   ```

The installer extracts the app under `~/.local/opt/licasa-appimage`. It works without FUSE. Keep Python 3 and `flock` installed for the launcher.

</details>

<details>
<summary>Installation Steps for Ubuntu DEB</summary>

1. Download the native package for **Ubuntu 24.04** and its matching `.sha256` file from the same release.
2. Open a terminal in the download folder. Verify and install the package.

   ```sh
   sha256sum -c licasa_0.9.8-1_amd64.deb.sha256 &&
     sudo apt install ./licasa_0.9.8-1_amd64.deb
   ```

</details>

<details>
<summary>Installation Steps for Debian DEB</summary>

1. Download the experimental `+deb13` package for **Debian 13 (Trixie)** and its matching `.sha256` file from the same release.
2. Open a terminal in the download folder. Verify and install the package.

   ```sh
   sha256sum -c licasa_0.9.8-1+deb13_amd64.deb.sha256 &&
     sudo apt install ./licasa_0.9.8-1+deb13_amd64.deb
   ```

</details>

<details>
<summary>Installation Steps for Fedora RPM</summary>

1. Download the package for **Fedora 44** and its matching `.sha256` file from the same release.
2. Open a terminal in the download folder. Verify and install the package.

   ```sh
   sha256sum -c licasa-appimage-0.9.8-1.fc44.x86_64.rpm.sha256 &&
     sudo dnf install ./licasa-appimage-0.9.8-1.fc44.x86_64.rpm
   ```

</details>

<details>
<summary>Installation Steps for Arch Linux</summary>

1. Download the package for **Arch Linux** and its matching `.sha256` file from the same release.
2. Open a terminal in the download folder. Verify and install the package.

   ```sh
   sha256sum -c licasa-appimage-0.9.8-1-x86_64.pkg.tar.zst.sha256 &&
     sudo pacman -U ./licasa-appimage-0.9.8-1-x86_64.pkg.tar.zst
   ```

</details>

<details>
<summary>Installation Steps for Snap Store</summary>

1. On a system with snapd, open a terminal and install Licasa from the stable channel.

   ```sh
   sudo snap install licasa
   ```

2. Run `snap list licasa` to see the installed version and tracked channel.

</details>

<details>
<summary>Installation Steps for Portable AppImage (run directly)</summary>

1. Download the AppImage and its matching `.sha256` file from the same release. Running it directly requires FUSE.
2. Open a terminal in the download folder. Verify the file, make it executable, and launch it.

   ```sh
   sha256sum -c Licasa-0.9.8-x86_64.AppImage.sha256 &&
     chmod +x Licasa-0.9.8-x86_64.AppImage &&
     ./Licasa-0.9.8-x86_64.AppImage
   ```

Fully quit before replacing the file and keep its path if you use shortcuts.

</details>

<details>
<summary>Installation Steps for Arch Linux (build from PKGBUILD)</summary>

1. Download `PKGBUILD`, `PKGBUILD.sha256`, `PKGBUILD.sources.sha256`, `native_lifecycle.py`, `licasa.install`, and `licasa-lifecycle.hook` from the same release.
2. Review the files, then open a terminal in the download folder and run these commands as your regular user.

   ```sh
   sha256sum -c PKGBUILD.sha256 &&
     sha256sum -c PKGBUILD.sources.sha256 &&
     makepkg -si
   ```

</details>

<details>
<summary>Update or uninstall</summary>

For GitHub downloads, install the new release using the same steps. Launch Licasa once and check your background preference. For Store installations, snapd handles updates. Run `sudo snap refresh licasa` to install an available update now. To move a manually installed Snap to Store updates, fully quit and run `sudo snap refresh --amend licasa --channel=stable`.

To uninstall, fully quit and use the command for your installation.

| Installed with | Remove |
| --- | --- |
| Managed AppImage | `bash "$HOME/.local/opt/licasa-appimage/install-licasa-appimage.sh" --uninstall` |
| APT | `sudo apt remove licasa` |
| DNF | `sudo dnf remove licasa-appimage` |
| pacman | `sudo pacman -R licasa-appimage` |
| Snap | `sudo snap remove licasa` |
| Raw AppImage | Delete the downloaded file and any shortcut you created for it. |

</details>

<details>
<summary>Troubleshooting and logs</summary>

To check an installed AppImage without opening the viewer, run the saved installer in diagnostic mode.

```sh
bash "$HOME/.local/opt/licasa-appimage/install-licasa-appimage.sh" --diagnose
```

If installation failed, use `bash install-licasa-appimage.sh --diagnose ./Licasa-0.9.8-x86_64.AppImage` from your download directory.

Installer and viewer logs are normally in `~/.local/state/licasa/` as `installation.log` and `errors.log`. A custom `XDG_STATE_HOME` or Snap installation can change the location. Review logs for personal paths before sharing them. When [reporting an issue](https://github.com/sanxchep/licasa/issues/new), include the package filename, distribution, desktop session, and exact error.

</details>

<details>
<summary>Package support and runtime details</summary>

<!-- support-matrix:start -->
| Package / target | Architecture / libc baseline | Qt runtime | Candidate qualification |
| --- | --- | --- | --- |
| ubuntu-deb / Ubuntu 24.04 | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| debian-deb / Debian 13 (Trixie) | x86-64 / glibc ≥ 2.41 | 6.8.2 | In development; not a published support claim |
| debian-deb / Debian 13 / MATE X11 / native GTK file picker | x86-64 / glibc ≥ 2.41 | 6.8.2 | In development; not a published support claim |
| appimage / Ubuntu 22.04 | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Ubuntu 24.04 | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Ubuntu 26.04 | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Debian 13 (Trixie) | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Debian 13 / MATE X11 / AT-SPI keyboard accessibility | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Fedora 44 | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| appimage / Arch Linux (rolling; tested image recorded in evidence) | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| rpm / Fedora 44 | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| arch / Arch Linux (rolling; tested image recorded in evidence) | x86-64 / glibc ≥ 2.35 | 6.4.2 | Required before stable |
| snap / Ubuntu 24.04 / GNOME X11 and Wayland | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
| snap / Ubuntu 24.04 / Plasma X11 and Wayland | x86-64 / glibc ≥ 2.39 | 6.4.2 | Required before stable |
<!-- support-matrix:end -->

</details>

<details>
<summary>Build from source and translations</summary>

## Build the Ubuntu package and AppImage

From the repository root, with Docker installed:

```sh
docker build -f packaging/build-deb.Dockerfile -t licasa-deb-builder packaging
mkdir -p build/deb-output
docker run --rm -v "$PWD:/source:ro" -v "$PWD/build/deb-output:/output" \
  licasa-deb-builder bash /source/packaging/verify-fresh-build.sh
```

Build the AppImage and its portable runtime in the Ubuntu 22.04 builder.

```sh
docker build -f packaging/build-appimage.Dockerfile -t licasa-appimage-builder packaging
mkdir -p build/appimage-work build/appimage-output
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/work -e PYTHONDONTWRITEBYTECODE=1 \
  -v "$PWD:/source:ro" -v "$PWD/build/appimage-work:/work" \
  -v "$PWD/build/appimage-output:/output" licasa-appimage-builder \
  python3 /source/tools/build_appimage.py --work-dir /work --output-dir /output --tools-dir /work/tools
install -m 0755 packaging/install-licasa-appimage.sh build/appimage-output/
(cd build/appimage-output && sha256sum install-licasa-appimage.sh > install-licasa-appimage.sh.sha256)
```

Verify the local package checksums.

```sh
(cd build/deb-output && sha256sum -c ./*.sha256)
(cd build/appimage-output && sha256sum -c ./*.sha256)
```

### Install a local development copy

After building Licasa in `./build`, run the deployment tool as your regular user.

```sh
python3 tools/deploy_local.py --build-dir ./build --launch
```

This installs **Licasa (Local)** into `~/.local` and launches it. It is separate
from the downloaded packages.

## Translations

Licasa uses Qt Linguist catalogs in [`translations/`](translations/). To update
the source strings or compile the catalogs, configure the project with Qt's
LinguistTools package installed, then run:

```sh
cmake --build ./build --target update_translations
cmake --build ./build --target release_translations
```

Edit the `.ts` catalog after updating it. In Settings, **Language** offers
**System default**, **English**, and **Svenska**. The choice is saved and applies
immediately to open windows. System default follows the computer's UI language,
with English as the fallback. Native file-dialog controls follow the desktop's
language; reopen an existing dialog to use updated application labels and filters.

The Swedish catalog (`licasa_sv.ts`) builds on
[yeager's contribution in PR #7](https://github.com/sanxchep/licasa/pull/7).

</details>

## License

Licasa's first-party source is licensed under the [BSD 2-Clause License](LICENSE). Third-party components and test fixtures have their own licenses, which are retained with the distributed materials.
