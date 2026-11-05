#!/usr/bin/env bash
set -euo pipefail

mkdir -p /work/licasa
tar -C /source --exclude='./build*' --exclude='./cmake-build-*' \
  --exclude='./.git' --exclude='./.idea' \
  --exclude='./private-validation' --exclude='./CMakeUserPresets.json' \
  -cf - . | tar -C /work/licasa -xf -
cd /work/licasa
dependency_args=()
# Only the hosted release workflow sets this after an exact, image-keyed hit.
# Application compilation, CTest and packaging always run from fresh sources.
if [ "${LICASA_REUSE_DEPENDENCIES:-false}" = true ]; then
  dependency_args+=(--skip-dependencies)
fi
python3 tools/build_ubuntu_deb.py --jobs "${LICASA_MAX_JOBS:-3}" \
  "${dependency_args[@]}" --output-dir /output
chown --reference=/source /output/licasa_*.deb /output/licasa_*.deb.sha256
