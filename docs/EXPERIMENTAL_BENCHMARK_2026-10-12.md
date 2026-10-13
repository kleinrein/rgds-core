# RG DS Experimental: device results, 12 October 2026

The capture-safe fast-forward change is deployed in RG DS Core release
`0.0.1-dev-bbbd2663b02b`. Select **Game options → Emulator → RG DS Experimental**.
The shipping fork uses its software renderer. Vulkan is a separate prototype;
there is no verified playable 5× mode yet. Zelda drawing is reserved for the user.

## Fork change and controlled comparison

Fast forward previously skipped presentation while still rasterizing every
frame. The fork now skips invisible raster work using the core's display-period
policy. CPU execution, geometry, guest timing and audio emulation continue.
Capture and display-FIFO frames render because their pixels can be read by the
guest. A second fix materializes the deferred 3D frame if capture becomes active
after the skip decision. `ff_skip=0` retains full rendering;
`DS_FF_RASTER=0` disables the new optimization for comparison.

The same candidate binary, ROM, checkpoint and default FF settings were used
with the optimization off and on, twice each. Each run had normal, fast-forward
and restored-normal phases of 15 seconds, an 8-second initial warmup and
2-second phase settling. Volume was muted; audio emulation remained enabled.
Existing user saves were not used or changed.

| Path | Repeat 1 emulated FPS | Repeat 2 emulated FPS | Mean |
| --- | ---: | ---: | ---: |
| Previous FF behavior | 151.57 | 151.77 | 151.67 |
| New capture-safe FF | 163.80 | 160.80 | 162.30 |

These rates are completed emulated frames divided by each phase's measured
wall time, not presentation refresh. The mean improvement is **7.0%** in this
New Super Mario Bros. checkpoint, approximately **2.7× normal DS speed**.
This is one short scene, not a library-wide speed claim. Runs were grouped off
then on rather than randomized; CPU temperature was 53–59°C. All sampled CPU
clocks used the existing 1.992 GHz performance profile.

Normal-phase sampled rates averaged 58.45–60.00 FPS across the four runs;
restored-normal rates averaged 59.82–60.00. All four runs acknowledged both FF
transitions, opened and resumed the shared menu, wrote a nonempty checkpoint,
and exited with code 0. This is functional smoke coverage, not a long stability
soak or audible playback acceptance.

Evidence in `build/experimental-device-20261012`:
`turbo-off.jsonl`, `turbo-on.jsonl` and their logs. The fork binary SHA-256 is
`92647a3a383c55b4642f79bdd77bb6cdeed8b829cf27d8c989666158e46a6c90`;
the source fingerprint is
`ff368bfd39c2b5dfc79b9a1de9581ff4ea370abc8a83a2c6139c98a391170605`.
Both paths used checkpoint SHA-256
`5248ec70c278e226bdd2924eace853b317938c5d998aaf4ff327d98ad4515fb8`.
Source, patches and build instructions accompany the engine in `source.tar.gz`.

## DSperate and NNDDSS comparison

An earlier two-repeat comparison disabled presentation skipping in DSperate
and the pre-optimization fork, and raised NNDDSS's speed setting to its supported
10× maximum. Each measured phase was 20 seconds. Compatible checkpoints were
reused per engine; the NNDDSS checkpoint format differs. Sampled emulated rates:

| Engine | Repeat 1 FPS | Repeat 2 FPS | FF rendering/audio policy |
| --- | ---: | ---: | --- |
| DSperate | 116.21 | 117.73 | All frames rendered/presented; audio emulated |
| RG DS Experimental before new FF | 116.74 | 118.90 | All frames rendered/presented; audio emulated |
| NNDDSS | 197.70 | 197.01 | Engine-managed rendering; engine mutes FF audio |

NNDDSS remains faster in this maximum-speed test. These policies perform
different work, so the table does not isolate CPU/core efficiency. In particular,
do not compare the new 162 FPS skipping result to the 117 FPS full-rendering
result as a claimed 39% optimization. An earlier NNDDSS test accidentally retained
its 2× speed cap and is excluded. Evidence: `max-speed-matched.jsonl`.

## Correctness and menu coverage

The new regression compares every byte of the captured 256×192×16-bit VRAM
image with and without the preceding raster skip. The full GPU suite passed
on both the ARM64 builder and the RG DS (`gpu-capture-arm64.log` and
`gpu-capture-device.log`). Four fork/input integration tests and 21 benchmark
harness tests passed. Earlier directed and randomized ARM64 JIT validation also
passed; these checks do not establish compatibility with every game.

SuperDrastic's shared RGDS menu was separately deployed and exercised on device:
three pause/resume cycles, slot save/overwrite/undo, load/undo-load, export and
save-on-exit succeeded. Emulated frames did not advance while the menu was open.
The light synthetic scene averaged 59.96 FPS in 3× Gengis mode. This is a separate
engine and does not imply that RG DS Experimental has inherited its 3× renderer.
Evidence: `shared-menu-device.jsonl` and `shared-menu.png`.

## Vulkan prototype

**Update, 13 October:** completed scaled renders, inspected pixels and a
controlled resolve-shader optimization are in the
[follow-up report](EXPERIMENTAL_BENCHMARK_2026-10-13.md). The text below records
the earlier interrupted run, not the latest status.

See [driver, renderer and reproduction notes](RGDS_VULKAN.md). A private PanVK
driver can enumerate the stock Mali-G52 and draw at 1×–3× when job stride is
explicitly set to 56 bytes. Its automatic stride selection failed. The light
128-polygon recording measured 2.08, 4.99 and 9.72 ms respectively for median
submit-and-wait time over 120 iterations, without MSAA. These figures include
the isolated renderer's upload/readback work, exclude device startup, and are
not full-game frame times. GPU clocks were not successfully captured for these
runs. Output images still need inspection.

The device connection dropped during 5× startup. No 5× or heavy-scene result is
claimed from that interrupted run. The cause (sleep, network loss or a driver
failure) has not yet been established. The private driver still reports a JIT
allocation warning and non-conformance. The shipping emulator and system
graphics libraries remain separate from this experiment.
