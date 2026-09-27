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

## Requirements

- Ubuntu 24.04 on amd64 for the current `.deb` package.
- Docker to build the release package from source using the supplied build environment.

## Install

When a package is published, download `licasa_0.1.0-1_amd64.deb` from [Releases](https://github.com/sanxchep/licasa/releases) and install it with:

```sh
sudo apt install ./licasa_0.1.0-1_amd64.deb
```

You can then open Licasa from your application menu or run `licasa /path/to/photo.jpg`.

## Build the Ubuntu package

From the repository root, with Docker installed:

```sh
docker build -f packaging/build-deb.Dockerfile -t licasa-deb-builder packaging
mkdir -p build/deb-output
docker run --rm -v "$PWD:/source:ro" -v "$PWD/build/deb-output:/output" \
  licasa-deb-builder bash /source/packaging/verify-fresh-build.sh
```

The `.deb` and its SHA-256 file appear in `build/deb-output/`. To install the local build:

```sh
(cd build/deb-output && sha256sum -c licasa_0.1.0-1_amd64.deb.sha256)
sudo apt install ./build/deb-output/licasa_0.1.0-1_amd64.deb
```

## Update

Download the newer `.deb` and its `.sha256` file from [Releases](https://github.com/sanxchep/licasa/releases), then check and install them. Replace the filename with the published version:

```sh
sha256sum -c licasa_0.1.0-1_amd64.deb.sha256
sudo apt install ./licasa_0.1.0-1_amd64.deb
```

## Uninstall

```sh
sudo apt remove licasa
```

## License

Licasa's first-party source is licensed under the [BSD 2-Clause License](LICENSE). Third-party components and test fixtures have their own licenses, which are retained with the distributed materials.
