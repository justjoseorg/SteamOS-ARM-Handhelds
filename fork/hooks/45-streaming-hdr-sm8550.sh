#!/usr/bin/env bash
set -euo pipefail
[[ "$SOC" == sm8550 ]] || exit 0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
A="${FORK_ASSETS:?FORK_ASSETS not set}"
T="$(mktemp -d)"
trap 'rm -f "$T/libSDL3.so.0" "$T/manifest.json" "$T/LICENSE.txt"; rmdir "$T"' EXIT
tar -xzf "$A/streaming-hdr.tar.gz" -C "$T" -- libSDL3.so.0 manifest.json LICENSE.txt
python3 - "$T" <<'PY'
import hashlib, json, re, sys
from pathlib import Path
root = Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text())
for key in ("steam_sdl_sha256", "libSDL3_sha256"):
    assert re.fullmatch("[0-9a-f]{64}", manifest[key]), f"Invalid {key}"
library = (root / "libSDL3.so.0").read_bytes()
assert library[:6] == b"\x7fELF\x02\x01" and library[18:20] == b"\x3e\x00", "Invalid SDL architecture"
assert hashlib.sha256(library).hexdigest() == manifest["libSDL3_sha256"], "SDL hash mismatch"
assert (root / "LICENSE.txt").stat().st_size, "Missing SDL license"
PY
DEST="$ROOTFS/usr/lib/steamos/streaming-hdr"
install -D -m755 "$T/libSDL3.so.0" "$DEST/libSDL3.so.0"
install -m644 "$T/manifest.json" "$T/LICENSE.txt" "$DEST/"
for name in sm8550-steam-av1 sm8550-streaming-client; do
    install -D -m755 "$HERE/steamos-overlay/usr/lib/steamos/$name" "$ROOTFS/usr/lib/steamos/$name"
done
install -D -m755 "$HERE/steamos-overlay/usr/share/deckard/RUNSTEAM.sh" \
    "$ROOTFS/usr/share/deckard/RUNSTEAM.sh"
