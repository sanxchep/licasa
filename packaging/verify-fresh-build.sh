#!/usr/bin/env bash
set -euo pipefail

mkdir -p /work/licasa
tar -C /source --exclude='./build*' --exclude='./cmake-build-*' \
  --exclude='./.git' --exclude='./.idea' \
  --exclude='./private-validation' --exclude='./CMakeUserPresets.json' \
  -cf - . | tar -C /work/licasa -xf -
cd /work/licasa
python3 tools/build_ubuntu_deb.py --jobs "${LICASA_MAX_JOBS:-3}" --output-dir /output
chown --reference=/source /output/licasa_*.deb /output/licasa_*.deb.sha256
