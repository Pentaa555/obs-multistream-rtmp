#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_PREFIX="${OBS_PREFIX:?Set OBS_PREFIX to a prepared OBS Studio installation prefix}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/ci-linux}"
STAGE_DIR="${STAGE_DIR:-${ROOT_DIR}/dist/stage}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"
EXPECTED_OBS_VERSION="32.2.2"

if [[ ! -d "${OBS_PREFIX}" ]]; then
    echo "OBS_PREFIX does not exist: ${OBS_PREFIX}" >&2
    exit 1
fi
if [[ ! -x "${OBS_PREFIX}/bin/obs" ]]; then
    echo "OBS executable does not exist: ${OBS_PREFIX}/bin/obs" >&2
    exit 1
fi

OBS_VERSION_OUTPUT="$("${OBS_PREFIX}/bin/obs" --version 2>&1)"
printf '%s\n' "${OBS_VERSION_OUTPUT}"
if ! grep -Eq "(^|[^0-9])${EXPECTED_OBS_VERSION//./[.]}([^0-9]|$)" <<< "${OBS_VERSION_OUTPUT}"; then
    echo "Expected OBS Studio ${EXPECTED_OBS_VERSION}; got: ${OBS_VERSION_OUTPUT}" >&2
    exit 1
fi

cmake --fresh -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DCMAKE_PREFIX_PATH="${OBS_PREFIX}" \
    -DCMAKE_INSTALL_PREFIX="${STAGE_DIR}" \
    -DOBS_PLUGIN_INSTALL_DIR=lib/obs-plugins \
    -DTWITCH_CLIENT_ID="${TWITCH_CLIENT_ID:-}" \
    -DYOUTUBE_CLIENT_ID="${YOUTUBE_CLIENT_ID:-}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure
cmake --install "${BUILD_DIR}"

PLUGIN="${STAGE_DIR}/lib/obs-plugins/obs-multistream-rtmp.so"
DATA_DIR="${STAGE_DIR}/share/obs/obs-plugins/obs-multistream-rtmp"
[[ -s "${PLUGIN}" ]]
[[ -d "${DATA_DIR}" ]]

if command -v readelf >/dev/null 2>&1; then
    readelf -d "${PLUGIN}" | grep -E 'RPATH|RUNPATH' >/dev/null
fi

if command -v ldd >/dev/null 2>&1; then
    OBS_LIB_DIRS="${OBS_PREFIX}/lib:${OBS_PREFIX}/lib/x86_64-linux-gnu"
    if LD_LIBRARY_PATH="${OBS_LIB_DIRS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" ldd "${PLUGIN}" 2>/dev/null | grep -q 'not found'; then
        echo "The staged plugin has unresolved runtime dependencies" >&2
        exit 1
    fi
fi

mkdir -p "${DIST_DIR}"
cmake --build "${BUILD_DIR}" --target package
find "${BUILD_DIR}" -maxdepth 1 -type f -name 'obs-multistream-rtmp-*.tar.gz' -exec cp {} "${DIST_DIR}/" \;
sha256sum "${DIST_DIR}"/obs-multistream-rtmp-*.tar.gz > "${DIST_DIR}/SHA256SUMS"

echo "CI validation and Linux artifact completed in ${DIST_DIR}"
