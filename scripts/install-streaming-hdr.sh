#!/usr/bin/env bash
# Install a separately built x86 SDL bundle without replacing Steam's libraries.
set -euo pipefail
R="${1:?usage: install-streaming-hdr.sh <rootfs>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="${STREAMING_HDR_BUILD:-${STEAMOS_WORK:-/work}/streaming-hdr-build}"
if [[ -z "${STREAMING_HDR_BUILD:-}" && ! -d "$BUNDLE" ]]; then
    echo "HDR: no SDL bundle at $BUNDLE; skipping HDR client installation" >&2
    exit 0
fi
python3 - "$BUNDLE" <<'PY'
import hashlib, json, re, sys
from pathlib import Path
root = Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text())
if not isinstance(manifest, dict):
    raise ValueError("Invalid SDL manifest: expected an object")
for key in ("steam_sdl_sha256", "libSDL3_sha256"):
    value = manifest.get(key)
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{64}", value):
        raise ValueError(f"Invalid {key}")
library = (root / "libSDL3.so.0").read_bytes()
if library[:6] != b"\x7fELF\x02\x01" or library[18:20] != b"\x3e\x00":
    raise ValueError("Invalid SDL architecture")
if hashlib.sha256(library).hexdigest() != manifest["libSDL3_sha256"]:
    raise ValueError("SDL hash mismatch")
if not (root / "LICENSE.txt").stat().st_size:
    raise ValueError("Missing SDL license")
PY
DEST="$R/usr/lib/steamos/streaming-hdr"
install -D -m755 "$BUNDLE/libSDL3.so.0" "$DEST/libSDL3.so.0"
install -m644 "$BUNDLE/manifest.json" "$BUNDLE/LICENSE.txt" "$DEST/"
for name in sm8550-steam-av1 sm8550-streaming-client; do
    install -D -m755 "$ROOT/steamos-overlay/usr/lib/steamos/$name" "$R/usr/lib/steamos/$name"
done
