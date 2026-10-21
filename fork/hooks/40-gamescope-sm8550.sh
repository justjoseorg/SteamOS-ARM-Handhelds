#!/usr/bin/env bash
set -euo pipefail
[[ "$SOC" == sm8550 ]] || exit 0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
A="${FORK_ASSETS:?FORK_ASSETS not set}"
T="$(mktemp -d)"
trap 'rm -rf -- "$T"' EXIT
tar -xzf "$A/gamescope.tar.gz" -C "$T" -- gamescope scripts
for path in gamescope scripts/00-gamescope/displays/ayn.odin2.portal.lua; do
    [[ -s "$T/$path" ]] || { echo "Gamescope component is missing $path" >&2; exit 1; }
done
install -D -m 0755 "$T/gamescope" "$ROOTFS/usr/local/bin/gamescope"
mkdir -p "$ROOTFS/usr/local/share/gamescope/scripts"
cp -a --no-preserve=ownership "$T/scripts/." "$ROOTFS/usr/local/share/gamescope/scripts/"
install -D -m 0755 "$HERE/steamos-overlay/usr/lib/steamos/gamescope-session" \
    "$ROOTFS/usr/lib/steamos/gamescope-session"
