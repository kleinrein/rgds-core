# Performance measurements

This guide separates emulated game speed, display presentation, launch/menu time and host integration costs. Results are historical measurements of identified development builds, not certification of the current checkout. No measured advantage over SimpleOS or ROCKNIXDS is established.

## Device gameplay: 11 October 2026

The standard RG DS ran Anbernic stock Linux with native resolution, shader Off, frameskip 0, muted audio and the performance power profile. The workload was New Super Mario Bros., World 1-1, with Mario stationary at the entrance. Each core used its own checkpoint of that scene; this is not a frame-identical cross-core replay.

After warmup/settling, each normal phase collected eight one-second samples. GAME rates below are sampled emulated frames/sec; display flips may repeat or skip game frames and cannot substitute for this metric.

| Core / configuration | Normal GAME FPS | Fast-forward GAME FPS | Restored normal GAME FPS |
| --- | ---: | ---: | ---: |
| DSperate, verified normal-only repeat 1 / 2 | 59.98 / 59.95 | Not measured in these repeats | Not measured in these repeats |
| NNDDSS, default pinned scheduling | 59.58 | 119.10 | 59.76 |
| melonDS, Threaded 3D / limited FF presentation | 44.84 | 45.01 | 45.86 |
| melonDS, Threaded 3D / present every frame | 46.40 | 46.33 | 46.31 |
| melonDS, Threaded 3D off | 41.71 | 40.86 | 40.64 |
| Standard DraStic | Unavailable | Unavailable | Unavailable |

DSperate used build `0.0.1-dev-d53f1de6ba74`; NNDDSS and the three melonDS variants used `0.0.1-dev-cc9dfeb78beb`. The [gameplay report](archive/GAMEPLAY_REVIEW_2026-10-11.md) retains exact ROM/checkpoint hashes, source settings, exclusions, captures and evidence filenames.

DSperate's repeats resumed the same verified checkpoint. Its sampled CPU use was 96.7% / 96.5% across all threads, where 100% means one occupied CPU rather than the whole four-core chip. CPU clock stayed at 1.992 GHz, GPU at 200 MHz; maximum sampled temperatures were 58.3 / 58.9 °C. Whole-run emulation-plus-presentation p95 was 9.231 / 9.300 ms, including startup, whereas the FPS row covers only the sampled normal phase.

NNDDSS and each melonDS configuration have one run here. NNDDSS's affinity opt-out did not show a convincing normal-speed gain. melonDS spent roughly 19–20 ms/frame in threaded core work versus 1.7–1.8 ms drawing, indicating an emulation bottleneck in this scene. The 1–2 FPS difference between presentation variants is not an established optimization: both stayed below the 60 Hz cap. Defaults were retained.

### Launch and menu time

Session-start-to-first-game-presentation in the same-scene tests was 4.195–4.375 s for the two DSperate repeats, 4.544 s for default NNDDSS, 4.716–4.930 s for the threaded melonDS variants and 3.602 s for Standard DraStic. These include launch/preparation; they are not physical button-to-photon measurements.

The earlier corrected four-core smoke test had three rendered-menu opens per core:

| Core | Median request → rendered-menu acknowledgement |
| --- | ---: |
| DSperate | 435 ms |
| NNDDSS | 399 ms |
| melonDS | 416 ms |
| Standard DraStic | 426 ms |

See the [device review](archive/DEVICE_REVIEW_2026-10-11.md) for the exact build and protocol. SuperDrastic's 21–27 ms native command acknowledgements are a different endpoint and cannot be compared as rendered latency. A later DSperate run required a two-second forced-close fallback on every menu resume. The October 12 candidate includes a resume fast path, but short successful cycles do not close every engine's teardown/physical interaction acceptance item.

### Fast-forward and soak boundaries

A later DSperate run measured 59.99 / 94.45 / 60.11 GAME FPS in normal/fast-forward/restored phases; an earlier profiled run reached 132.64 in fast-forward. That unexplained variation needs controlled repeats. Do not use these numbers to rank cores: each backend has a different configured speed policy.

An original lightweight VBlank-counter ROM measured SuperDrastic at 59.93 / 785.27 / 59.97 emulated FPS. The approximately 13.1× acceleration belongs to this synthetic workload; it is not a retail-game throughput claim. See [the probe method](SUPERDRASTIC.md#actual-emulation-speed-check--2026-10-11).

On October 12, Cooking Mama/DSperate completed a 600-second startup/title-screen phase, three pause/resume cycles and save-on-exit. Display presentation averaged 60.00 FPS; intervals were 16.7 ms p50, 17.1 ms p95 and 18.1 ms p99. Maximum sampled SoC temperature was 65 °C. The ALSA kernel queue's 84.833 ms median measures buffering rather than end-to-end audio latency. See [the validation report](archive/GAP_CLOSURE_2026-10-12.md).

## Why there is no broad benchmark claim

These checks establish specific scene speed, counter behavior and adapter flows. They do not establish full-library compatibility, sustained gameplay, battery life, physical input/audio latency or superiority to another OS. No competitor was measured. Muted runs cannot certify audible quality. A presentation counter cannot prove emulation speed.

A publishable comparison needs repeated runs from fixed per-core checkpoints with recorded input, matching ROM/settings/power/thermal conditions, measured emulated frames, demanding gameplay and explicit sample counts/variation. Match fast-forward policies before comparing throughput. Keep physical latency and battery testing separate. See [remaining device acceptance](NEXT_DEVICE_TEST.md).

## Reproduce device measurements

Build/package and install the candidate, then stop the development watcher and exit any current RG DS Core session. The runner refuses an existing session and creates disposable test profiles/ROM copies; it does not use live saves as writable test storage. Substitute your actual SSH address, installation root and existing device ROM path:

```sh
python3 tools/benchmark_emulators.py \
  --device root@<device-IP> \
  --root /actual/installation/rgds \
  --rom '/actual/ROM/folder/game.nds' \
  --cores dsperate nnddss melonds drastic \
  --phase normal --seconds 30 --repeats 3 --menu-cycles 3 \
  --output build/performance/device-current.jsonl
```

A cold launch without a verified checkpoint is a smoke test, not a fixed-scene gameplay result. Prepare a disposable per-core checkpoint and use `--resume-from /remote/benchmark/results.json` to seed later runs from matching ROM bytes. Check engine logs/captures for actual restoration, not just a seed hash. `--capture-scene` captures supported renderers before settling; `--feature-checks` exercises save/DS-control flows, and `--checkpoints` enables save-on-exit. Use `--help` for threading, profiling and phase options. The runner records build/ROM hashes, actual phase boundaries, clocks, temperatures and engine logs.

## Host integration microbenchmarks

These October 7–8 measurements used the development host, temporary fixtures and software SDL/dummy rendering. They measure elapsed integration work rather than process CPU, device FPS or total boot time.

| Host workload | Before | After |
| --- | ---: | ---: |
| 30 simulated seconds of bridge/input queries | 30.38 ms | 9.04 ms |
| 200 checkpoint checks with 1,000 states in the directory | 782.71 ms | 0.62 ms |
| 180 top-screen volume HUD presentations | 50.23 ms | 27.62 ms |
| 180 opaque menu presentations | 154.75 ms | 82.53 ms |
| 5,000 metadata iterations, three INIs each | 250.5 ms; 15,000 reads | 1.05 ms; zero warm reads |
| Startup validation, 30 alternating samples | 35.771 ms median | 9.514 ms median |

The validation workload was an 80-file, 32.9 MB release, with 11.97 MB selected for startup and three warmups. Source hashes/raw samples remain in local `build/performance/`. A separate 16-launch native first-frame test stayed around 18–20 ms with mixed before/after variation; it did not demonstrate a large timing gain. Stage timings overlap and must not be added into a device boot estimate.

A 120-frame launcher fixture read four metadata records and one ROM banner in total, with no library asset reads/stat checks inside drawing. Background preparation and bounded caches remove repeated file work from animation; they do not make cold loads free. The texture cache is bounded to 64 entries / 16 MiB of RGBA data.

```sh
python3 tools/build.py host
python3 tools/benchmark_bridge.py --output build/performance/bridge-current.json
python3 tools/benchmark_launcher_startup.py --iterations 16 \
  --output build/performance/launcher-startup-current.json
python3 tools/benchmark_boot.py --app /path/to/complete/release \
  --output build/performance/boot-validation-current.json
python3 tools/benchmark_runtime_idle.py --nnddss-app /path/to/complete/release \
  --output build/performance/runtime-current.json
```

`benchmark_bridge.py --source-root /path/to/baseline` permits an older source snapshot under the same compiler/SDL. Retain exact inputs and raw before/after samples when publishing a new comparison. The historical baseline JSON and device logs are ignored local build outputs, not files present in a fresh clone.

## Implemented optimizations

The launcher uses background library workers and bounded metadata/banner/texture caches. The bridge caches unchanged control files and shader programs/uniforms, renders shaders on native screen textures and avoids routine framebuffer readbacks. DSperate uses its pinned ARM64 JIT/NEON build with Cortex-A55 tuning and LTO; emulator logs drain into a bounded 128 KiB RAM tail instead of continuous card writes. These describe code paths. Their existence alone is not evidence of device speed, energy savings or improved timing accuracy.
