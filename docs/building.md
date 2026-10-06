# Building

I build everything in an arm64 Linux VM (Colima on a Mac).

- Kernels: `external-and-mods/kernel-sm8650/build.sh` and `external-and-mods/kernel-sm8550/build.sh` (shared script in `kernel-common/`, chip specific bits in each `soc.env`)
- gamescope: `scripts/build-gamescope-in-rootfs.sh`, source in `external-and-mods/gamescope/`
- the image: `make-steamos-sm8650.sh` (`SOC=sm8550` for the 8 Gen 2 one), or `./make-steamos-sm8750.sh` for the Snapdragon 8 Elite (Odin 3); `./make-steamos-sm8350.sh` turns the rootfs from `make-steamos-sm8650.sh` (or a release image) into the REDMAGIC 6 fastboot kit (kernel: `external-and-mods/kernel-sm8350/build.sh`, see [redmagic6.md](redmagic6.md))

Valve's files and the Steam client aren't in this repo, the build downloads them. How the pieces fit together is in [HOW-IT-WORKS.md](HOW-IT-WORKS.md).

## Releasing

1. Publish the GitHub release with the `.7z` image parts and `SHA256SUMS`, same as always.
2. The **Update package** workflow (`.github/workflows/update-package.yml`) kicks in on publish. It builds the SteamOS Update package from the image with `external-and-mods/konkr-update/image-to-package.py` and attaches it to the same release as `steamos-arm-handhelds-<soc>-<tag>-update.tar.gz.001`, `.002`, … plus a `.sha256` for the joined file (GitHub caps release assets at 2 GiB, hence the parts).
3. That's what lets people update in place from **SteamOS Update** instead of reflashing. If the workflow fails or you need to redo it, run it from the Actions tab with the release tag.

To build the package by hand, run on any Linux box (needs root to loop-mount the image):

```
sudo python3 external-and-mods/konkr-update/image-to-package.py steamos-arm-handhelds-sm8550-<tag>.7z.001 \
  --soc sm8550 --version <tag> --output steamos-arm-handhelds-sm8550-<tag>-update.tar.gz
```
