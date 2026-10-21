#!/usr/bin/env bash
# Lenovo Legion Y700 Gen 5, test 2 installed from Windows (WSL) or macOS:
# puts the GPU's zap shader (and the two other qcom/kaanapali/Lenovo/ files)
# back at their exact path inside the card's firmware.tar. The old installer
# built the set in folders on the PC, where Lenovo/ and lenovo/ are one
# folder, so the GPU never started and Game Mode stayed black. No Android,
# no reinstall: the tablet unpacks the corrected set on its next start.
#
#   fix-firmware-case.sh [CARD_DRIVE]
#
# CARD_DRIVE: the card's small FAT drive (the one with firmware.tar), e.g.
# /mnt/e in WSL or /Volumes/NO\ NAME on macOS. Found by itself if left out.
set -euo pipefail
die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
command -v python3 >/dev/null || die "needs python3"
d="${1:-}"
if [[ -z "$d" ]]; then
  for c in /mnt/[a-z] /Volumes/*; do
    [[ -f "$c/firmware.tar" && -f "$c/android-boot.sha256" ]] && { d="$c"; break; }
  done
  [[ -n "$d" ]] || die "card not found: put the microSD card in the PC (on WSL it has to have a drive letter), or give its drive, e.g. $0 /mnt/e"
fi
[[ -f "$d/firmware.tar" ]] || die "no firmware.tar on $d"
echo "card: $d"
t="$(mktemp)"; trap 'rm -f "$t"' EXIT
python3 - "$d/firmware.tar" "$t" <<'PY'
import sys, tarfile
src, out = sys.argv[1:]
exact = ["qcom/kaanapali/Lenovo/baldur/gen80200_zap.mbn",
         "qcom/kaanapali/Lenovo/baldur/soccp.mbn",
         "qcom/kaanapali/Lenovo/baldur/soccp_dtb.mbn"]
by_lower = {p.lower(): p for p in exact}
fixed, have = 0, set()
with tarfile.open(src) as i, tarfile.open(out, "w") as o:
    for m in i:
        name = m.name.removeprefix("./")
        if m.isfile() and name.lower() in by_lower and name != by_lower[name.lower()]:
            m.name = "./" + by_lower[name.lower()]; fixed += 1
        if m.isfile(): have.add(m.name.removeprefix("./"))
        o.addfile(m, i.extractfile(m) if m.isfile() else None)
gone = [p for p in exact if p not in have]
if gone: sys.exit("not in firmware.tar: " + ", ".join(gone) + " (run the installer again)")
print(f"{fixed} file(s) moved to their right folder" if fixed else "already right, nothing changed")
PY
cmp -s "$t" "$d/firmware.tar" && { echo "done"; exit 0; }
cp "$t" "$d/firmware.tar.new" && sync
cmp -s "$t" "$d/firmware.tar.new" || die "the card didn't take the new file (firmware.tar is untouched)"
mv -f "$d/firmware.tar.new" "$d/firmware.tar" && sync
echo "done: eject the card, put it back in the tablet and turn it on"
