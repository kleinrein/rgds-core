# Own OS for the RG DS: brief and loose plan

Status: proposal, 2026-10-26, started the same day (see Progress below). Written for the developer who takes this on.

## Goal

Boot RG DS Core from its own Linux image on a microSD card: fast boot, read-only system, A/B update slots, full control of the display, power and audio path. Stock firmware stays untouched on the other card so the handheld can never be bricked by this work.

Non-goals for the first milestone: PortMaster, RetroAchievements, an app store, other languages. Get a stable boot to the RG DS Core menu and one DS game running on both screens first.

## What we already know

The Armbian attempt (October 2026, retired; its write-up is gone, the raw logs are in `build/os/r3-hardware-failure-20261010` and `build/os/r4-hardware-failure-20261010`, kernel reference in `build/os/kernel-reference`) got a long way:

- Armbian edge kernel 7.1.8 (mainline `rockchip64`) booted on the handheld.
- Both DSI panels and the VOP bound; SDL saw two 640x480 displays; Panfrost initialised the Mali-G52.
- All inputs enumerated: gpio-keys, ADC joystick, ADC home/play keys, RK805 power key, both Goodix touchscreens.
- Weston plus the RG DS Core session started and drew the UI.

What broke, per the owner: the UI shows for about a second, then any touch or click makes the screens go blank or flash, and only a hard power-off recovers. Wi-Fi (`rtw88_8821cs` over SDIO) also logged `-110` timeouts at boot.

The captured logs end before the failure. Hard power-off loses the tail of the log, so **we have no evidence of the actual fault yet.** Step one is to get some.

Another project, [BananaLinux](https://github.com/JorreFog/BananaLinux) (MIT, Buildroot, 64 stars), already runs on exactly this hardware. Its README says: Buildroot, a ROCKNIX-derived kernel with RG DS device trees, drivers and U-Boot kept in `buildroot/`, **no compositor** (both screens driven directly), and the panel timings retuned in the device tree from 60.10 Hz to 60.0013 Hz. That last point is a clue (see hypotheses).

## Hypotheses for the blank/flashing screens

Ranked by how well they fit "fine for a second, breaks on first input". All unproven.

1. **Panel clocks / shared PLL.** Two DSI panels on one VOP need pixel clocks derived from limited PLLs. If the mainline device tree gives clocks that cannot both be met cleanly, one pipe underruns or loses sync as soon as something forces a new atomic commit (a touch-triggered redraw). BananaLinux retuning the panel rate in the DT points at this area.
2. **Compositor on dual DSI.** Weston's atomic commits to two outputs on one VOP2 (plane or port assignment, vblank timeouts, `flip_done` timeouts). The stock-card build already presents to KMS directly (see the direct present notes in `docs/RGDS_VULKAN.md` and `docs/PERFORMANCE.md`), so a compositor-less path is known to work in our code.
3. **GPU hang or fault under Panfrost** on the first repaint under load (`panfrost ... gpu sched timeout`, MMU faults). The "Failed to obtain a high priority context" line in `weston.log` shows the GPU stack was not fully happy.
4. **Power rail dip.** GPU or CPU DVFS step on load, PMIC (RK817/RK805, FAN53555) voltage not matching the mainline OPP table, causing a brown-out of the panel or backlight. Flashing screens plus a hard hang fit this too.
5. **Touch interrupt/I2C storm** from two Goodix controllers wedging the bus. Least likely, but cheap to rule out by running without touch.

## Ground rules

- **Never touch the stock card or its partitions.** Develop on a second microSD card. The handheld boots from the slot you put the card in; swapping back must always restore stock.
- Keep a known-good recovery image on every card: a minimal image that boots, shows text on both screens and exposes a shell over USB or serial.
- **Tell the owner before each hardware test**; a hard power-off is the expected failure mode, so every test must be safe against one (read-only rootfs, journaled data partition).
- Reuse licensed work properly. BananaLinux is MIT; ROCKNIX and the kernel are GPL. Keep a `THIRD_PARTY.md`, keep copyright headers, and ask the BananaLinux maintainer before lifting code wholesale. Contacting them is probably the fastest route anyway.
- No Nintendo BIOS/firmware or game files in the image (see the PictoChat and BIOS notes).

## Progress, 2026-10-26

Offline Phase 0 work, plus the first Phase 1 card. No hardware test has run yet.

- **The kept Armbian logs do not contain the failure.** The r4 `weston.log` ends in `caught signal 15` and `syslog` in an
  orderly systemd shutdown, about 20 s after boot. Both outputs came up at `640x480@60.0, 42.1 MHz`. There is no
  `flip_done`, underflow or Panfrost fault line anywhere. The failure layer still needs a hardware run.
- **Mainline device tree vs Banana's** (`.cache/own-os/dts/mainline-vs-banana.diff`; mainline is identical in 7.1.8 and
  7.2.7):
  - Display clocks. Mainline parents both `DCLK_VOP0` and `DCLK_VOP1` to `PLL_VPLL` at 292.5 MHz, so both panels get
    292.5/7 = 41.79 MHz (59.6 Hz). Banana puts VOP0 on `PLL_HPLL` and VOP1 on `PLL_VPLL`, both at 126.4 MHz, one PLL
    per panel, and retunes the porches to 60.0013 Hz. `dclk_vop*` has no `CLK_SET_RATE_PARENT` in `clk-rk3568.c`, so
    one port cannot re-rate the shared PLL under the other. That weakens hypothesis 1 as a commit-time fault but does
    not rule it out.
  - Panels. Mainline uses `jadard,jd9365da-h3`; Banana uses ROCKNIX's `rocknix,generic-dsi` with the init sequence in
    the DT. The DSI mode flags are the same (video, burst, non-continuous clock, LPM).
  - Input. Mainline uses gpio-keys, adc-joystick and gpio-mux; Banana uses ROCKNIX's single-ADC joypad driver. The
    Goodix `panel` property in mainline is unused (the Goodix driver has no panel-follower support).
  - Banana also carries `fan53555: accept the STI8070A` (vdd_cpu on the RG DS Plus) and a pending VOP2 patch that
    starts the second video port on the first one's frame start.
- **Open questions answered:**
  1. Banana runs gregkh **v7.2.7** (`Linux version 7.2.7-banana`) plus ROCKNIX-derived patches, its own DTS, ROCKNIX's
     generic DSI panel and joypad drivers, U-Boot v2026.01 (`quartz64-a-rk3566` defconfig, SPL boot order `sdmmc0`
     then `sdhci`), BL31 v1.45 and DDR v1.23. It is not a vendor BSP.
  2. The RG DS Plus has its own DTS and panel tuning (`rk3568-anbernic-rg-ds-plus.dts`).
  4. Banana's layout is already what Phase 4 wants: U-Boot at 32K, FAT boot partition (`a/` and `b/` kernels with
     `extlinux.conf` picking one), two 256 MiB squashfs root slots, and an exFAT data partition created on first boot.
  5. The owner's unit has **one card slot** (`sdmmc0`, `fe2b0000`), and the stock Linux system runs entirely from
     that card. It **also has a 29.1 GiB eMMC** (`R21BMB`, HS200, 15 partitions, presumably the factory Android). The
     stock kernel never probes it; Banana's kernel shows it as `mmcblk0`. Our image must never write to `mmcblk0`.
     The SD card still wins the boot when present, so testing means powering off and swapping the stock card for
     the test card, and swapping back restores stock.
- **The Phase 1 card.** BananaLinux v0.4.2 `banana-rgds.img.gz` (SHA-256 `a885c8c6…cb0ec`, checked against the
  release's sums) is in `.cache/own-os/banana-0.4.2/`. `tools/own_os_card.py` lists disks, flashes a spare card (it
  refuses internal disks, disks over 256 GB and stock-looking cards unless `--erase-stock-looking` is passed) and
  copies `banana/logs` back into `build/os/`.

### First hardware test: BananaLinux on the owner's RG DS

This is the cheapest decisive test. If Banana's kernel and DT run both screens with touch, the decision gate after
Phase 0 already points to Banana as the base.

1. Power the handheld off. Take the stock card out and put it aside. Put the Banana card in. Power on.
2. Follow Banana's `docs/FIRST-BOOT.md` checks: boot animation on both panels, setup screens, then the menu. Then
   **tap and drag on the bottom screen for a minute** (this is the Armbian failure trigger), buttons, sound, and lid
   close/open.
3. Hold power for 2 s to shut down (or hard power-off if it hangs). Put the card in the Mac, then run
   `tools/own_os_card.py logs --label first-boot`.
4. Swap the stock card back in.

**Result, 2026-10-26:** the owner flashed the card (disk4 in the Mac's reader) and booted BananaLinux v0.4.2 on their
RG DS. Both screens, touch, buttons and sound all worked, with no blanking under touch. The fault the Armbian card
showed is therefore not in this unit's hardware. It sits in the mainline DT/driver stack or in the Weston path, both of
which Banana avoids. Per the decision gate, Banana's kernel, DT and U-Boot become the base for Phase 1.

The logs are in `build/os/banana-first-boot-20261026-204916/`:

- **Display: clean.** There are no `flip_done`, underflow, VOP or Panfrost errors in 122 s, which included a menu
  session, a store app run and a lid sleep. The panel tuner measured 42133334 Hz on both panels, which is 60.0013 Hz,
  as the DT says.
- **Boot to menu in 4.54 s**: init at 0.81 s, panels open at 2.14 s. The first boot also created the data partition.
- **Wi-Fi:** the same `rtw88_8821cs ... sdio read32 failed (0xf0): -110` lines as on Armbian, but the firmware loads
  right after. This is a probe-time hiccup, not a dead chip. Wi-Fi itself was not tried. `regulatory.db` is missing.
- **Sleep:** the first suspend failed (`xhci-hcd: Clearing Run/Stop bit failed -110`). Banana's fallback retried and
  the second suspend worked: 4 s asleep, the battery gauge carried over, and USB was re-initialised on resume. This
  goes into Phase 3.
- **RTC:** it read 2026-11-05 on a 2026-10-26 boot, so the clock is 10 days fast and there was no network time to fix
  it. Phase 3 needs time sync or a sane RTC write.

### RG DS Core on Banana's kernel (probe, 2026-10-26)

`os/banana-probe/` runs our launcher on a BananaLinux card with no compositor. `hold.sh` borrows the panels from
Banana's menu, in RAM only. `run.sh` starts the launcher with SDL's KMSDRM driver. `trace.py` turns the launcher's
UiTrace marks into per-stage frame timings. `press.py` injects button presses into the joypad's evdev node.

- **Both panels and touch work** through KMSDRM and GLES2 on Panfrost, with no Weston.
- **"Incredibly slow" had two causes.** `SDL_RENDER_DRIVER=opengles2` without `SDL_RENDER_BATCHING=1` turns SDL2's
  batching off, so a panel took 35-170 ms to draw. And Banana's bananad keeps the menu at a powersave GPU (200 MHz)
  and a 1.4 GHz CPU cap. With batching and full clocks the median frame is about 6-11 ms. The stock session already
  sets batching; the probe had missed it. Our own OS sets its own governors.
- **Buttons.** Banana's kernel uses ROCKNIX's `retrogame_joypad`, which has BTN_MODE (the F key) at SDL index 10.
  `MainlineControls::from_joypad` translates that layout. This was checked with injected Right, Down and A presses.
- **Crash fixed in `menu_audio.hpp`.** `reap()` ran `finish_open()` before clearing `job`. When the DAC level
  cannot be restored (Banana has no `DAC` control), the sink closes inside that hook and the thread was joined
  twice, which aborted with `thread::join failed`. Stock could hit the same thing whenever the DAC write fails.
- **Still open:** the audio mixer controls (`amixer DAC` missing), Wi-Fi status inside RG DS Core, emulator input
  through the bridge (`emulator_bridge.cpp` still assumes gpio-keys numbering), and `Could not restore CRTC` on exit.

### RG DS OS image v0 (2026-10-26)

`os/buildroot/` is our Buildroot external tree (`BR2_EXTERNAL_RGDS`). `tools/own_os_build.py` stages RG DS Core
and builds it in the Lima VM.

- **Board layer.** BananaLinux's, copied in with attribution (`THIRD_PARTY.md`): kernel 7.2.7 with ROCKNIX's
  patches, the DSI panel and joypad drivers, the 60 Hz device tree, U-Boot v2026.01 and BL31. The toolchain is
  Buildroot's own (GCC 14, glibc, 6.18 headers). Panfrost is built into the kernel, so RG DS Core can draw from its
  first frame.
- **System.** BusyBox init. `rcS` mounts the card, makes the exFAT data partition on first boot (`RGDS`, partition
  4), pins the GPU's devfreq to performance, and starts Wi-Fi (unless `rgds/wifi-off`) and, when
  `rgds/ssh/authorized_keys` is on the card, SSH. init then runs `/usr/libexec/rgds/session`, which starts RG DS Core on
  KMSDRM with batching. No compositor, no udev (libudev-zero), no Banana userspace.
- **Card layout.** U-Boot at 32K, FAT `RGDS-BOOT` (`a/` and `b/` kernels, `extlinux.conf`), `rgds-root-a` and
  `rgds-root-b` (768 MiB squashfs each), and the `RGDS` data partition: `rgds/userdata`, `rgds/logs`, `roms/<system>`.
- **RG DS Core in the image.** `/usr/share/rgds/releases/<build>`, a release of its own with a fresh manifest
  (emulator launches verify it): the latest release package's emulators and Samba,
  plus this checkout's runtime, themes, assets and device launcher.

- **Wi-Fi without NetworkManager.** RG DS Core's Wi-Fi page used `nmcli`. On RG DS OS, `runtime/wpa_wifi.py`
  drives wpa_supplicant over its control socket instead (scan, join, saved networks, the on/off switch), so a
  password never goes through argv. wpa_supplicant saves networks to `rgds/wifi.conf` itself. `/usr/libexec/rgds/net
  wifi|wifi-off` is the switch, and the `wpa-event` hook (`wpa_cli -a`) runs udhcpc on each join and sets the clock
  once by NTP. A `wifi.conf` with only `network={}` blocks, from `rgds-setup/` on the boot partition, still works:
  `net` adds the header.

### v0 on the handheld (2026-10-27)

It boots and runs. Joining Wi-Fi from Settings worked first time and saved the network to `rgds/wifi.conf`.

- **Boot**, by the kernel's clock: `rcS` at 1.86 s, data partition mounted at 2.51 s, RG DS Core started at 2.58 s.
  U-Boot comes before that and isn't counted.
- **Input**: 24 ms median from button press to finished frame, 34 ms worst, none missed. A held direction redraws
  at 43 fps. (`latency.py` with `RGDS_UI_TRACE`, set through a bind mount over `session`.)
- **System**: 89 MiB of RAM in use, of 2.9 GiB. dmesg shows only missing-supply lookups. The processes are init,
  syslogd, the launcher, getty, wpa_supplicant, wpa_cli, udhcpc and dropbear.
- **Not yet used**: the top panel's touchscreen (Goodix on i2c-3, fe5c0000). The session only hands RG DS Core the
  bottom one (i2c-5).

### Games on RG DS OS (2026-10-27)

DS games run. Updates go to the other A/B slot over SSH (`tools/own_os_push.py`), leaving games and saves alone.
What it took:
- **A release of its own.** Every game launch verifies RG DS Core's files against `manifest.json`. The image now
  installs a release made for it, `/usr/share/rgds/releases/<build>`. Buildroot is kept from stripping it, and its
  local-package copy is redone from scratch each build: `rsync -u` had kept stale files. The build fails unless the
  release verifies in the image.
- **ROCKNIX's joypad is a mainline pad.** It shares its name with the stock driver but uses BTN_* codes, with F as
  BTN_MODE. The bridge, the in-game menu's key watcher and the input profile read it that way only in the
  independent session. The Home key's device tree code is now KEY_HOME (`dts-patches/0003`).
- **Menu without a compositor.** The game keeps both panels (the KMS mode), and the bridge draws the menu over it.
  There is no stock screen swap. The overlay finds each screen by its window title.
- **2x-5x.** The release's PanVK fork only drives kbase. Its Bifrost job queue calls kbase directly, so on panfrost
  it retried a submit forever: the game froze after about 32 frames. Mesa 25.0's upstream PanVK has no HOST_CACHED
  memory, which DSCore requires. `tools/build_panvk_rgds_os.py` builds upstream PanVK from Mesa 26.2.5 (its SHA-256 is
  pinned) into `/usr/lib/rgds-panvk`. It offers cached memory on panfrost 1.6 (`WB_MMAP`), and Mario Kart DS runs at
  2x with it. It needs `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`, which `emulators.py` sets on RG DS OS.

### Power, sleep, lid and restart (2026-10-27)

None of them worked from the menu at first: the independent session's power actions all went through `systemctl`
(`linux_platform.system_command`), and the committed power-off request was left for stock's supervisor, which RG DS
OS does not run. Now, on RG DS OS: reboot and power-off are BusyBox's (`/sbin/reboot`, `/sbin/poweroff`; init's
`rcK` stops RG DS Core, the network and the card), and the menu runs them itself once the request is committed.
Sleep goes through `/usr/libexec/rgds/power suspend`, which plays the part of the systemd sleep hook: it echoes the
menu's token to `/run/rgds/suspend-state` as `phase=pre`, writes `mem` to `/sys/power/state` (deep sleep), and
`phase=post` on resume, so `system_controls.suspend_linux` waits for the real resume. Verified by hand: 14 s of deep
sleep with an RTC alarm as the wake, counters and Wi-Fi back afterwards. The lid: the session exports the hall
sensor's event node and its platform device's wakeup file (`RGDS_LID_DEV`, `RGDS_LID_WAKEUP`), which the menu's lid
watcher and the helper's `lid()` use; the PMIC's power key, the hall sensor and the RTC are wake sources in the
device tree. The Power page gained a Restart row (seventh, after Power off). Power off and Restart first failed silently on
the handheld: the helper's `system_exit` asked stock's hand-off for an owner record that only stock's `session.py`
writes; on RG DS OS it now runs the action itself after the same checks. The System page's Startup entry (stock's
"Boot into" choice) is gone on RG DS OS (`RGDS_OS=1` from the session, `Options::rgds_os`). Lid sleep: the sensor is
debounced over two 50 ms polls, then the menu flushes its state, plays the close cue and starts the Python helper,
which is where the second or so before the screens go dark comes from.
A sleep indicator: the red status LED (one of three PWM LEDs: green power, amber charging, red status) is lit dimly
by the power script around the suspend, and `dts-patches/0004` keeps the red and amber LEDs' state through sleep
(the LED core otherwise switches them off). Whether the PWM keeps driving in deep sleep is the owner's observation.
The amber LED follows the charger through the power-supply core's trigger (`battery-charging-blink-full-solid`,
set in `rcS`): blinking while charging, solid when full. Lid sleep's second of delay is left as is for now.

### More systems: the libretro adapter (2026-10-27)

RG DS Play now plays libretro cores: `play/src/core/libretro_core.cpp` puts a core's `.so` behind `play::Core`
(video in every libretro pixel format, audio, the pad as a joypad with the bitmask fast path, save states through
serialize, the battery from `SAVE_RAM` with a revision that follows its contents, `peek` from `SYSTEM_RAM`, rumble
through the core's interface, the core's options left at their defaults). One core per process (the API has no user
pointer), so these systems have no shadow and therefore no live-map companion; the second screen shows the system
and the game. `rgds-play --core LIB --system ID --system-dir BIOS` chooses it; `runtime/play_options.py`
maps the extensions (`.nes .fds`, `.sfc .smc`, `.md .gen .smd .32x`, `.sms`, `.gg`) to systems and systems to cores
(`LIBRETRO_CORES`), and `emulators.py` adds the arguments when the core is installed under `/usr/lib/libretro`
(RG DS OS only; a release never ships a core). The cores come from Buildroot (`os/buildroot/package/libretro-*`,
BananaLinux's pinned recipes: FCEUmm, Snes9x, PicoDrive); the session lists `/data/roms/{nes,snes,md,sms,gg}` and
`rcS` makes them with `/data/rgds/bios`. First run on the handheld (build osa6769983): a generated one-bank
cartridge (a reset vector that loops) started through the real launch plan, FCEUmm took RGBX888, drew on both
panels under KMSDRM at 48 kHz, saved a state through the shared in-game menu and returned home after 843 frames.
A real game is the owner's next test (`/data/roms/nes`, `snes`, `md`, `sms`).

### Engine speed on RG DS OS: the first numbers (2026-10-27)

A benchmark that needs no one at the controls: `os/banana-probe/bench_record.py` drives the pad for 36 s while
DSCore records the inputs (`--record`), and `--replay` plays them back frame-exactly with `DS_FRAME_STATS=1
--stats-from 300` (`run_game.py` takes `ARGS=`, `BIN=` and `FRESH=1`). Mario Kart DS at 2x from the owner's race
state, 2400 measured frames, performance governor:

| Engine | emulation per frame, median / p95 / max | present thread, mean per panel |
| --- | ---: | ---: |
| shipped (zig cross-build) | 4.58 / 4.84 / 19.0 ms | 3.4 and 4.7 ms |
| GCC 13 + upstream's PGO profile (`--compiler gcc-pgo`) | 4.43 / 4.65 / 14.4 ms | 3.1 and 3.0 ms |

The heavier scene, Mario Kart's attract race from a fresh boot (`FRESH=1`, 100 s with no input, 5400 measured
frames): shipped engine median 9.8 ms, p95 14.8, 1.19 % of frames over budget, worst 67 ms; GCC+PGO median 9.9,
p95 14.1, 0.70 % over budget, worst 38 ms. The present thread there: 12-14 ms per panel per frame (its wait for the
flip included).

Where the frame goes (`DS_PROFILE=1` on the attract replay, direct tier, GCC engine; shares of the stage time):
ARM9 36 %, ARM7 10 %, the 2.5x output compose 14 % (1.6 ms a frame), 2D background draw 9 %, 3D geometry 7 %,
GPU line hooks 6 %, texture-cache prep 6 %, SPU 5 %, the line-0 worker join 4 %. So the CPU cores' emulation is the
bulk, the compose to 640x480 a modest slice: composing at the 3D size and scaling on a VOP2 plane (the atomic,
scaled `DrmOut` is in, inert until the core's `Gpu::PRESENT_W/H` becomes the 3D size) would give about 6 %; the
larger gains are JIT work (block linking, fastmem), the engine's own roadmap. Composing at the 3D size is not a
constant change: `Gpu::compose_row` is built around the 2.5x mapping (the `(2p+1)/5` column map, three sample
phases, the four-sample footprint resolve at 3x-4x and the exact 2x2 box at 5x, the HD marks the bridge reads at
native block centres), so it is a second compose path (1:1 3D rows, integer 2D rows, the 2D filter at integer
scale), about a day's engine work with its own verification; parked behind the JIT work. And a correction: the direct tier is
chosen by `[video] sink` in the config, which the launcher sets to `renderer` whenever a filter is chosen, so a
hand run needs `DS_RGDS_SINK=auto` before `launch_plan` and `DS_DIRECT_PRESENT=1` after; the "direct" latency rows
earlier in this document were renderer runs. On the real direct tier the present thread is 2.9 ms a panel
(compose 1.3, copy and flip 1.1) against 12-14 ms on the renderer tier in the same scene.

GCC+PGO is 3-4 % faster with a shorter tail; `own_os_build.py` takes the engines from `build/engines/`, so the
image carries it. Two things stand out. This scene is light (4.5 ms of a 16.7 ms budget), so 3-5x targets need
heavier ones (a fresh-boot attract demo is the next recording). And the present thread costs 3-5 ms per panel per
frame: the CPU composes a 640x480 canvas (at 2x a 1.25x nearest upscale of the 512x384 frame, at 3x a nearest
decimation of 768x576) and copies it into an uncached dma-buf. VOP2 can scale a plane in hardware, which removes
that work and gives 3x a filtered downscale instead of dropped pixels: the display item's first step.

### Boot chart (2026-10-27)

Measured with the ARM architected counter, which runs from SoC reset (`os/banana-probe/since_reset.c` prints
"before the kernel" = counter minus the kernel's uptime), U-Boot's bootstage table handed to the kernel in the device
tree (`CONFIG_BOOTSTAGE_FDT`, read from `/proc/device-tree/bootstage`), the kernel's own timestamps and
`/run/rgds/boot-times`.

| Stage | before | after this round |
| --- | ---: | ---: |
| boot ROM, TPL (DDR), SPL | ~1.0 s | ~1.0 s |
| U-Boot proper: pre-relocation driver init (`dm_f`) | 0.57 s | 0.55 s |
| U-Boot: prompt to `bootm` (bootflow scan, kernel + DT read from FAT) | 2.45 s | 1.53 s |
| U-Boot: `bootm` to the jump | 2.33 s | 0.61 s |
| **power to kernel** | **5.7 s** | **4.1 s** |
| kernel to init | 1.83 s | 1.70 s (debugfs off, CMA 64 MiB; jitterentropy is selected by the DRBG and stays) |
| init to RG DS Core started | 0.35 s | 0.38 s |
| RG DS Core to its first frame | 3.07 s | 0.69 s (`--boot-startup`) |
| **power to the first frame** | **~11.0 s** | **~6.9 s** (emblem at ~5.1 s) |

What changed: the kernel goes to the card as an LZ4 frame (14 MB instead of 28; U-Boot's `booti` unpacks it, but
only the modern frame with independent blocks, not the kernel's own `Image.lz4`, a legacy frame that U-Boot does not
recognise and that left the slot unbootable until `post-image.sh` packed it itself); U-Boot lost EFI, USB, PCI, NVMe,
SCSI, the ethernet driver and the VBE/script/PXE boot methods (`uboot-rgds.fragment`), which took 1.7 s out of its
bootm path alone. `tools/own_os_push.py --uboot` writes the build's U-Boot at 32K of the card and keeps the running
one in `rgds/logs/`.

**RG DS Core's own start** (`RGDS_LAUNCH_PROFILE=1`, now set by the session; one JSON line in `launcher.log`):
3.07 s from exec to the first frame, of which 2.2 s is the constructor before SDL is even initialised, and of that
2.1 s is three synchronous calls into the Python helper (the hardware probe 0.9 s, the volume 0.65 s, the saved
system settings 0.55 s; everything else, the shader catalog, preferences, locale, fonts, themes and a 34-game scan,
is under 0.1 s together). Then 0.3 s for the first KMS window and 0.3 s for assets and the first draw. The session
now starts RG DS Core with `--boot-startup`, the way stock's session does: the boot scene is drawn first, and the
hardware probe and the library scan run behind it. The kernel also paints the emblem through fbcon (`CONFIG_LOGO`
with `board/rgds/boot-logo.ppm`, 221 colours from `assets/boot/logo.png`) from the moment the console takes the
framebuffer, about 1 s, until the scene replaces it.

**Kernel initcalls** (`initcall_debug` once, `rgds/logs/dmesg-initcall.txt`): 1.46 s of the 1.85 s to init.
The panels' driver 700 ms (two panels in sequence), the I2C buses with the PMIC's regulators 137 ms, CMA setup
88 ms (a 256 MiB region from BananaLinux's patch; a few MiB are used), deferred probes 84 ms, the jitter-entropy
RNG's self-test 69 ms, clock debugfs 37 ms, the charger 25 ms; everything else is under 25 ms each. Cut here: the
jitter-entropy RNG and debugfs out of the config, CMA down to 64 MiB (`dts-patches/0005`).

Tried and measured, no gain: UHS for the card in U-Boot and a card-first `bootflow scan` (its prompt-to-bootm time
stayed 1.53 s; the next step there needs the UART, nothing in the bootstage table names what it waits for), the
SPI-flash probe removed (`dm_f` stayed 0.54 s), and probing the DSI driver asynchronously (it forced a synchronous
probe, a one-line patch lifts that, but the panels' 650 ms still held init: the DRM bind runs from the deferred-probe
worker, which `late_initcall` flushes, so the PMIC and the card still queue behind it). What would move it: shorter
panel delays (`delays=20,10,50,120,50` per panel in the DT, run in sequence for the two panels), or the two panels
prepared in parallel.

### Button latency in games on RG DS OS (2026-10-27)

Measured as press to completed page flip (the panel's own scan-out, up to one refresh, comes on top), Mario Kart DS,
20 presses per run: `os/banana-probe/pace_trial.sh` runs the game by hand with the bridge's `RGDS_LATENCY_TRACE`
and DSCore's `DS_CADENCE_LOG`, `game_latency.py` presses the real pad node, and `press_to_flip.py` joins the two
logs. The CPU governor was `performance` for all rows (the session boots on `schedutil`, whose ramp-up showed as a
91 ms worst case; a game now switches it and puts it back, `linux_platform.cpu_governor`).

| Path | Pacing | Press to flip, median | worst |
| --- | --- | ---: | ---: |
| renderer (a filter chosen), 1x | free | 31.0 ms | 64 ms |
| renderer, 1x | display lock | 24.0 ms | 31 ms |
| renderer, 2x | free | 37.3 ms | 48 ms |
| renderer, 2x | display lock | 25.4 ms | 79 ms (one adaptive switch) |
| direct present, 2x | free | 38.5 ms | 125 ms |
| direct present, 2x | display lock | 20.2 ms | 53 ms |

**3x slogged with the lock** (the owner's report): at 3x the present thread's larger upload puts start-to-swap at
19 ms, the lead (work plus band) at 31 ms, and the lock aimed two ticks ahead, missed one in three (band capped at
8 ms) and idled 22 ms after each miss: 45 fps with 11 ms frames, while free pacing held 60. DSCore now pays the lock
only while the lead is about a refresh (free over 1.5 periods, locked again under 1.3), and six missed ticks in 120
frames drop it to free pacing for 600 frames. The image carries this tree's engines (`own_os_build.py` overlays
`build/engines/*` on the release).

Free pacing lost a whole refresh: the panels here hold a clean 60.00 Hz grid while the limiter runs at the DS's
59.83 Hz, so nearly every swap waited for the previous flip and was shown a refresh later (swap-to-flip median 15-16
ms). DSCore's phase lock (`video.pace`, `phase_lock.h`) was built for exactly this and had stayed off on stock, where
the grid wanders; on RG DS OS `emulators.py` sets `DS_PACE=display` for DSCore. Its band settles near 7 ms with 22
missed ticks per 25 s; a tighter starting band (1.5 ms) missed 100 ticks and gained nothing. What is left is the
poll at the frame's start (half a frame, 8 ms median), the emulation itself (6 ms at 1x, 10-15 ms at 2x) and the
present's 2-3 ms; the next cuts are a faster engine (section 4 below) and a mid-frame input poll.

### Lag and whine on the probe, fixed (2026-10-26)

- **Lag.** Measured with `os/banana-probe/latency.py`: a median of 195 ms from button to present, a worst case of
  663 ms, and 2.5 fps on a held direction. The cause: mainline's RK817 driver names the playback volume `Master`,
  not `DAC`. Every first click after 3 s of quiet reopened the sink, failed to restore `DAC`, ran the synchronous
  Python helper on the UI thread, and closed the sink again. The launcher (`menu_sounds.hpp`), the bridge and
  `runtime/audio.py` now fall back to `Master`. Afterwards: 3 ms median, 58 ms worst, 44.5 fps held.
- **Whine.** This was the vibration motor, not the speakers. With haptics off it was gone; Banana's own menu,
  which doesn't vibrate, never had it. ROCKNIX's joypad driver sets the duty as magnitude/0xFFFF (stock's driver
  uses /0x8000), so our taps ran the motor at 19-50% of a 10 kHz PWM, and the coil sang. `haptics.hpp` now doubles
  magnitudes for `retrogame_joypad`, and `dts-patches/0002` moves the motor PWM to 25 kHz, above hearing. The
  25 kHz part only exists in our image; the Banana probe keeps 10 kHz.

### Leaving ROCKNIX's hardware layer (after v0 boots)

The owner would rather not carry ROCKNIX's drivers. Swap them one at a time on the second card, using the probe tools:

1. The joypad: mainline `gpio-keys` + `adc-joystick` + `pwm-vibrator`, which RG DS Core already supports. That also
   retires the `from_joypad` translation and the haptics scaling.
2. The panels: mainline `jadard` RG DS driver with our KMSDRM path and no Weston. This is the bisect Phase 0 never
   ran; watch for the Armbian blanking.
3. The kernel patches (charger, AW87391 amp, Goodix, sleep): keep what mainline lacks as our own small patch set.

### After v0: speed, picture and more systems

Rule one: **measure before and after every change**, on the same scenes, on stock and on RG DS OS. The tools exist:
DSCore's benchmark scenes (`dscore/scenes/*.dsin`) and PGO profiles (`dscore/pgo/`), the launcher's UiTrace and
`os/banana-probe/latency.py`, and `/run/rgds/boot-times`. A "3-5x" claim is a ratio of two such numbers.

**1. Boot (target: RG DS Core's menu in under 3 s from power).** Banana reaches its menu at 4.5 s. Owning init
gives us: no udev and no initramfs (done), panels, input and GPU built in (done), Wi-Fi off the critical path
(done), U-Boot with no delay (done). Still to do: trim the kernel config to this board (ROCKNIX's is generic), LZ4
or uncompressed `Image`, launcher start straight from `rcS` with its first frame from cached assets, and a measured
boot chart.

**2. Display (target: both panels in one atomic commit, in phase, no tearing).** Today SDL KMSDRM swaps each panel
separately. Next:
- Our own DRM present in the launcher and engines: one atomic commit for both CRTCs, plus Banana's pending VOP2
  "start video ports in step" patch, so both screens show the same frame on the same refresh.
- Scaling on VOP2's hardware planes (Cluster and Esmart windows scale in the display engine), so a 256x192 or
  640x480 picture costs the GPU nothing to enlarge.
- Input-to-photon measured with the latency tool, plus a camera test once.

**3. CPU and GPU policy (target: full speed when it matters, cool when idle).**
- Per-workload governors: the menu on schedutil with a GPU floor, DS games on performance with the emulator
  thread pinned to its own A55 core (`SCHED_FIFO`, other work kept off that core).
- No CPU idle states during a game frame, `vm.swappiness=0` (done), THP for the JIT cache.
- The CPU reaches 1.99 GHz on this kernel; check the OPP table and voltages against the PMIC for stability first.

**4. DSCore (target: 3-5x on the heavy 3D scenes over stock).** Ranked by expected gain:
- **GPU 3D on PanVK or Panfrost GLES 3.1**, instead of the stock vendor blob through the compat layer. Build Mesa
  with PanVK; Mesa 25.0's Panfrost needs no LLVM, while newer PanVK may need the CLC toolchain, so weigh that cost.
  DSCore's Vulkan path (`build_rgds_vulkan.py`) then runs on an open driver we can fix.
- **PGO and LTO for this exact CPU**, built with our own toolchain (`-mcpu=cortex-a55`), with profiles from the
  benchmark scenes.
- JIT block linking and fastmem on our kernel, which has no vendor hacks in the way.
- The geometry and rasteriser split across two cores, with the third free for audio and the frontend.

**5. HD smoothing (target: 4x 2D upscale and 2-4x 3D internal resolution at full speed).** Today `rgds_2d=4` runs
the CPU rules plus a GPU reconstruction, and `5` is HD smooth.
- Move the 2D rules to a GLES 3.1 compute or fragment pass, so the CPU does none of it.
- Add an xBRZ/ScaleFX-class edge-directed upscaler tuned for DS sprites and fonts, plus a "ridiculous" mode: 4x
  plus a sharpening reconstruction tuned for the 640x480 panels.
- 3D at 2x or 4x internal resolution through the GPU renderer, downsampled with a proper filter.
- Every mode measured per scene; a mode that can't hold 60 fps in a game is greyed out for that game.

**6. More systems (target: every system the stock card had, the RG DS Play way).** Per the consistency rule, every
system gets the same library, in-game menu, saves, frames and shaders as the DS. RG DS Play already has the
`play::Core` interface (mGBA for GBA, GB and GBC). Add a **libretro adapter core** that loads a libretro `.so`, so
one adapter brings many systems, all under RG DS Play's menu and frames:
- First wave: NES (fceumm or nestopia), SNES (snes9x), Mega Drive, Master System and Game Gear (genesis_plus_gx or
  picodrive), PC Engine (beetle-pce-fast), Neo Geo Pocket (beetle-ngp), WonderSwan (beetle-wswan), PlayStation
  (pcsx_rearmed), PICO-8 (fake08).
- Second wave: N64 (mupen64plus-next, GLES), arcade (fbneo), Dreamcast and PSP if they are fast enough on the A55s.
- The cores are built in Buildroot as packages (Banana's `libretro-*` recipes are a starting point). Each system
  gets the companion screen where it makes sense (SNES and PlayStation maps, as GBA does).

If the screens blank on touch under Banana too, the fault is in this unit's hardware or a shared path (power,
touch). Next is the UART at 1500000 baud plus `drm.debug=0x1e loglevel=8` in `extlinux.conf`.

## Phases

Each phase ends with a demonstrable exit test. Skip ahead only if an earlier phase turns out to be unnecessary.

### Phase 0: See the failure (1 to 2 days)

Make the Armbian crash observable, time-boxed. We learn the real fault and whether it lives in the kernel/DT or in userspace.

- Get persistent evidence that survives a hard power-off: a USB serial console or the UART pads for `console=ttyS2,1500000`, `pstore`/`ramoops` (reserve memory in the DT), and `netconsole` or USB gadget ethernet. Log with `drm.debug=0x1e` and `loglevel=8`.
- Reproduce on the old Armbian image if it can still be rebuilt from `build/os` scripts; if not, skip to Phase 1.
- Run the bisect that costs nothing: boot with no compositor and no touch (blank text console on both screens, `kmscube` or `modetest -s` on each connector), then add Weston, then add touch. Note which step first flashes.
- Record per step: `/sys/kernel/debug/dri/*/state`, `dmesg | grep -E 'panfrost|vop|dsi|flip_done|underflow|timeout'`, and `cat /sys/kernel/debug/clk/clk_summary` for the VOP/DSI/PLL clocks.

Exit: one sentence naming the failing layer (clock, VOP, GPU, power, input) with a log line as proof.

### Phase 1: Known-good base (about 1 week)

Stop debugging someone else's distro. Start from a kernel and device tree that is already proven on this panel:

- Build BananaLinux's `rgds` image with `make rgds` (Linux host; first build takes an hour or more) and boot it on the second card. Confirm both screens, touch, buttons and audio work on the owner's handheld.
- Record exactly which pieces matter: kernel version and patch set, RG DS device tree, panel and DSI driver patches, U-Boot, the `rk817` charger/audio drivers, panel clock tuning.
- Decide: **fork their `buildroot/` board layer** (preferred, with their consent) or **extract kernel, DT and U-Boot into our own Buildroot external tree**. Either way, our userspace (RG DS Core, DSCore, the play/ GBA/GB player) is layered on top; the stock-firmware rootfs is not reused.

Exit: our own image boots on the second card to a text prompt on both screens, with touch and buttons reporting events.

### Phase 2: Display path (about 1 week)

- Run RG DS Core with its **direct KMS present** and no compositor, as on stock. Input via evdev/libinput directly.
- Prove it under input: tap, drag, hold the screen for 10 minutes. Watch for `flip_done`, `underflow` and Panfrost timeouts in `dmesg`.
- Compare GPU stacks. Stock uses Mali blob compat; mainline uses Panfrost/Mesa. The repo already builds PanVK (`build_panvk.py`); check that DSCore's Vulkan path and the GL filters work on it.
- Keep the refresh-rate/phase-lock findings in the memory notes (panel grid wanders 59.2 to 60.3 Hz) in mind; the DT clock tuning may fix this properly.

Exit: menu plus one DS game on both screens for 30 minutes with touch, no flashing, no hangs.

### Phase 3: System services (about 1 to 2 weeks)

Re-create what stock gave us for free: battery and charger (`rk817_charger.ko` is in `build/os`), lid/hall switch sleep, suspend and resume, audio mixer and headphone detect, brightness, haptics, Wi-Fi (`rtw88_8821cs` SDIO fix, then Bluetooth later), RTC, LEDs, safe power-off and low-battery shutdown.

Exit: a day of normal use without a forced power-off.

### Phase 4: Image, updates, install (about 1 week)

- Layout: boot, system A, system B (read-only), data partition (exFAT, readable on a computer).
- Updater that writes the inactive slot and falls back if the next boot fails.
- Flasher/installer instructions (the owner's existing OTA channel and signing key in `docs/OTA.md` can be reused).
- `tools/qemu-boot.sh` style smoke test so most of the work can be checked without a handheld.

Exit: flash from a fresh download, boot, update, roll back.

### Phase 5: Port the features

Frames and shaders, cheats, GB/GBC and GBA players, PortMaster compat (needs its own look on mainline libc and Mesa), PictoChat, languages. Most of this is application code and should move over unchanged.

PortMaster: in the image and run on the handheld on 2026-10-27. PortMaster installs and its GUI opens, and Wizznic, Bugdom, Sokobrawn (armhf), Magical Broom and Chromium B.S.U. run, with the menu on the bottom panel. It adds bash, PortMaster GUI's SDL libraries with PNG, fontconfig with DejaVu, GLES 1 in Mesa, a DRM lease per port process for the top panel and one for the menu helper on the bottom panel, and RG DS OS's own compat manifests (arm64 from focal, armhf from Debian bookworm for a KMSDRM SDL and Mesa 22.3). Lesson: Buildroot does not rebuild a package when its options change; dirclean it. See docs/PORTMASTER_PLAN.md, "PortMaster on RG DS OS".

## Decision gates

- After Phase 0: if the cause is in userspace (compositor or app), Phase 1 can reuse our Armbian base. If it is in the kernel, DT or clocks, go to BananaLinux/ROCKNIX as planned.
- After Phase 2: if the dual-screen present is stable, continue. If not, stop and reconsider; the stock-card install already works, and this project is optional.

## Open questions for the developer to answer early

1. Which kernel version and branch does BananaLinux's RG DS image use, and is it a ROCKNIX tree or a vendor BSP?
2. Does the RG DS Plus need different panel handling than the RG DS? (BananaLinux retunes them differently.)
3. Where is the UART on the board, and is a USB-C gadget serial available from the bootloader?
4. How do the A/B slots and the data partition interact with the handheld's SD slot numbering (TF1 vs TF2)?
5. Does the owner's device boot from the second slot at all, or only from TF1?

## Pointers in this repo

- `build/os/` Armbian-era logs, scripts and the 7.1.8 kernel reference.
- `tools/os_probe.py` read-only facts from a running stock device.
- The stock install and boot docs (`SD_FLASH.md`, `BOOT.md`, `DEVICE_SETUP.md`) were removed with the stock runtime; see git history before the removal.
- `docs/RGDS_VULKAN.md`, `docs/PERFORMANCE.md` GPU and direct present notes.
- `docs/PORTMASTER_PLAN.md` what a PortMaster layer on a custom OS must provide.
