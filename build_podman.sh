#!/usr/bin/env bash
#
# Build the viewer inside the podman container, without having to
# remember the autobuild incantation. Thin wrapper around
# create_podman.sh (which builds/enters the container environment).
#
# Usage:
#   ./build_podman.sh                  # configure + build ReleaseFS_open, -A 64, unpackaged
#   ./build_podman.sh ReleaseFS        # build a different autobuild config
#   ./build_podman.sh ReleaseFS_open --clean   # extra autobuild switches
#   ./build_podman.sh ReleaseFS_open --package # also build the installer tarball
#
# `autobuild build` reconfigures on every invocation (fast; cmake doesn't
# force recompiles), so this one command covers both a fresh checkout and
# a rebuild. Output lands in build-linux-x86_64/newview/ in this working
# tree, same as a native build -- nothing needs to be copied out.
#
# --no-package skips the final tar.xz packaging step (viewer_manifest.py
# compressing the whole app tree), which is the slowest part of the build
# and unnecessary for local testing: the runnable app is staged unpacked
# at build-linux-x86_64/newview/packaged/firestorm regardless of this flag.
# Pass --package (after the config name) to override and build the tarball.
#
# --chan is passed explicitly (rather than left to configure_firestorm.sh's
# default) because autobuild reconfigures from scratch on every invocation,
# and each podman container gets a fresh random hostname -- so the
# "private-$(hostname)" fallback channel changes on every single build.
# Override with FS_CHANNEL=whatever, or pass --chan=whatever after the
# config name to take precedence over this default.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

CONFIG="${1:-ReleaseFS_open}"
[ $# -gt 0 ] && shift

CHANNEL="${FS_CHANNEL:-todo}"

exec ./create_podman.sh -- autobuild build -A 64 -c "$CONFIG" -- --no-package "--chan=$CHANNEL" "$@"
