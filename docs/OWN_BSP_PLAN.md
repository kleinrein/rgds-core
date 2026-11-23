# Own board layer for RG DS OS: plan

Started 2026-11-20. Companion to [OWN_OS_PLAN.md](OWN_OS_PLAN.md) (its "Leaving ROCKNIX's hardware layer" section
is the seed of this one).

## Where we are

We do not run Banana's binaries. `os/buildroot` builds everything itself, from upstream sources:

| Part | Source today | Whose it really is |
| --- | --- | --- |
| Kernel | gregkh `v7.2.7` tarball | upstream stable |
| Kernel config | `board/rgds/linux-rgds_defconfig` (775 settings, a `savedefconfig`) + our two fragments | **ours** since 2026-11-23 (was ROCKNIX's 7771-line handheld config) |
| Device trees | `board/rgds/dts/*.dts` (1341 / 1392 lines) + our 9 dts-patches | **Banana's**, ROCKNIX drivers (`rocknix,generic-dsi`, single-ADC joypad) |
| Out-of-tree drivers | `board/rgds/kernel/` (panel-generic-dsi, rocknix-singleadc-joypad, rk3568 pm_config) | **ROCKNIX's**, with our early-power changes in the panel |
| Kernel patches | `board/rgds/linux-patches/` (21) | mostly ROCKNIX/Banana, 2 ours (`7-rgds-*`) |
| U-Boot | upstream `v2026.01`, **`quartz64-a-rk3566_defconfig`** + our fragment + 4 patches | upstream, borrowed board |
| BL31 (TF-A) | `rkbin/rk3568_bl31_v1.45.elf` | Rockchip binary |
| DDR init (TPL) | `rkbin/rk3568_ddr_1056MHz_v1.23.bin` | Rockchip binary |

So "our own" means owning the **board layer** — device tree, config, drivers, U-Boot board — not forking a kernel.
Mainline already carries `rk3568-anbernic-rg-ds.dts` (jadard panels, gpio-keys/adc-joystick); the diff against
Banana's is 767 lines (`.cache/own-os/dts/mainline-vs-banana.diff`). The DDR blob stays: there is no open RK3568
DDR init.

## Goals

1. Our DTS = mainline's RG DS / RG DS Plus file + a small, reviewed overlay of our fixes. No ROCKNIX compatibles.
2. Mainline drivers only (jadard panel, gpio-keys, adc-joystick, pwm-vibrator); our patches kept small and
   upstreamable.
3. A kernel config written for this board (~a few hundred meaningful lines), not a generic one.
4. An `anbernic-rg-ds-rk3568` U-Boot board instead of Quartz64's.
5. Faster: more GPU/memory headroom, less frame jitter, quicker boot. Every step measured before/after.

## Perf levers, ranked by expected gain

1. ~~DDR at 1560 MHz~~ **ruled out 2026-11-20: the board has LPDDR3.** The TPL's DRAM type in PMUGRF OS_REG2/3
   (mainline `rockchip-dfi` encoding) reads 6 = LPDDR3. The memory runs at 920 MHz, which is stock's own setting
   (924; the PLL's nearest step) and the LPDDR3 ceiling. Rockchip's 1560 blobs apply only to DDR4/LPDDR4/4X. What's
   left here is power, not speed: a lower memory clock in Saver (Phase B2).
2. **Quiet emulation core.** DSCore and Play already pin the emulator to CPU 3 and IRQs to CPU 0. Add
   `nohz_full=3 rcu_nocbs=3 irqaffinity=0` (CONFIG_NO_HZ_FULL) so CPU 3 takes no scheduler tick or RCU callbacks
   while a game runs, plus a `cpu_dma_latency` PM-QoS hold from the session during games so CPU 3 never pays the
   idle-state exit (the idle states come from ROCKNIX patch `1001`). Target: fewer over-budget frames on the heavy
   attract scene (today 0.75 %).
3. **Board-sized kernel config.** ROCKNIX's config builds drivers for many handhelds. Trim to this board:
   - `CONFIG_HZ_1000` (hrtimers already do pacing; test whether wakeups land earlier on the vblank edge).
   - Drop features the Cortex-A55 doesn't have (SVE/SME/MTE/GCS/POE paths), KALLSYMS_ALL, unused filesystems, buses
     and drivers. Gains: boot time (initcalls), memory, a smaller LZ4 kernel to load.
   - Check `mitigations=` cost on A55 with a syscall/IPI micro-bench; keep whatever buys nothing.
4. **U-Boot proper on a diet.** Own board defconfig with only SD, the FAT boot partition, bootcount and extlinux;
   SPL straight to U-Boot with no dead probes. Before-the-kernel is 4.1 s, of which boot ROM/TPL/SPL are ~1.0 s.
   Falcon mode (SPL loads the kernel directly) saves more but loses bootcount/A-B fallback: only if it can keep both.
5. **GPU and CPU OPP check.** Compare mainline's rk3568 OPP tables with stock Android's (voltages vs the RK817/FAN53555
   rails). Not to overclock — to make sure we aren't running at a lower OPP or higher voltage than the silicon needs.
6. **Open TF-A (later, optional).** Upstream TF-A has an RK3568 port. Owning BL31 is "own solution" in full, but our
   true sleep relies on BL31 v1.45's SiP deep-sleep mode and PMIC sleep pin (True sleep, 2026-10-31). Only if
   upstream does both; no speed to win here.

## Phases

Each phase lands on one A/B slot first, with the other slot left on the known-good build. Use a copy of userdata for
every test run, never the owner's saves; don't take the panels while the owner is playing.

### Phase A: Measure the baseline (1 day)
- Record on the current image: boot chart (`/run/rgds/boot-times`, bootstage), over-budget frames and fps on the
  bench scenes (MK attract, MK 3x, a PSP and an N64 scene), press-to-flip, menu/sleep current.
- Read the RAM type and the stock DDR rate (done: LPDDR3, ~920 MHz on both; lever 1 is out).

#### Baseline, 2026-11-20 (slot b, 0.9.1-0de9d393e37c-os8974f5f1, read-only probes)

**Memory.** 3 GB (MemTotal 3,051,632 kB). The DDR clock is **920 MHz**, measured two independent ways:
- the kernel's `rockchip_ddr` perf PMU (`cycles`, read with perf_event_open from python; no `perf` tool on the image)
- the CRU registers through `devmem`. DPLL_CON0/1 = `0x6073`/`0x1441`, so 24 × 115 / 6 = 460 MHz, and the DDR
  clock is twice the DPLL. The same decode gives APLL = 1992 MHz, which matches the CPU.

That is a cold boot with no sleep since (suspend_stats 0/0). The menu idles at ~200 MB/s read and ~13 MB/s write
(mostly the two panels' scanout).

**RAM type: LPDDR3.** PMUGRF OS_REG2 = `0x5000CCA0` and OS_REG3 = `0x30000002`, so DRAM type (bits 15:13, extended by
OS_REG3 bits 13:12 since sysreg v3) = 6 = LPDDR3.

**DDR blob, stock vs ours.** The stock eMMC loader (read with `dd` from `mmcblk0`, nothing written) carries the same
DDR firmware: `ddr-v1.23-03ea844c5d`, rebuilt 25/11/10, with SPL 2017.09 `fwver v1.14`. It differs from our rkbin
blob only in the date and in the per-type frequency fields: u16 at 0xe068/0xe094/0xe0c0/0xe114, low 12 bits = MHz,
confirmed by rkbin's v1.26 1056/1332/1560 blobs, which differ only there.
- Ours is 1056 for every type.
- Stock is 1560 for DDR4/LPDDR4/4X and **924 for LPDDR3** (the third block).

This board runs the LPDDR3 entry, so stock and we both end up at ~920. Stock's blob (`ddr_stock_v1.23`, SHA-256
`0386170a…54bd`) is kept in the session scratchpad only.

**Rails and OPPs.** Mainline's fixed tables: CPU 1.15 V at 1800 *and* 1992 MHz (0.9 V up to 1104), GPU 1.0 V at
800 MHz (0.825 V up to 400). No per-chip leakage tables. Menu temperatures: CPU 57 °C, GPU 56 °C.

**Boot (U-Boot bootstage, µs since U-Boot start).**

| Mark | Time (s) |
| --- | --- |
| `board_init_f` | 0.007 |
| `board_init_r` | 0.87 |
| `main_loop` | 0.95 |
| `bootm_start` | 2.49 |
| `start_kernel` | 3.09 |

| Linux (`/run/rgds/boot-times`) | Time (s) |
| --- | --- |
| rcS started | 1.51 |
| data ready | 1.79 |
| RG DS Core | 2.00 |

The 1.54 s from main_loop to bootm_start is mostly reading `/b/Image.lz4`: **14.1 MB**. The 0.6 s after that is
LZ4 decompression and the jump. (The trimmed-kernel reboots below split it: ~11 MB/s from the card plus ~0.3 s of
fixed work.) Both shrink with the kernel, so lever 3 (a board-sized config) is also the
biggest boot lever left, ahead of U-Boot's own init.

**Games, memory traffic (1 s averages, DDR 920 MHz, GPU 800 MHz).** LPDDR3 at 920 MHz on an assumed 32-bit bus
peaks near 7,000 MiB/s, so neither engine measured so far comes near the bus:
- Dreamcast, Crazy Taxi (driving): median 1,981 MiB/s, max 2,287.
- 3DS (Ocarina 3D, 3D Land, Mario Kart 7; 347 s): mean 1,209 MiB/s, p95 2,027, max 2,309.
- Menu: about 216 MiB/s.

Their limits are the CPU JIT and the GL driver, not memory. Collected passively: `/run/rgds/ddrlog.py` (nice 19, 1 s rows, read-only)
logs DDR MHz, read/write MB/s, GPU MHz and the running engine to `/run/rgds/ddrlog.tsv` (tmpfs, gone on reboot)
while the PSP, Dreamcast and 3DS sessions ran their own scenes. Still to record: DS bench scenes
(`/data/rgds/bench/mkds-*.dsin`) on a data copy, press-to-flip, menu and sleep current. Those need the panels, so
they wait for a free slot on the handheld.

### Phase B: DDR 1560 trial — dropped 2026-11-20

The owner OK'd a trial, but the probes showed LPDDR3 already at its ceiling (see Baseline), so nothing was flashed.
Nothing faster exists for this RAM without overclocking past what Rockchip's and Anbernic's firmware allow.

### Phase B2: memory clock per power mode (battery only)
- BL31 already implements DDR frequency switching for Rockchip's downstream `rockchip_dmc` driver: the SiP
  `DRAM_SET_RATE` call over a shared page, using frequency points the TPL trained at boot (the blob's f0–f3).
- Write a small mainline-style devfreq driver for it, keyed to the DFI counters we already read. Switch only in
  vblank (the downstream driver blocks the VOPs around a switch so the panels don't underflow).
- Owner's intent was the faster memory in **Performance** only. With LPDDR3 there is no faster memory, so the
  remaining value is battery: Saver and the menu at a lower memory clock (e.g. 528 or 780), Performance and
  Balanced at 920. Worth it only if Phase A's current numbers show the memory clock matters to idle mA.

### Phase C: Our device tree (3–5 days)
- Start from mainline `rk3568-anbernic-rg-ds.dts` (and `-plus`). Port, one change per commit, from Banana's:
  the VOP0/VOP1 clocks on HPLL/VPLL at 126.4 MHz (the blanking cure), 60.00 Hz timings, our dts-patches
  (motor 25 kHz, Home key, LEDs in sleep, CMA 64M), ramoops, the Plus board-ID bits.
- Swap drivers one at a time, bisecting on the second slot: input (gpio-keys + adc-joystick + pwm-vibrator; RG DS
  Core already supports a mainline pad, retires `from_joypad`), then panels (jadard + our early-power and
  in-step changes ported onto it). Watch for Armbian's blank-on-touch with the panels.
- Done = no `rocknix,*` compatible left, `board/rgds/kernel/` empty, both handhelds pass the hardware checklist
  (screens, touch both panels, buttons, stick, rumble, sound, headphones, Wi-Fi, BT, sleep/lid, charge).

#### Step 1 done, 2026-11-20: the RG DS tree is mainline's plus our differences (no behaviour change)

- `board/rgds/dts/rk3568-anbernic-rg-ds-mainline.dtsi` is Linux v7.2.7's `rk3568-anbernic-rg-ds.dts`, unchanged under
  a three-line note. It's a `.dtsi` because our `rk3568-anbernic-rg-ds.dts` takes the in-tree name, and U-Boot's
  `fdtfile` and the slots' `FDTDIR` load that name.
- `board/rgds/dts/rk3568-anbernic-rg-ds.dts` (~450 lines, was 1341) includes it and overrides only what RG DS OS
  runs differently, one commented section per reason:
  - ROCKNIX joypad instead of gpio-keys/adc-joystick/pwm-vibrator, and the Home key as KEY_HOME;
  - ROCKNIX generic-dsi panels with the 60.0013 Hz timings and init sequences;
  - the VOPs on HPLL/VPLL at 126.4 MHz;
  - the AW87391 driver we carry and its "AMP" control names, and the amp regulator's names;
  - deep sleep (`rockchip-suspend`, the PMIC sleep pin, `vdd_logic` off, LEDs keep their state);
  - the lid pin pulled up, the USB 1.1 host off, ramoops.
- `dts-patches` 0001–0004 are folded in and gone, 0006–0009 (Plus) stay. 0005 (CMA 64 MiB) was folded into
  `linux-patches/2-linux-0666-cma-region.patch` on 2026-11-23.
  `external.mk` copies `dts/*.dtsi` too.
- **Proof:** `tools/dtb_compare.py` reads two `.dtb` files and resolves every phandle by the target's `#…-cells`, so
  only real differences show (`tests/test_dtb_compare.py`). Replaying `external.mk`'s refresh step (copy, patches
  0005–0009) on a RAM copy of the kernel's Rockchip DTS folder, both compiled trees compare **equal in meaning**
  to the shipped DTBs (`105d6511…` RG DS, `1f4a9648…` Plus). Bytes differ only in node order and phandle numbers.
  So no hardware test is needed for this step.
- Checked first: the committed `.dts` plus patches 0001–0004 is exactly the source this was built from (its last
  commit is 1e1bcd2, 2026-11-03), so no committed edit was lost.

What mainline's tree vs ours showed (the overrides above are the whole list): 36 nodes only in mainline (its
input stack), 26 only in ours (the ROCKNIX joypad, the amps under their old names, sleep, ramoops), 28 differing
properties.

**Step 2, input (hardware test):** delete the joypad section and let mainline's gpio-keys + adc-joystick (through
its ADC mux) + adc-keys-play + pwm-vibrator stand. RG DS Core side to check first:
- the `rocknix_joypad` / `from_joypad` path in the bridge and launcher;
- haptics (today ff-memless on the joypad's own event device, `haptics.hpp`; pwm-vibrator is a separate device);
- the Home key code (mainline's 316/BTN_MODE vs KEY_HOME);
- stick calibration (ROCKNIX's tuning and inversion vs adc-joystick's ranges).

**Step 3, panels (hardware test):** delete the panel section and run mainline's jadard driver, carrying over our
`panel-generic-dsi.c` changes (early power, backlight timing) and the 60.0013 Hz timings if jadard's 59.6 Hz
disturbs the phase lock. Watch for the blank-on-touch seen with Armbian, with the clock section kept.

**RG DS Plus:** mainline has no Plus tree. Once steps 2–3 hold on the RG DS, the Plus can become the RG DS
`.dtsi` plus Plus differences the same way.

#### Step 2 done, 2026-11-20 (axis fix 2026-11-22): mainline input + our pad combiner

- The owner chose our own combiner. `src/pad/rgds-pad.c` (+ `rgds_pad_axes.h`) merges `gpio-keys-control`,
  `adc-joystick` and `pwm-vibrator` into one uinput pad with ROCKNIX's identity: `retrogame_joypad`,
  0x19/0x484B/0x1101/0x0100, phys `rocknix-singleadc-joypad/input0`, the same 17 buttons, ±1800 sticks with fuzz and
  flat 32, and rumble passed through with the motor's own FF bits.
  - It opens the sources through private nodes in `/dev/rgds-pad`, grabs them and unlinks their `/dev/input`
    nodes, then restores them on a clean stop.
  - It exits 3 when a kernel joypad exists (the Plus) or the sources are missing.
  - It runs on CPUs 0–2 at SCHED_FIFO 10.
- The stick math is ROCKNIX's to the unit (`tests/test_rgds_pad.py`, against a Python port of the driver):
  - mux state 0 → X (inverted), 1 → RY (inverted), 2 → Y (inverted), 3 → RX; sticks pair states {1, 3} (right)
    and {2, 0} (left). ROCKNIX's driver drives the select lines the other way round from mainline's `gpio-mux`: its
    channel c sets (sel_a, sel_b) = (1,1), (1,0), (0,1), (0,0), i.e. mux state 3, 1, 2, 0. The test derives the
    table from the driver's `joypad_amux_select` switch again;
  - rest position at first open, ×2 scale, radial deadzone 64, 250 % gain, clamp.
- Tree: the joypad section is replaced by:
  - F = BTN_MODE (mainline: KEY_HOME), Home = KEY_HOME;
  - `adc-joystick` polled every 5 ms (mainline: 60) and reporting raw mux channels 0..3 as ABS_X/Y/RX/RY, `<0 1023>`,
    no fuzz or flat;
  - the vibrator at 40000 ns.

  `dtb_compare`: the only change is the joypad node gone and mainline's input nodes in; the Plus is unchanged.
- Packaging: `package/rgds-pad` (enabled in the defconfig, dircleaned by `own_os_build.py`); `rcS` starts it in a
  restart loop right after `/run` and waits up to 1 s for `/run/rgds/pad-ready` or `/run/rgds/pad-none`.
  Cross-compiles cleanly with `-Werror`.
- **Hardware test passed, 2026-11-20** (owner at the handheld, `os/device-bench/pad_log.py`, slot a with the step 2
  tree and a hand-run combiner, compared with a baseline on today's ROCKNIX pad):
  - same identity (0019/484b/1101/0100), the same 17 buttons, all pressed (F = 316), Home = 102;
  - the same rumble effect bits (80, 81, 88, 89, 90, 96); a 1.5 s rumble through the combiner was felt;
  - full stick range on all four axes (rest 12/0/0/0, baseline 12/−14/0/0);
  - the menu, D-pad, A/B, left stick and F/Home behave as before.

  mainline's `adc-keys-play` (saradc channel 2, ~1.65 V at rest) never moved on this unit: the key it describes
  isn't there. Harmless, it never fires. Not tried: a DS pen stick, N64 analog, a PortMaster port.
- Bugs found and fixed on the way:
  - `/run` is mounted `nodev`, so the private nodes live in `/dev/rgds-pad`.
  - The sources' joydev nodes are hidden too, and the pad takes the lowest freed `jsN` (it is `js0` again).
  - **`realpath()` needs a `PATH_MAX` buffer under `_FORTIFY_SOURCE`.** Buildroot builds with it, my hand builds
    didn't: the first installed image aborted at boot ("buffer overflow detected"), after hiding the controls. It
    was fixed with a bind mount on the running system within minutes, then by an image. Hand test builds must use
    Buildroot's CFLAGS (`-D_FORTIFY_SOURCE=1 -O2` and friends).
- **Installed 2026-11-20:** `0.9.1-0de9d393e37c-os90451798` built 14:46, the same on both slots, running slot b,
  committed. It carries the trimmed kernel #17, the step 2 tree, rgds-pad, the U-Boot I/O-domain fix (already on
  the card), Flycast with patches 0002–0004 (dircleaned), the 3DS engine f1ac7770, and every session's work in the
  tree at that time.
- **Bug, found by the owner 2026-11-22: the left stick didn't steer in DS games.** The first table took ROCKNIX's
  channel numbers as mainline's mux states, which swaps states 0 and 3: the left stick's X came out as RX (the DS
  pen) and the right stick's X as X. The menu hid it, since either stick moves the selection there, and the
  hardware test above only checked that each axis moved. The table and the stick pairs were fixed and
  `test_mux_states_map_to_the_axes_rocknix_reported` was added. Image `0.9.1-0de9d393e37c-os2a88cb59`
  (rgds-pad md5 `b293ef99`) went on slot a. Slot b got the same pad in another session's `os2f9cf014`
  (2026-11-23). The owner confirmed both sticks are fine on 2026-11-23. A hardware check of a new input table must
  hold each direction and see which single axis moves (`pad_log.py`), not just the range.

### Phase D: Our kernel config and patches (2–3 days)
- `make savedefconfig` from a config built up from `rockchip` essentials + what `lsmod`/`/sys` show the board uses;
  diff against ROCKNIX's to see what drops. Add lever 2 and lever 3.
- Patches: keep what mainline lacks (rk817 charger fixes, AW87391/AW88166, Goodix fixes, mmc resume retry, VOP2
  in-step), rename to `rgds-NNNN`, each with a one-line why. Drop the rest after a check.
- Measure against Phase A.

#### First trim, 2026-11-20 (built, then tested on the handheld below)

**What the board really uses.** Read on the running handheld:
- Only 4 of the 284 shipped modules are loaded (the rtw88 Wi-Fi stack). The OS has **no udev/mdev**, so a module
  loads only when a script calls modprobe (Wi-Fi and Bluetooth do).
- The bound drivers are: DSI/VOP2, panfrost, the RK817 family, AW87391, Goodix, fan53555, dwmmc/sdhci, dwc3/ehci,
  SARADC, the ROCKNIX panel and joypad drivers, and SCMI.
- Neither DTS enables HDMI, SFC/SPI flash, PCIe, SATA, GMAC, the camera or the video decoder.
- PortMaster's runner needs cgroup2, mount namespaces, loop and squashfs; `/dev/uinput` and `/dev/fuse` are used.

**Where the 27.5 MB `Image` goes.** Summing object text+data from the link list (the `built-in.a` files are thin
archives, so their sizes mean nothing): 23.7 MB in all. The big parts nothing here uses are XFS (+ Btrfs and the
RAID6/XOR it drags in), NFS/SunRPC/lockd, V4L2/DVB (~0.5 MB), BPF (0.77 MB), netfilter/conntrack/NAT, USB Ethernet
and modem drivers, MTD/SPI-NOR/NAND, HDMI, seven other handhelds' panels, and about 20 USB touchscreens.

**`board/rgds/linux-board.fragment`** turns those off. Changes to built-in code only; existing modules are left as
they are, and anything people plug in stays built in: HID game pads and keyboards, USB-C audio, USB storage + UAS.
It also compiles in `NO_HZ_FULL` + `RCU_NOCB_CPU` for lever 2, inert until the command line names a CPU.

The merged config (Buildroot's exact `.config` + the fragment, `merge_config.sh` + `olddefconfig`) differs from
today's only in those options and the dependencies that fall with them.

| | Today (0.9.1 image) | Trimmed |
| --- | --- | --- |
| `Image` | 27,502,600 B | 20,627,464 B (−25 %) |
| `Image.lz4` (`lz4 -9 --no-frame-crc`, what U-Boot reads) | 14,104,251 B | 10,141,363 B (−28 %) |
| vmlinux text / data | 18.7 / 8.5 MB | 14.5 / 5.9 MB |

Expected at boot: ~0.4 s less card read at the ~9 MB/s U-Boot gets today, a little less LZ4 work, and fewer
initcalls. (Measured on the handheld below: −0.51 s to the kernel jump. Since then the fragment keeps
`SQUASHFS_ZLIB`/`SQUASHFS_LZO`, which PortMaster's runtime images may need, and drops two `DW_HDMI` lines Kconfig
overrode anyway.)

How it was built: in the VM's `/dev/shm` (its disk has 1.6 GB free), from a copy of Buildroot's patched
`linux-custom` without objects, `HOSTCC=/usr/bin/gcc`, `CROSS_COMPILE=<host>/bin/aarch64-buildroot-linux-gnu-`.

**On the handheld, 2026-11-20 (slot a, kernel `7.2.7-rgds #16`).**

Test image: slot b's exact 0.9.1 system (`0.9.1-0de9d393e37c-os8974f5f1`; rootfs, kernel and DTBs matched the build
cache by SHA-256) with only the kernel and `/lib/modules` replaced.
- Unpacked and repacked in the VM's `/dev/shm` with Buildroot's flags (`-noappend -b 128K -comp lz4 -Xhc`).
- File lists identical outside `/lib/modules` (owners, modes, file sizes).
- Pushed with a copy of `own_os_push.py` pointed at a scratch build folder, so the shared build cache was untouched.
- Modules: 284 → 117. Only netfilter/IPVS/V4L2 modules dropped; rtw88, hci_uart, btrtl, bluetooth and zram are
  present.

Checks, all passed:
- Boot: first boot clean, trial committed by the session.
- Both DSI panels connected and enabled; both Goodix touchscreens, the joypad (FF_RUMBLE advertised), power and
  volume keys, lid switch and headphone jack all present.
- RK817 sound card, battery and charger, panfrost at 800 MHz, USB gadget.
- Wi-Fi joined. Bluetooth via `bt on`: firmware loaded, BlueZ powered; `bt off` after.
- zram, cgroup2, and a loop-mounted squashfs all work.
- Real sleep via `power suspend` with a 30 s RTC wake: deep, `result=0`, panels and backlights back, Wi-Fi rejoined.
- Mario Kart DS attract replay, 75 s on the Vulkan engine, using `/data/rgds/perf-userdata` and `--save` in
  `/run`: both panels on KMS, audio, no kernel errors; the owner's MK files hash-identical before and after.
- **Not measured:** game fps (`--frames` is needed for a clean quit that writes `performance.ini`; the one there
  was stale), USB-C audio, a Bluetooth pad, a PortMaster port, USB network traffic.

| Mean of 3 reboots each (s) | Kernel #15 (today's config) | #16 trimmed | Change |
| --- | --- | --- | --- |
| card read (`main_loop` → `bootm_start`) | 1.546 | 1.194 | −0.35 |
| LZ4 unpack + jump (`bootm_start` → `start_kernel`) | 0.599 | 0.443 | −0.16 |
| `start_kernel` | 3.091 | 2.584 | **−0.51** |
| RG DS Core starts (Linux clock) | 2.10 | 2.00 | −0.10 |

About 0.6 s faster from power to menu. The same numbers say U-Boot itself is slow: ~11 MB/s from the card plus
~0.3 s of fixed work in that step, and LZ4 at ~45 MB/s, which is very slow for an A55. U-Boot's CPU clock, caches
and SD bus mode are the next boot lever (Phase E).

**Next.**
1. ~~An image with the fragment, pushed to the slot that isn't running~~ done above. Wiring (2026-11-20):
   - `rgds_os_defconfig` lists `linux-board.fragment` after `linux-rgds.fragment`.
   - `own_os_build.py` gained `kernel_config_drift()`. Before a build it stops with "run with --reconfigure" when
     the kept `output/.config` names other fragments than the defconfig (Buildroot ignores new defconfig lines
     otherwise), and runs `linux-reconfigure` when the built kernel lacks a fragment setting. After the build it
     fails if a setting still did not land (Kconfig overriding it).
   - `tests/test_kernel_fragments.py` covers both cases.
   - The check found two lines of the first fragment that never took effect: `DRM_DW_HDMI` and
     `SND_SOC_HDMI_CODEC`, which Rockchip's DRM selects. They were dropped, so the wired config equals the kernel
     tested on the handheld.
   - **The first image build after this needs `--reconfigure`**; it rebuilds the kernel.
   - Still to do: fps on a bench scene with `--frames`, with the A/B trial armed. Check the hardware
   list (both screens + touch, buttons, stick, rumble, sound + headphones + USB-C audio, Wi-Fi, BT pad, USB
   network, sleep/lid, charge, a PortMaster port), then the bootstage marks.
2. Why U-Boot reads the card at only ~9 MB/s (Linux gets UHS SDR104 on the same card). At 25 MB/s, 10 MB is
   0.4 s, not 1.1 s. Also try zstd instead of LZ4: smaller file, slower unpacking; on a slow read it may win.
3. ~~Then `savedefconfig` the result into our own `linux-rgds_defconfig` and retire `linux-rocknix.config`.~~ Done
   2026-11-23, see below.

#### Our own config, 2026-11-23 (build-side, no behaviour change)

- `board/rgds/linux-rgds_defconfig` is `make savedefconfig` of the kernel built for 0.9.1 (ROCKNIX's
  `linux-rocknix.config` + `linux-rgds.fragment` + `linux-board.fragment`): 775 settings instead of 7771 lines.
  `linux-rocknix.config` is gone; `rgds_os_defconfig` builds on the new file and still lists both fragments, which
  stay the place to change a setting (one line and a reason each).
- Proof, in a RAM copy of the patched kernel tree in the VM with Buildroot's cross compiler: `olddefconfig` of the
  defconfig, and Buildroot's merge (`merge_config.sh -m` with both fragments, then `olddefconfig`), each give back
  the built `.config` byte for byte, apart from `CONFIG_OPENSSL_SUPPORTS_ML_DSA`, a probe of the build host's
  OpenSSL (Buildroot builds with its own).
- `own_os_build.py`'s drift check now also stops a build whose kept `output/.config` names another base config
  (`--reconfigure` needed once, as with the fragments); `tests/test_kernel_fragments.py` covers it.
- **Still in it from ROCKNIX, the owner's call** (docs/PERFORMANCE.md measured an 85 µs Panfrost job submit):
  `DEBUG_SPINLOCK`, `INIT_ON_ALLOC_DEFAULT_ON` + `INIT_ON_FREE_DEFAULT_ON` (every page zeroed on allocation and on
  free), `SCHEDSTATS`, `DEBUG_DEVRES`, `REGULATOR_DEBUG`, `PROFILING`. The init_on pair can be tried without a build
  (`init_on_alloc=0 init_on_free=0` on the command line); the rest need a kernel build.

#### ROCKNIX's debug defaults on trial, 2026-11-23 (owner's yes)

Test kernel #18: the built kernel's exact config minus `DEBUG_SPINLOCK`, `SCHEDSTATS`, `INIT_ON_ALLOC_DEFAULT_ON`,
`INIT_ON_FREE_DEFAULT_ON`, `DEBUG_DEVRES` and `REGULATOR_DEBUG` (`SCHED_INFO` falls with `SCHEDSTATS`). It was built
in the VM's RAM copy of the patched tree and packed with slot b's exact `os2f9cf014` system: only the kernel and
`/lib/modules` were swapped (same 117 modules, Buildroot's depmod). It went on slot a with a scratch copy of
`own_os_push.py`. Then `init_on_alloc=1 init_on_free=1` on slot a's boot line put the zeroing back alone. Afterwards
the handheld went back to slot b, with the boot file restored and the trial committed.

Measures: `os/device-bench/kbench.c` (median of 5 rounds, pinned to CPU 3, CPU at 1992 MHz under the performance
governor), and three Mario Kart DS attract replays (`mkds-attract.dsin`, 3600 frames, on `/data/rgds/perf-userdata`,
`DS_FRAME_STATS=1`, session held).

| | Today's kernel #17 | #18, no debug, no zeroing | #18 with zeroing back |
| --- | --- | --- | --- |
| syscall (ns) | 182 | 181 | 182 |
| first touch + free of a page (µs) | 4.65–4.68 | 4.02–4.06 | 4.78–4.84 |
| pipe round trip, CPU 3 ↔ 2 (µs) | 19.9–20.0 | 18.6–18.7 | 18.5–18.7 |
| futex wake (ns) | 347 | 344 | 347 |
| mmap + munmap 64 KiB (µs) | 8.5–8.6 | 8.2 | 8.6 |
| MKDS emulation work, mean (ms) | 10.13 / 10.15 / 10.16 | 9.89 / 9.87 / 9.86 | 10.16 / 10.16 (a third run, 120 s after boot, was hit by boot-time work) |
| MKDS frame time, mean (ms) | 9.52 / 9.52 / 9.55 | 9.27 / 9.27 / 9.26 | 9.53 / 9.54 |
| MKDS work frames over budget (of 3600) | 32 / 29 / 31 | 27 / 30 / 27 | 33 / 30 |

What it says:
- **The page zeroing is the whole game-side gain: −2.7 % emulation work in Mario Kart DS**, with run-to-run spread
  ~0.3 %. That is more than the PSP trial on 5 November showed (+0.4 % frames, docs/PERFORMANCE.md). The cost is
  hardening: freed and newly allocated pages are no longer wiped, so a kernel memory-disclosure bug could leak old
  data. Owner's call.
- **The four debug options cost no measurable game time**, only kernel paths: −6.5 % on a cross-CPU wakeup (spinlock
  checks and scheduler statistics). They give nothing in safety on a shipped handheld. Turning them off is a
  fragment change (`linux-board.fragment`) and a normal image build.
- No problems on #18: the boot was clean, the trial committed, and both panels, sound, battery, charger, Wi-Fi modules
  and the pad all came up. Not tried on #18: sleep, Bluetooth, PortMaster.
- **Owner's decision, 2026-11-23: both off.** All six lines are in `linux-board.fragment`. Merged over
  `linux-rgds_defconfig`, they give exactly the tested #18 config. The next image build reconfigures the kernel
  (own_os_build's drift check runs `linux-reconfigure`) and rebuilds it. Check sleep, Bluetooth and a PortMaster
  port on that image.

#### Lever 2 on trial: `nohz_full=3 rcu_nocbs=3`, 2026-11-23 (owner's yes)

Kernel #17 (today's, `NO_HZ_FULL` and `RCU_NOCB_CPU` compiled in since the trim) on slot b, with only the boot
line changed. `dmesg`: "Full dynticks CPUs: 3", "Offload RCU callbacks from CPUs: 3". Each run started after the
boot's background work was over (uptime ≥ 200 s). rcS already sends every IRQ to CPU 0 and the session keeps
itself on CPUs 0–2, so CPU 3 has only the emulation thread.

| | Normal boot line | `nohz_full=3 rcu_nocbs=3` |
| --- | --- | --- |
| MKDS emulation work, mean (ms) | 10.13 / 10.15 / 10.16 | 9.91 / 9.84 / 9.88 (−2.7 %) |
| MKDS work frames over budget (of 3600) | 32 / 29 / 31 | 24 / 23 / 27 (about −20 %) |
| MKDS work p99 (ms) | 16.63 / 16.48 / 16.63 | 16.40 / 16.03 / 16.32 |
| CPU 3 timer interrupts | 250/s (HZ) | ~12/s during the replays |
| Crazy Taxi drive, game fps (whole drive / 65–135 s; one drive each) | 55.96 / 58.04 | 55.84 / 57.79 |
| kbench on CPU 3: syscall / pipe / fault | 182 ns / 20.0 µs / 4.7 µs | 522 ns / 27.1 µs / 5.1 µs |

- **DS gains** about as much as the page zeroing did, and on top of it the frames over budget drop by about a
  fifth: the emulation thread no longer takes a tick every 4 ms.
- **Dreamcast is unchanged** (one drive each, differences within a drive's own spread).
- **The price is paid by every kernel entry on CPU 3:** context tracking makes a syscall ~340 ns dearer. A CPU 3
  thread making 10,000 syscalls a second would lose ~0.3 % of the core. DS and Flycast's SH4 thread don't; 3DS
  and PSP have not been measured.
- The boot line went back to normal afterwards. Shipping it means adding the two words to `extlinux.conf.in`'s
  `APPEND`; the OTA rewrites `extlinux.conf`, so updated handhelds get it too.

**3DS and PSP, checked before shipping (the owner asked), 2026-11-23.** The same boot lines, each run ≥ 200 s after
boot, with the 3DS session's `race-run.sh` (Mario Kart 7, 150 s, its userdata copy) and the PSP session's `run2.sh`
(OutRun Cross route, 90 s, Performance clocks, `RGDS_PLAY_CPU=3` as a real launch), driven by
`/data/rgds/kbench/nz.sh` and `psp2.sh`:

| | Normal | `nohz_full=3 rcu_nocbs=3` |
| --- | --- | --- |
| 3DS MK7, ms per frame (60.00 fps, 0 over 16.7 ms in all) | 11.63 / 11.24 | 11.42 / 11.22 |
| PSP OutRun, frame ms mean per race | 30.19 / 32.90 / 28.33 / 29.20 (mean 30.15) | 32.97 / 34.14 / 29.13 / 34.65 (mean 32.72) |
| PSP 300-frame windows reached in 90 s | 8 / 8 / 9 / 9 | 7 / 7 / 9 / 7 |

- **3DS: no difference.**
- **PSP: about 8 % slower with nohz_full**, and every nohz race was slower than the normal race in the same
  position. PPSSPP's main loop on CPU 3 drives GL, so it enters the kernel all the time (ioctls), and on a
  `nohz_full` CPU each entry pays the context-tracking cost (syscall 182 → 522 ns).
- **So not shipped globally.** It would only pay if CPU 3 held nothing but syscall-light emulation threads (DS,
  Dreamcast's SH4, 3DS) and PSP's GL-driving main loop moved to another CPU. That needs its own PSP measurement,
  since that loop is the PSP's limit (docs/PERFORMANCE.md).
- **Owner's decision, 2026-11-23: leave it off.** The kernel keeps `NO_HZ_FULL` compiled in (inert without the
  boot words), so a later trial still needs no build.

#### The kernel patches, 2026-11-23

All 21 still apply to 7.2.7, and each one does something this hardware needs. They keep their names: the numeric
prefixes set the order Buildroot applies them in, and the names are what the docs and the history refer to.

| Patch | Why | Needed by |
| --- | --- | --- |
| `1-mainline-0003` pwm_set_period | helper for the ROCKNIX joypad's rumble | Plus only |
| `1-mainline-0004` adc-keys 316 → joypad | routes the Plus's F key (adc-keys, BTN_MODE) into the ROCKNIX joypad | Plus only |
| `6-rk3326-003` input-polldev | the polling layer the ROCKNIX joypad driver is written against | Plus only |
| `2-linux-0002` rk817 names | `battery` / charger names the runtime reads | both |
| `2-linux-0008` SD resume retry | the card comes back after a deep sleep that power-cycled it | both |
| `2-linux-0021` dma-names | `dma-names` for UART1, the Bluetooth UART, so its DMA channels can be found | both |
| `2-linux-0026` AW87391 | the RG DS's speaker amplifier driver (not in mainline) | RG DS |
| `2-linux-0027` AW88166 firmware | the Plus's amplifier loads its firmware without stalling boot | Plus |
| `2-linux-0028` fan53555 STI8070A | the Plus's `vdd_cpu` chip reports a die ID the Silergy path rejects (cpufreq would defer forever) | Plus |
| `2-linux-0666` CMA 64 MiB | default CMA region; **folded with `dts-patches/0005` on 2026-11-23** | both |
| `2-linux-1001` rk356x idle states | CPU idle states (power) | both |
| `2-linux-1011` Goodix | skip the config-from-disk wait (~60 s) | both |
| `3-default-9901` sync suspend | suspend/resume in order (sleep reliability) | both |
| `5-mainline-rockchip-001…008` | RK817 charger and fuel-gauge fixes (state of charge across boots and sleep) | both |
| `7-rgds-0001` DSI async probe | ours: boot time | both |
| `7-rgds-0002` VOP2 ports in step | ours: both panels start on the same frame | both |

The three Plus-only input patches, `kernel/rocknix-singleadc-joypad.c` and `rocknix-joypad.h` go once the Plus moves to
mainline input + `rgds-pad` (step 2 for the Plus, which needs a Plus at hand to test).

The CMA fold: the region was 256 MiB in `0666` and cut to 64 MiB by `dts-patches/0005` after it; now `0666` says 64
MiB itself and `0005` is gone. The new `0666` reverses cleanly on the built kernel tree (the same file, so the same
DTB).

### Phase E: Our U-Boot board (2 days)

#### Findings, 2026-11-20 (static: U-Boot tree in the VM, registers read on the handheld)

U-Boot boots this board with **Quartz64-A's device tree** (`CONFIG_DEFAULT_DEVICE_TREE="rockchip/rk3566-quartz64-a"`,
an RK3566 board). U-Boot v2026.01's `dts/upstream` has no RG DS tree; the RG DS tree landed in Linux 7.1. Compared
with ours, three things matter:

1. **The `vccio4` I/O domain is set to 1.8 V mode while the RG DS powers it from 3.3 V.**
   - Quartz64 says `vccio4-supply = <&vcc_1v8>`; the RG DS says `<&vcc_3v3>`.
   - U-Boot's `rockchip-io-domain` driver probes right after bind (`DM_FLAG_PROBE_AFTER_BIND`) and writes the
     voltage selects from the tree.
   - Linux fixes it when its io-domain driver probes at 0.93 s. Read on the handheld afterwards: PMU GRF
     `IO_VSEL0` = `0x48`, `IO_VSEL1` = `0xB7`, so `vccio4` is in 3.3 V mode.
   - So every boot spends about 3 s with those pins (by the trees, the Wi-Fi SDIO side) mismatched. Rockchip's
     hardware guide says the domain setting must match its supply. Banana's U-Boot has done the same since
     day one; no harm is known, but it should not stay.
2. **The SD card's power switch has the opposite polarity.** (Wrong, 2026-11-23: a `regulator-fixed` takes its
   polarity from `enable-active-high`, not from the GPIO flag, in Linux and U-Boot alike, so both trees drive the
   pin the same way. The slow read had another cause; see "The card clock" below.)
   - The pin is the same, GPIO0_A5: active-low on Quartz64, active-high on the RG DS.
   - RG DS marks it always-on; Quartz64 doesn't.
   - A UHS start-up power-cycles the card, so this fits UHS never engaging (the 2026-10-27 UHS try changed
     nothing) and the ~11 MB/s read: plain 50 MHz high speed. The SD pins' own domain (`vccio3` = `vccio_sd`,
     RK817 LDO5) is wired the same in both trees.
3. **The CPU runs at 816 MHz in U-Boot** (`APLL_HZ` in `cru_rk3568.h`), which fits LZ4 at ~45 MB/s. 1104 MHz is
   the highest step the OPP table allows at 0.9 V, but U-Boot's `vdd_cpu` at boot is unverified, so no change
   without a measurement.

The RK817 rails themselves match: every fixed voltage is the same, and only `vdd_logic`/`vdd_gpu` lower limits
and LDO8's upper limit differ, which `regulator_autoset` does not drive to.

**The fix:** U-Boot built with the RG DS tree, either imported from Linux 7.2.7 into U-Boot's `dts/` or as a
`-u-boot.dtsi` override as a first step (`vccio4-supply = <&vcc_3v3>`, `vcc3v3_sd` active-high and always-on). Then
measure the card read with bootstage, and whether UHS SDR104 engages (expected ~0.6 s off the read).

**Board-by-board, not one tree.** The RG DS Plus is wired differently:

| Domain | Quartz64 tree (U-Boot) | RG DS | RG DS Plus |
| --- | --- | --- | --- |
| `vccio4` | 1.8 V | 3.3 V | 1.8 V (`vcc1v8_dvp`) |
| `vccio7` | 3.3 V | 3.3 V | 1.8 V |

One card and one U-Boot serve both, so a fixed tree can only be right for one of them.

**Trial build, 2026-11-20** (`build/os/uboot-trial-20261120/`, not written yet):
- `uboot-patches/005` removes `vccio4-supply` and `vccio7-supply` from the I/O-domain node, so U-Boot's driver
  leaves those two alone.
- `preboot` sets them per handheld once the SARADC has named it (RG DS 3.3/3.3 V, Plus 1.8/1.8 V), in the same
  order as Linux's RK3568 code: `IO_VSEL0` at `0xfdc20140`, then `IO_VSEL1` at `0xfdc20144`. A failed ADC read
  falls back to the RG DS values, which on a Plus means 3.3 V mode on a 1.8 V rail: the direction that doesn't
  overvolt the pins, and what `vccio7` gets today anyway.
- `preboot` reads both registers back into `rgds_vsel0`/`rgds_vsel1` (`CONFIG_CMD_SETEXPR`), so a test boot can pass
  them on the kernel command line.
- Built in a RAM copy of `uboot-custom`. Only U-Boot proper's DT and environment changed (the SPL differs by its
  date string), and the DDR blob and BL31 are the same bytes. 9,245,696 bytes; the first partition starts at sector
  32768, the image ends at 18122.
- Build traps: U-Boot wants `ARCH=arm`, and Buildroot's `host/bin/pkg-config` answers for the target sysroot unless
  `PKG_CONFIG_SYSROOT_DIR=/` and `PKG_CONFIG_LIBDIR=<host>/lib/pkgconfig:<host>/share/pkgconfig` are set (host tools
  then link against the handheld's libc).
- Not changed in this trial: the SD power-pin polarity and UHS. U-Boot's `dw_mmc` has no tuning routine, so SDR104
  can't run there at all; DDR50, which needs no tuning, is the speed experiment for a later write.

**On the handheld, 2026-11-20 (owner's OK).**
- Backup on the card: `/data/rgds/logs/u-boot-before-iodomains-1795174512.bin`. Same as the cache's
  `u-boot-rockchip.bin` (`4e7e…`) and `build/os/uboot-trial-20261120/u-boot-before.bin`.
- First write, with the readback: it boots. `rgds.detected=rgds`, `rgds.adc=1066275`, and `rgds.vsel=40,bf`
  (`IO_VSEL0` 0x40, `IO_VSEL1` 0xbf before the kernel), so `vccio4` and `vccio7` are in 3.3 V mode. With the old
  U-Boot, `vccio4` would have been in 1.8 V mode. Linux then leaves `0x48`/`0xb7`, the difference being `vccio3`
  (the card) switching to 1.8 V for UHS.
- That build cost **+0.16 s** from `bootm_start` to `start_kernel` (0.757 vs 0.599 s, three boots), with the same
  kernel and DTBs on slot b (hashes checked) and only `PREBOOT`/`CMD_SETEXPR` differing in `.config`.
- Without `setexpr` and the readback (`u-boot-iodomains-noreadback.bin`, `a645…`, 9,242,624 bytes) it is back to
  0.599 s. `start_kernel` 3.074 s against 3.091 s before (two boots). That is the version in the repo and on the
  card. The cause of the +0.16 s, `setexpr` itself or the image layout moving, is not pinned down.
- Screens, touch, joypad, sound and Wi-Fi all fine (SDIO in SDR50, on the `vccio4` side).
- In the repo: `uboot-patches/005-rgds-io-domains-per-board.patch` and the `PREBOOT` in `uboot-rgds.fragment`.
  Buildroot's U-Boot was rebuilt (`uboot-dirclean uboot`) and differs from the tested image only in build-date
  strings and what depends on them.
- **Who gets it:** OS updates never write U-Boot. Only newly flashed cards (`rgds-os.img`) and
  `own_os_push.py --uboot` carry it. **The Plus branch has not run on a Plus**, so try it on one before a release
  card goes out (the readback variant can show it).

U-Boot sits at 32K on the card and serves **both** slots, so a broken one means the card has to be rewritten on
the Mac. Keep the running U-Boot's backup (`own_os_push.py --uboot` writes one to `/data/rgds/logs/`) and the
Mac-side `dd` command ready before the first write. Needs the owner's OK.
- `configs/anbernic-rg-ds-rk3568_defconfig` + a board DT (U-Boot's `dts/upstream` already syncs mainline's) so the
  Quartz64 defconfig and patch `001-fix-defconfig` go away; keep bootcount, altbootcmd, SARADC board ID.
- Measure kernel-jump time with bootstage.

#### The card clock and the unpack, 2026-11-23 (owner's yes; on the card)

Measured with U-Boot's bootstage marks (`/proc/device-tree/bootstage`, `/data/rgds/kbench/stages.sh` on the
handheld), three boots per build, kernel #17 on slot b:

| U-Boot | Kernel read (`main_loop` → `bootm_start`) | LZ4 unpack (→ `start_kernel`) | Kernel starts |
| --- | --- | --- | --- |
| 0.9.x (I/O-domain fix, `a645…`) | 1.193 s | 0.444 s | 2.584 s |
| without `sd-uhs-sdr104` in U-Boot's tree | 1.193 s | 0.444 s | 2.584 s |
| + `uboot-patches/006` (card clock) | 0.682 s | 0.558 s | 2.189 s |
| + `uboot-patches/007` (LZ4 at -O2, aligned) | **0.682 s** | **0.409 s** | **2.039 s (−0.55 s)** |

How it was found:
- Removing SDR104 changed nothing. U-Boot's Rockchip dw_mmc never calls `mmc_of_parse`, so `sd-uhs-*` in the tree
  is never read. That trial U-Boot is not kept.
- A diagnostic U-Boot whose `preboot` did a raw `mmc read` of 10 MiB added 0.872 s to the boot: **12.0 MB/s, the
  limit of a 25 MHz bus**, so the filesystem was not the problem (it adds ~0.32 s over the raw read).
- A second diagnostic passed registers to Linux on the boot line (`setexpr.l` → `rgds.mmc=`): CRU CLKSEL_CON30 =
  `0x5420` (the card clock's source at 50 MHz), the dw_mmc's CLKDIV 0 (bypass), CTYPE 4-bit, UHS_REG 0. U-Boot had
  picked high speed. But the controller's CLKGEN halves its input before CLKDIV, which is why Linux's dw_mmc-rockchip
  asks for twice the clock (`RK3288_CLKGEN_DIV`). U-Boot asks for the card clock itself, so the card ran at 25 MHz.
  **`006`** asks the CRU for twice the clock, falling back to the old request where the CRU has no such rate (the
  400 kHz identification clock). It speeds up the SPL's read of U-Boot too.
- With `006` the unpack got 0.11 s slower although its code did not change: `ulz4fn` moved from `0x857050` to
  `0x857074`. U-Boot builds `lib/` with `-Os`, under which GCC ignores alignment flags. That is also what the
  2026-11-20 readback build's +0.16 s most likely was. **`007`** builds `lz4_wrapper.o` with `-O2` and 64-byte
  function / 32-byte loop alignment: 0.409 s, now independent of the code before it.

Where it stands:
- The tested U-Boot (`886304c3…`) is on the card. Buildroot's U-Boot was rebuilt from the repo (`uboot-dirclean
  uboot`): `ulz4fn` at the same `0x857000`, differing from the tested image by 101 bytes, all build-date strings.
  New cards (`rgds-os.img`) get it from the next full image build. OTA updates never write U-Boot.
- Backups on the handheld: `/data/rgds/logs/u-boot-before-nosdr104-1795444243.bin` (= `a645…`, the 0.9.x U-Boot,
  also on the Mac as `build/os/uboot-trial-20261120/u-boot-iodomains-noreadback.bin`). Restoring from the Mac with
  the card in a reader: `dd` it to the card at 32 KiB (`seek=64` in 512-byte blocks).
- **Not tried on a Plus.** It has the same SoC and controller, so it should get the same gain; check it with the
  Plus's I/O-domain branch.
- Next in this step: U-Boot's CPU runs 816 MHz on the SYR827's power-on 850 mV (the kernel's log shows `vdd_cpu ...
  at 850 mV` when it first registers it; U-Boot has no FAN53555 driver enabled). Mainline's table puts 408–1104 MHz
  at 900 mV, so a faster unpack needs U-Boot to set the regulator to 900 mV first (`DM_REGULATOR_FAN53555` and an
  RG DS node). Worth at most ~0.1 s of the 0.41 s unpack. The read is now ~15 MB/s against the 50 MHz bus's 25:
  the next suspects are U-Boot's FAT reads and the bounce buffer.

### Phase F: Upstream and tidy (ongoing)
- Send DT fixes and small driver patches upstream (kernel, U-Boot). Every one that lands is one less to carry.
- `THIRD_PARTY.md`: shrink the Banana/ROCKNIX entries to whatever is left; keep `LICENSE.BananaLinux` while any of
  their files remain.

## Decision gates
- B2 only if a lower memory clock measurably cuts menu/Saver current.
- After C: if the jadard panels blank where generic-dsi didn't, keep generic-dsi as *our* driver (copied, renamed,
  maintained) rather than stall the rest.
- After D: a config change that shows no measurable gain and no boot-time gain is reverted.

## Open questions for the owner
1. ~~OK to trial DDR at 1560 MHz?~~ Yes (2026-11-20), but moot: the RAM is LPDDR3, already at its ceiling.
2. Is open TF-A worth doing if it costs the current deep sleep?
3. Both handhelds (RG DS and Plus) in every phase, or RG DS first?
