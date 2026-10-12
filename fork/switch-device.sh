#!/usr/bin/env bash
# One-time switch of a device running an upstream image to this fork's updates:
# trust the fork's signing key and look for releases in the fork.
#
#   sudo bash switch-device.sh [owner/repo]
#
# Later fork releases carry the same changes, so this is only needed once.
set -euo pipefail
REPO="${1:-justjoseorg/SteamOS-ARM-Handhelds}"
[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }
AGENT=/usr/lib/steamos-arm/update-agent
PUB=/usr/share/steamos-arm/update/signing.pub

ro=0
if command -v steamos-readonly >/dev/null && steamos-readonly status 2>/dev/null | grep -qi enabled; then
  steamos-readonly disable; ro=1
fi
trap '[[ $ro -eq 1 ]] && steamos-readonly enable' EXIT

cp -a "$PUB" "$PUB.upstream" 2>/dev/null || true
cat > "$PUB" <<'KEY'
-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAfbutDFp9G/UiUvKRAJSWTYpza661UNxrquy0w4vnLdE=
-----END PUBLIC KEY-----
KEY
chmod 644 "$PUB"
sed -i "s|^REPO = .*|REPO = \"$REPO\"|" "$AGENT"
grep -qx "REPO = \"$REPO\"" "$AGENT"
echo "Updates now come from $REPO (key backup: $PUB.upstream)"
