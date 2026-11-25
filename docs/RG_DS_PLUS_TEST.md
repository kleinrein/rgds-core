# Testing RG DS OS on an RG DS Plus

RG DS OS ships one card image for both handhelds. The board's ID voltage (SARADC channel 5) tells them apart: about
1.06 V on the RG DS, 1.8 V on the Plus. Linux reads it reliably, U-Boot does not: before Linux the Plus's ID divider
is unpowered, and one Plus read 0.27, 0.33, 1.03 and 1.57 V in U-Boot on four boots. So Linux records the handheld in
`rgds-board.txt` on the boot partition (`usr/libexec/rgds/board-check`), and U-Boot loads the device tree that file
names. Only a card's first boot, before the file exists, goes by U-Boot's own reading (0.8 to 1.4 V means the RG DS,
anything else the Plus); when that guess is wrong, Linux writes the file and restarts once, so a first start may show
the boot twice. A card moved between an RG DS and a Plus does the same. The first Plus ran RG DS OS on 2026-11-24. The
design is in [OWN_OS_PLAN.md](OWN_OS_PLAN.md) ("One card for both handhelds").

## What to flash

Each release has two card images:

| Image | When |
| --- | --- |
| `RG-DS-OS-<version>.img.gz` | Try this first. It works on both handhelds. |
| `RG-DS-OS-Plus-<version>.img.gz` | Use this if the first one shows nothing on the screens. It always loads the Plus's device tree. |

Write the image to a spare microSD card (8 GB or more) with balenaEtcher or Raspberry Pi Imager (not Rufus: its cards stay black). Put the card
in the **first** card slot and power on. The Plus has two slots, and the handheld only boots from the first one. If
nothing happens, try the other slot.

Nothing on the handheld is changed: its built-in storage is never touched. To go back, power off and put the stock
card back in.

## What to check

Spend a few minutes on each item and note what you see.

1. **Screens.** Do both screens light up with the RG DS Core menu? Is the picture sharp and steady, with no
   flicker, rolling or tearing?
2. **Touch.** Tap and drag on the bottom screen. Does the pointer land where your finger is?
3. **Buttons.** D-pad, A/B/X/Y, L/R, L2/R2, Start/Select, both sticks, the volume keys, and the key under the screens
   (it should act as Home).
4. **Sound.** Do the menu sounds play from the speakers? Do the volume keys change the volume, all the way down to
   silent? Plug in headphones: does the sound move to them and the speakers go quiet? Unplug them: does it come back
   to the speakers? (A game that is already running keeps its sound where it started; check this in the menu.)
5. **A game.** Play any DS game for 5 minutes. Is the sound clear, without crackling or a wrong pitch? Does the
   in-game menu (the key under the screens) open?
6. **Microphone.** Settings > Microphone: does the level move when you blow on it?
7. **Sleep.** Close the lid for a minute, then open it. Does the game or menu carry on? Does the red LED light while
   it sleeps?
8. **Vibration.** Is there a short buzz on menu taps?
9. **Wi-Fi.** Settings > Connectivity: does it join your network?
10. **Battery.** Does the battery level look right, and does the amber LED blink while charging?

## Sending the results

After the handheld has been on for 2 minutes (with headphones plugged in and unplugged at least once), power it off.
Put the card in a computer and send these files from the card's large partition:

- `rgds/logs/hardware.txt`: what the handheld detected: the board, its ID voltage, the sound cards and the panel modes
- `rgds/logs/dmesg.txt` and `rgds/logs/boot.txt`
- `rgds/logs/launcher.log`

If the screens stay black, the first file to look at is `hardware.txt`. The `cmdline:` line ends in
`rgds.detected=… rgds.adc=… rgds.from=…`: the board U-Boot picked, the voltage it read (in microvolts) and whether it
went by `rgds-board.txt` (`file`) or that voltage (`adc`). `board-check:` lines in the same file say what Linux read
and whether it restarted.

## For the developer: what each result points to

| Symptom | Likely cause | Where |
| --- | --- | --- |
| Black or white screens with the universal image, working with the Plus image | The wrong device tree (`rgds.detected` and `rgds.from` in `hardware.txt`); `board-check` should have restarted once | `usr/libexec/rgds/board-check`, `uboot-rgds.fragment` preboot |
| Black screens with both images | Panel timing | `dts-patches/0006` (59.9986 Hz); drop it to try ROCKNIX's 60.17 Hz |
| No sound from the speakers | The AW88166 card (`aw88166` in the card list), its firmware (`aw88166_acf.bin` in dmesg), or the route | `package/rocknix-extra-firmware`, `usr/libexec/rgds/audio` |
| Volume keys do nothing on the speakers | The amplifiers' control names (`Left PCM`/`Right PCM` or `PCM`) | `runtime/audio.py` `Amplifier` |
| Sound pitched up or crackling | The 48 kHz stream did not stick | `usr/libexec/rgds/audio` (dmix rate) |
| Home does nothing | The key's code | `dts-patches/0008` |
