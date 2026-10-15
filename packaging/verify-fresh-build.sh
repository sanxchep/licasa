#!/usr/bin/env bash
set -euo pipefail

mkdir -p /work/licasa
tar -C /source --exclude='./build*' --exclude='./cmake-build-*' \
  --exclude='./.git' --exclude='./.idea' \
  --exclude='./private-validation' --exclude='./CMakeUserPresets.json' \
  -cf - . | tar -C /work/licasa -xf -
cd /work/licasa
python3 tools/build_ubuntu_deb.py --jobs "${LICASA_MAX_JOBS:-3}" --output-dir /output
version="$(sed -nE '/^[[:space:]]*VERSION [0-9]+\.[0-9]+\.[0-9]+/s/^[[:space:]]*VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)"
python3 tools/package_appimage.py "/output/licasa_${version}-1_amd64.deb" --output-dir /output
install -m 0755 packaging/install-licasa-appimage.sh /output/install-licasa-appimage.sh
(cd /output && sha256sum install-licasa-appimage.sh > install-licasa-appimage.sh.sha256)
chown --reference=/source /output/licasa_*.deb /output/licasa_*.deb.sha256 \
  /output/Licasa-*.AppImage /output/Licasa-*.AppImage.sha256 \
  /output/install-licasa-appimage.sh /output/install-licasa-appimage.sh.sha256
