# Fork layout

This fork follows upstream (hashtagbasit/SteamOS-ARM-Port) and publishes its own
signed `.sau` update packages, so a device only ever updates from this fork's
releases and always gets the newest upstream release plus the changes in `fork/`.

```
upstream release ──► fork-sau.yml ──► image + fork/ changes ──► signed .sau ──► this repo's Releases
upstream main    ──► fork-sync.yml ──► merged into main
```

## What gets applied to the upstream image (`scripts/fork-build-sau.sh`)

- `signing.pub` replaces the device's trusted update key, and `update-agent`'s `REPO` points here.
- `overlay/` is copied over the root filesystem (paths as on the device). It currently turns **SSH on by default**
  (`sshd.service` enabled; host keys generated on first start). Password login for `steamos` works once you've run `passwd`.
- `patches/*.patch` are applied with `patch -p1` from the rootfs root.
- `hooks/*.sh` run last as root with `ROOTFS`, `SOC` and `VERSION` set.

Only `/usr`, `/opt`, `/etc` (minus accounts, `ssh`, etc.), the kernel and the two Decky plugins are shipped by updates,
so changes must land there. Anything under `/etc/ssh` is deliberately preserved by the updater, hence SSH is enabled via `/usr`.

## Portal HDR streaming

The SM8550 update includes a patched Gamescope WSI layer and a separate x86 SDL
for the Portal's existing `.pyrowave-box64` streaming opt-in. Native Steam's SDL
is not replaced. At each stream launch, the helper checks the board, the stock
Steam SDL fingerprint, the packaged SDL checksum and an AV1 P010 VA-API config.
An incompatible Steam update keeps the stock SDL and reports the reason in
`streaming_client_box64.log`. Create `.no-hdr-box64` in the Steam directory to
disable this override.

The SDL build uses the Portal profile's factory peak and Gamescope's current SDR
white level, only while HDR is active on the internal display. Host HDR currently
needs **Custom / enabled**; **Match client** did not negotiate ten-bit video in
the tested setup. This does not add a Spout toggle or 4:4:4 support.

## Versions

Release `vX.Y.Z.N` is upstream `vX.Y.Z` revision `N`. A new upstream release starts at `.1`; run
**Fork update packages** manually with *rebuild* ticked after changing `fork/` to publish `.N+1`.

## Setup (once)

1. Repo secret `SAU_SIGNING_KEY`: the private key matching `signing.pub`
   (`~/.config/steamos-arm-release/update-signing.pem` on the machine that generated it).
2. Enable Actions on the fork (scheduled workflows start disabled on forks).
3. Optional secret `SYNC_TOKEN` (PAT with `repo` + `workflow`) so syncing can pull upstream workflow changes.
4. On each device, once, as root: `bash fork/switch-device.sh` (it trusts the fork's key and points the updater here).
   After that, **SteamOS Update** finds the fork's releases.

Keep the private key secret; losing it means re-running step 4 with a new key on every device.
