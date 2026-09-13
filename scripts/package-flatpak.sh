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

# Build and export to a local OSTree repo.
${BUILDER} --user --force-clean --install-deps-from=flathub \
    --repo=repo build-dir "${MANIFEST}"

# Produce the distributable single-file bundle.
flatpak build-bundle --runtime repo "${BUNDLE}" "${APP_ID}" "${BRANCH}"

echo "Flatpak bundle created: ${BUNDLE}"
echo "Install with: flatpak install --user ${BUNDLE}"
