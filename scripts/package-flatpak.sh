#!/usr/bin/env bash
# Build the OBS Studio Flatpak plugin extension and produce a distributable
# single-file bundle (obs-multistream-rtmp.flatpak).
#
# Requirements (installed at --user scope, no root needed):
#   flatpak install --user flathub org.flatpak.Builder org.freedesktop.Sdk//25.08
#   flatpak install --user flathub com.obsproject.Studio      # the OBS runtime
#
# The OBS Studio Flatpak runtime already ships libobs/obs-frontend-api under
# /app, so no separate OBS SDK is required.
#
# Usage:
#   bash scripts/package-flatpak.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FP_DIR="${ROOT_DIR}/packaging/flatpak"
MANIFEST="com.obsproject.Studio.Plugin.MultistreamRtmp.yaml"
APP_ID="com.obsproject.Studio.Plugin.MultistreamRtmp"
BRANCH="stable"
BUNDLE="${FP_DIR}/obs-multistream-rtmp.flatpak"

BUILDER="flatpak-builder"
if ! command -v flatpak-builder >/dev/null 2>&1; then
    # Fall back to the Flatpak-packaged builder.
    BUILDER="flatpak run org.flatpak.Builder"
fi

cd "${FP_DIR}"

# Inject public-client OAuth Client IDs from the environment into a working
# copy of the manifest. These IDs are not secrets (PKCE + state + loopback
# provide the protection); leaving them empty only disables Twitch/YouTube login.
WORK_MANIFEST="${MANIFEST}"
if [[ -n "${TWITCH_CLIENT_ID:-}${YOUTUBE_CLIENT_ID:-}" ]]; then
    WORK_MANIFEST="_generated-${MANIFEST}"
    sed -e "s|@TWITCH_CLIENT_ID@|${TWITCH_CLIENT_ID:-}|g" \
        -e "s|@YOUTUBE_CLIENT_ID@|${YOUTUBE_CLIENT_ID:-}|g" \
        "${MANIFEST}" > "${WORK_MANIFEST}"
else
    # No IDs provided: strip placeholders so the build still succeeds
    # (generic RTMP and Facebook remain functional).
    WORK_MANIFEST="_generated-${MANIFEST}"
    sed -e "s|@TWITCH_CLIENT_ID@||g" \
        -e "s|@YOUTUBE_CLIENT_ID@||g" \
        "${MANIFEST}" > "${WORK_MANIFEST}"
fi

# Build and export to a local OSTree repo.
${BUILDER} --user --force-clean --install-deps-from=flathub \
    --repo=repo build-dir "${WORK_MANIFEST}"

# Produce the distributable single-file bundle.
flatpak build-bundle --runtime repo "${BUNDLE}" "${APP_ID}" "${BRANCH}"

rm -f "${WORK_MANIFEST}"

echo "Flatpak bundle created: ${BUNDLE}"
echo "Install with: flatpak install --user ${BUNDLE}"
