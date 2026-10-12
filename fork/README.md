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
