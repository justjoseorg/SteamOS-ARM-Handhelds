# Updating

From v1.2 on, new versions install over your current system and keep your games, saves, accounts and Wi-Fi.

**Online:** Open **SteamOS Update** in Desktop Mode and hit **Check for updates**. If a newer release for your device is found, **Download and prepare** fetches it and gets it ready to install, no manual steps needed.

**Manual, from an official `.tar.gz` package:**
1. Download the update package for your chip from [Releases](https://github.com/hashtagbasit/SteamOS-ARM-Port/releases): all the `-update.tar.gz.00N` parts and the `.sha256`, into the same folder.
2. Open **SteamOS Update** in Desktop Mode and pick the `.001` part.
3. The checksum fills in from the `.sha256` (or paste it from the release page); hit **Prepare update**.

**Manual, from a full release image:** some releases only publish the split `.7z` image used for fresh installs. Pick the first `.7z` part (or an already-extracted `.img`) in **SteamOS Update** instead: it's verified against the release's `SHA256SUMS` and installed in place, same as the `.tar.gz` path, no reflash needed.

Either way, hit **Restart and install** once preparation finishes. If the update gets interrupted, it rolls back on the next boot. An update only installs on the devices it was made for, so you can't grab the wrong one by accident.

Coming from v1.1? That one has no updater yet, so flash v1.2 once.
