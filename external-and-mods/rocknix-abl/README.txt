ROCKNIX ABL @VERSION@ for @SOC@ (@DEVICES@)

The bootloader menu that lets this device start SteamOS from the microSD
card or internal storage, and still start Android. From the ROCKNIX project
(https://github.com/ROCKNIX/abl), GPL-2.0, (c) ROCKNIX contributors. Only
needed once; skip this if the ABL menu already shows ROCKNIX ABL @VERSION@
or newer.

You need Android's "run script as root" option (in the device's settings).
The scripts are plain dd lines, the same as ROCKNIX's own, because those
tools run a script line by line and don't show its output.

1. Copy this whole rocknix_abl folder to the top of the device's internal
   storage, so it is /sdcard/rocknix_abl (not inside Download or another
   folder). The scripts only look there.
2. Run backup_abl.sh as root. It saves your current ABL as abl_a.img and
   abl_b.img in that folder. Check that both files are there (about the
   same size as each other, not 0 bytes), copy them to a PC and keep them.
3. Run flash_abl.sh as root. It writes ROCKNIX ABL to both slots.
4. Restart holding Volume Down. In the menu (Volume to move, Power to
   select): set your device model, choose Linux as the boot mode (not
   UEFI, the image has no UEFI loader and the screen stays black), Start.
   Retroid Pocket 5 and Flip 2 each come in two panel versions: if the
   screen stays black with the plain model, try the one marked Visionox.

To go back to the ABL you had: put abl_a.img and abl_b.img back in
/sdcard/rocknix_abl and run restore_abl.sh as root.

Android keeps working with ROCKNIX ABL: choose Android in the ABL menu.
