#!/usr/bin/env bash
set -euo pipefail
[[ "$SOC" == sm8550 ]] || exit 0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
A="${FORK_ASSETS:?FORK_ASSETS not set}"
T="$(mktemp -d)"
trap 'rm -f "$T/msm_drv_video.so" "$T/libgbm.so.1"; rmdir "$T"' EXIT
tar -xzf "$A/vaapi.tar.gz" -C "$T" -- msm_drv_video.so libgbm.so.1
[[ -s "$T/msm_drv_video.so" && -s "$T/libgbm.so.1" ]]
VAAPI_BUILD="$T" bash "$HERE/scripts/install-v4l2-vaapi.sh" "$ROOTFS"
