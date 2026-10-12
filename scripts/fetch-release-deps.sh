#!/bin/bash
# Fetch every pinned third-party source the release configuration builds
# (scripts/build-release-apk.sh defaults: tawcMethods=tawcroot,
# tawcGraphics=libhybris,cpu), without building anything.
#
#   scripts/fetch-release-deps.sh [--offline]
#
# This is the F-Droid recipe's prebuild step (fdroid/me.phie.tawc.yml).
# F-Droid scans the source tree right after prebuild, so every dep has to
# be on disk by then; whatever the build clones later is never scanned.
# --offline then creates deps/.offline, after which scripts/lib/deps.sh
# refuses to clone or download anything: a dep missing from the list
# below fails the build instead of slipping past the scan.
#
# The list is by consumer. Dev-only deps (proot, debootstrap, cleat, mesa,
# gfxstream) are not part of a release build.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

OFFLINE=0
for arg in "$@"; do
    case "$arg" in
        --offline) OFFLINE=1 ;;
        *) echo "usage: fetch-release-deps.sh [--offline]" >&2; exit 2 ;;
    esac
done

cd "$REPO_DIR"
scripts/ensure-deps.sh \
    termux-app        `# settings.gradle.kts: terminal widget` \
    libhybris android-headers `# scripts/build-libhybris.sh` \
    libxkbcommon      `# scripts/build-libxkbcommon.sh` \
    smithay rutabaga_gfx `# compositor/Cargo.toml path deps (app/build.gradle.kts)` \
    openssh-portable  `# remote/sftp-server/build.sh`
# Xwayland and its 22 deps, plus the libmd tarball.
scripts/build-xwayland.sh --fetch-only

if [ "$OFFLINE" = 1 ]; then
    touch deps/.offline
    echo "==> deps/.offline set: no further dep fetches allowed"
fi
