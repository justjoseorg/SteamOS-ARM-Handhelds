#!/usr/bin/env bash
# Build signed .sau update packages for this fork from an upstream release image.
#
#   sudo fork-build-sau.sh <first .7z part> <soc> <version> <signing key.pem> <outdir>
#
# The image is extracted and mounted read-only; fork/overlay, fork/patches and
# fork/hooks are applied on top through an overlayfs, so nothing upstream ships
# is edited in the repo. The fork's own signing key and update source replace
# the upstream ones. Needs root, 7z, patch, openssl and ~25 GB of scratch space.
set -euo pipefail

SRC="${1:?first .7z part}"; SOC="${2:?soc}"; VERSION="${3:?version}"
KEY="${4:?signing key}"; OUT="${5:?output dir}"
REPO="${FORK_REPO:-${GITHUB_REPOSITORY:?set FORK_REPO=owner/name}}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FORK="$HERE/fork"
[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }

SRC="$(readlink -f "$SRC")"; OUT="$(mkdir -p "$OUT" && readlink -f "$OUT")"
W="$(mktemp -d "${WORKDIR:-$(dirname "$SRC")}/.fork-sau-XXXXXX")"
LOOP=""
cleanup() {
  set +e
  for m in "$W/rootfs/home/steamos" "$W/rootfs" "$W/home" "$W/boot" "$W/storage"; do
    mountpoint -q "$m" && umount "$m"
  done
  [[ -n "$LOOP" ]] && losetup -d "$LOOP"
  rm -rf "$W"
}
trap cleanup EXIT

echo "== extracting $(basename "$SRC")"
7z x -o"$W" -y "$SRC" >/dev/null
IMG=("$W"/*.img)
[[ ${#IMG[@]} -eq 1 ]] || { echo "expected one .img in the archive" >&2; exit 1; }

LOOP="$(losetup --find --show --partscan --read-only "${IMG[0]}")"
partition() {                       # label, fallback index
  local dev names
  for dev in "${LOOP}"p*; do
    names="$(lsblk -no PARTLABEL,LABEL "$dev" | tr '[:lower:]' '[:upper:]' | tr -s ' \n' '\n\n')"
    if grep -qx "$1" <<<"$names"; then echo "$dev"; return; fi
  done
  echo "${LOOP}p$2"
}
mkdir -p "$W"/{storage,boot,home,upper,ow,rootfs}
mount -o ro "$(partition STORAGE 2)" "$W/storage"
mount -o ro "$(partition ROCKNIX 1)" "$W/boot"
mount -o ro "$(partition HOME 3)" "$W/home"
[[ -f "$W/boot/KERNEL" ]] || { echo "image has no boot KERNEL" >&2; exit 1; }

mount -t overlay overlay -o "lowerdir=$W/storage,upperdir=$W/upper,workdir=$W/ow" "$W/rootfs"
R="$W/rootfs"
mkdir -p "$R/home/steamos"
mount --bind "$W/home/steamos" "$R/home/steamos"

echo "== applying fork changes"
# The device checks updates against its own copy of the key, and update-agent
# looks for releases in REPO.
install -D -m 0644 "$FORK/signing.pub" "$R/usr/share/steamos-arm/update/signing.pub"
AGENT="$R/usr/lib/steamos-arm/update-agent"
sed -i "s|^REPO = .*|REPO = \"$REPO\"|" "$AGENT"
grep -qx "REPO = \"$REPO\"" "$AGENT" || { echo "could not point update-agent at $REPO" >&2; exit 1; }
if [[ -d "$FORK/overlay" ]]; then
  cp -a --no-preserve=ownership "$FORK/overlay/." "$R/"
fi
shopt -s nullglob
for p in "$FORK"/patches/*.patch; do
  echo "patch $(basename "$p")"
  patch -p1 -d "$R" --no-backup-if-mismatch <"$p"
done
for h in "$FORK"/hooks/*.sh; do
  echo "hook $(basename "$h")"
  ROOTFS="$R" SOC="$SOC" VERSION="$VERSION" bash "$h"
done

echo "== building $SOC $VERSION"
mkdir -p "$OUT"
python3 "$HERE/scripts/build-update-v2.py" --rootfs "$R" --kernel "$W/boot/KERNEL" --soc "$SOC" \
  --version "$VERSION" --sign-key "$KEY" --output "$OUT/steamos-arm-$SOC-$VERSION.sau"
