#!/usr/bin/env bash
#
# Build and/or enter the Firestorm Linux build environment as a Podman
# container matching the actual GitHub Actions Linux runner exactly
# (Ubuntu 24.04.4 LTS, GCC 14) rather than the older Ubuntu 22.04/GCC 11
# setup still described in doc/building_linux.md. See podman/Containerfile
# for the package list and its sources.
#
# Usage:
#   ./create_podman.sh            # build the image, then drop into a shell
#   ./create_podman.sh --build-only
#   ./create_podman.sh -- autobuild configure -A 64 -c ReleaseFS_open
#
# For actually building the viewer without remembering autobuild's
# arguments, use ./build_podman.sh instead -- it calls this script.
#
# The repo (this checkout) and a sibling fs-build-variables checkout are
# bind-mounted into the container, so all build output -- e.g.
# build-linux-x86_64/newview/... -- lands directly in this working tree
# and is already on the host once the container exits. No copying out
# of the container is needed.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

IMAGE="firestorm-build-env:ubuntu24.04-gcc14"
REPO_DIR="$(pwd)"
VARS_DIR="${FS_BUILD_VARIABLES_DIR:-$HOME/src/fs-build-variables}"

echo "==> Building image ${IMAGE}"
podman build -t "$IMAGE" -f podman/Containerfile .

if [ "${1:-}" = "--build-only" ]; then
    exit 0
fi

if [ ! -d "$VARS_DIR" ]; then
    echo "==> Cloning fs-build-variables into $VARS_DIR"
    mkdir -p "$(dirname "$VARS_DIR")"
    git clone https://github.com/FirestormViewer/fs-build-variables.git "$VARS_DIR"
fi

if [ "${1:-}" = "--" ]; then
    shift
fi

# No --userns=keep-id here: the container runs as root (no USER in the
# Containerfile), and rootless Podman's default user namespace already
# maps container root to your host UID, so files created in the mounted
# volumes come out host-owned. --userns=keep-id is only needed when the
# container runs as a non-root UID that must match yours -- adding it
# here just forces Podman to make a slow ID-mapped copy of every image
# layer for no benefit.
tty_flags=(-i)
if [ -t 0 ] && [ -t 1 ]; then
    tty_flags=(-it)
fi

exec podman run --rm "${tty_flags[@]}" \
    -v "$REPO_DIR:/work/phoenix-firestorm" \
    -v "$VARS_DIR:/work/fs-build-variables" \
    -e AUTOBUILD_VARIABLES_FILE=/work/fs-build-variables/variables \
    -w /work/phoenix-firestorm \
    "$IMAGE" \
    "${@:-bash}"
