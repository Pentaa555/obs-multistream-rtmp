#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_PREFIX="${OBS_PREFIX:?Set OBS_PREFIX to the validated OBS Studio 32.2.2 prefix}"
YOUTUBE_CLIENT_ID="${YOUTUBE_CLIENT_ID:?Set YOUTUBE_CLIENT_ID for the release build}"
YOUTUBE_CLIENT_SECRET="${YOUTUBE_CLIENT_SECRET:?Set YOUTUBE_CLIENT_SECRET for the public Desktop-client build}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build/deb-linux}"
DIST_DIR="${DIST_DIR:-${ROOT_DIR}/dist}"

if [[ ! -x "${OBS_PREFIX}/bin/obs" ]]; then
    echo "OBS executable does not exist: ${OBS_PREFIX}/bin/obs" >&2
    exit 1
fi

OBS_VERSION_OUTPUT="$(${OBS_PREFIX}/bin/obs --version 2>&1)"
printf '%s\n' "${OBS_VERSION_OUTPUT}"
grep -Eq '(^|[^0-9])32[.]2[.]2([^0-9]|$)' <<< "${OBS_VERSION_OUTPUT}"

cmake --fresh -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DCMAKE_PREFIX_PATH="${OBS_PREFIX}" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
    -DOBS_PLUGIN_INSTALL_DIR=lib/x86_64-linux-gnu/obs-plugins \
    -DTWITCH_CLIENT_ID="${TWITCH_CLIENT_ID:-}" \
    -DYOUTUBE_CLIENT_ID="${YOUTUBE_CLIENT_ID}"

cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure

rm -f "${BUILD_DIR}"/*.deb "${BUILD_DIR}"/*.tar.gz
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G DEB

mkdir -p "${DIST_DIR}"
find "${BUILD_DIR}" -maxdepth 1 -type f -name '*.deb' -exec cp {} "${DIST_DIR}/" \;

PACKAGE="$(find "${DIST_DIR}" -maxdepth 1 -type f -name '*.deb' -printf '%T@ %p\n' | sort -nr | head -1 | cut -d' ' -f2-)"
[[ -n "${PACKAGE}" && -s "${PACKAGE}" ]]

echo "=== package metadata ==="
dpkg-deb --info "${PACKAGE}"
echo "=== package contents ==="
dpkg-deb --contents "${PACKAGE}"

if dpkg-deb --contents "${PACKAGE}" | grep -E '(^|/)(build|CMakeCache.txt|build.ninja|\.git)/' >/dev/null; then
    echo "Build files leaked into the Debian package" >&2
    exit 1
fi

echo "Debian artifact: ${PACKAGE}"
sha256sum "${PACKAGE}" > "${PACKAGE}.sha256"
printf 'SHA256SUMS for Debian artifact:\n'
cat "${PACKAGE}.sha256"
