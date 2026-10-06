# Building

I build everything in an arm64 Linux VM (Colima on a Mac).

- Kernels: `external-and-mods/kernel-sm8650/build.sh` and `external-and-mods/kernel-sm8550/build.sh` (shared script in `kernel-common/`, chip specific bits in each `soc.env`)
- gamescope: `scripts/build-gamescope-in-rootfs.sh`, source in `external-and-mods/gamescope/`
- the image: `make-steamos-sm8650.sh` (`SOC=sm8550` for the 8 Gen 2 one), or `./make-steamos-sm8750.sh` for the Snapdragon 8 Elite (Odin 3); `./make-steamos-sm8350.sh` turns the rootfs from `make-steamos-sm8650.sh` (or a release image) into the REDMAGIC 6 fastboot kit (kernel: `external-and-mods/kernel-sm8350/build.sh`, see [redmagic6.md](redmagic6.md))

Valve's files and the Steam client aren't in this repo, the build downloads them. How the pieces fit together is in [HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## Why the image is as big as it is

The shared rootfs that every SoC starts from is Valve's **Steam Frame** (VR
headset) build, not a handheld one — it's the closest match for these
Adreno GPUs, but it ships the full SteamVR/OpenVR server, the lighthouse
base-station tracking driver, and the Deckard (headset) hardware daemons
(BLE, FPGA, charger, type-C logger, boot animation, USB-gadget/ADB). None of
that hardware exists on a handheld. `scripts/apply-overlays.sh` and
`scripts/apply-overlays-sm8750.sh` already masked the corresponding systemd
units (`ln -sfn /dev/null …`) so nothing starts, but masking a unit doesn't
delete the binary it pointed at — a "== purge Frame-only VR runtime" step
now actually removes those binaries/drivers (each removal is a no-op if the
path is absent, so it's safe across rootfs variants). Check the build log
for the `VR purge: rootfs … MiB -> … MiB` line to see how much it reclaimed
on your build.
