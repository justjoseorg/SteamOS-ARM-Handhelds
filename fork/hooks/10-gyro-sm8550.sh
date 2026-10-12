#!/usr/bin/env bash
# sm8550: put this repo's own builds on top of upstream's image, so the fork's
# releases carry what is in main even when upstream's release image predates it
# (gyro: kernel with the fastrpc sensors patch, qcom-gyro userspace, patched
# InputPlumber). Inputs are the "fork-assets" release assets, in $FORK_ASSETS.
set -euo pipefail
[[ "$SOC" == sm8550 ]] || exit 0
A="${FORK_ASSETS:?FORK_ASSETS not set}"
R="$ROOTFS"

tar -xzf "$A/qcom-gyro-userspace-sm8550.tar.gz" -C "$R" --no-same-owner
install -m 0755 "$A/inputplumber" "$R/usr/bin/inputplumber"

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
tar -xzf "$A/kernel.tar.gz" -C "$T"
krel="$(basename "$(ls -d "$T"/modules/*/)")"
echo "kernel $krel"
rm -rf "$R/usr/lib/modules/$krel"
cp -a "$T/modules/$krel" "$R/usr/lib/modules/$krel"

# Keep the release's own boot command line (root= and the other flags) and take
# only the kernel, its device trees and the initramfs from our build.
python3 - "$BOOT_KERNEL" "$T/boot/KERNEL" "$FORK_KERNEL" <<'PY'
import sys
old, new = (open(p, "rb").read() for p in sys.argv[1:3])
assert old[:8] == new[:8] == b"ANDROID!"
buf = bytearray(new)
buf[64:576] = old[64:576]
buf[608:1632] = old[608:1632]
open(sys.argv[3], "wb").write(buf)
print("cmdline:", bytes(buf[64:576]).split(b"\0")[0].decode())
PY
