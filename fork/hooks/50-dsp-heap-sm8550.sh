#!/usr/bin/env bash
set -euo pipefail
[[ "$SOC" == sm8550 && "${FORK_UPSTREAM_KERNEL:-0}" == 1 ]] || exit 0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SOURCE="$BOOT_KERNEL"
[[ ! -f "$FORK_KERNEL" ]] || SOURCE="$FORK_KERNEL"
python3 "$HERE/scripts/fix-portal-dsp-heap.py" "$SOURCE" "$FORK_KERNEL"
