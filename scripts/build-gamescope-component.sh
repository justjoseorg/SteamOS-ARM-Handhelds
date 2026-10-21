#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/external-and-mods/gamescope"
OUT="$(realpath -m "${OUT:?output directory required}")"
WORK="$(mktemp -d)"
# Pinned dependency sources for the vendored wlroots 0.20 API.
UPSTREAM_REF=aa289ea5977d8b21b92b0f246f0e8f0c225d180f

git init -q "$WORK/upstream"
git -C "$WORK/upstream" fetch -q --depth 1 https://github.com/ValveSoftware/gamescope.git "$UPSTREAM_REF"
git -C "$WORK/upstream" checkout -q FETCH_HEAD
git -C "$WORK/upstream" submodule update -q --init --depth 1 \
    subprojects/wlroots subprojects/libliftoff subprojects/vkroots subprojects/libdisplay-info
mkdir "$WORK/src"
rsync -a --exclude=.git --exclude=subprojects/ "$SRC/" "$WORK/src/"
rsync -a --exclude=.git "$WORK/upstream/subprojects/" "$WORK/src/subprojects/"
cp "$WORK/src/subprojects/wlroots/subprojects/"*.wrap "$WORK/src/subprojects/"

luajit "$WORK/src/tests/ayn.odin2.portal.lua" \
    "$WORK/src/scripts/00-gamescope/displays/ayn.odin2.portal.lua"
meson setup "$WORK/build" "$WORK/src" \
    --prefix=/usr/local --buildtype=release \
    -Dpipewire=enabled -Dinput_emulation=enabled -Dbenchmark=disabled \
    -Denable_openvr_support=false -Denable_gamescope_wsi_layer=false \
    --force-fallback-for=wlroots,libliftoff,vkroots,wayland,wayland-protocols,libdrm,libxkbcommon,pixman
meson compile -C "$WORK/build" -j 2
meson test -C "$WORK/build" --print-errorlogs
meson install -C "$WORK/build" --no-rebuild --destdir "$WORK/install"

install -Dm755 "$WORK/install/usr/local/bin/gamescope" "$OUT/gamescope"
cp -a "$WORK/install/usr/local/share/gamescope/scripts" "$OUT/scripts"
