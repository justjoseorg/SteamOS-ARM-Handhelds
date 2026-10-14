#!/usr/bin/env bash
# sm8550: the PMIC haptic motor. Upstream's image has no qcom-hv-haptics source
# in the Odin 2 composite device and nothing sets the motor's gain, so ship
# both from this repo's sm8550-overlay (the kernel driver and the InputPlumber
# rumble patch come from the fork-assets build in 10-gyro-sm8550.sh).
set -euo pipefail
[[ "$SOC" == sm8550 ]] || exit 0
R="$ROOTFS"
O="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/sm8550-overlay"
Y=etc/inputplumber/devices.d/02-ayn-odin2-mini.yaml

grep -q qcom-hv-haptics "$O/$Y"
install -D -m 0644 "$O/$Y" "$R/$Y"
install -D -m 0644 "$O/$Y" "$R/var/lib/overlays/etc/upper/inputplumber/devices.d/02-ayn-odin2-mini.yaml"
install -D -m 0755 "$O/usr/libexec/qcom-haptics-gain" "$R/usr/libexec/qcom-haptics-gain"
install -D -m 0644 "$O/usr/lib/systemd/system/qcom-haptics-gain.service" "$R/usr/lib/systemd/system/qcom-haptics-gain.service"
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
ln -sf ../qcom-haptics-gain.service "$R/usr/lib/systemd/system/multi-user.target.wants/qcom-haptics-gain.service"
