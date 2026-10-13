# DraStic add-ons and settings review — 2026-10-11

**Follow-up:** [SuperDrastic is now integrated as an optional local candidate](../SUPERDRASTIC.md).
The review below records the earlier assessment before implementation. Advanced
DraStic remains unbundled; hardware acceptance is still pending.

Advanced DraStic and SuperDrastic enhance the closed Linux DraStic engine; they
are not independent DS cores. Neither is bundled or enabled by this review.
DSperate remains the recommended default, based on the owner's standard RG DS
results. Existing saved choices are preserved. No handheld deployment or test
was performed.

## Pinned upstream review

| Project | Reviewed commit | Result |
| --- | --- | --- |
| [SuperDrastic](https://github.com/JorreFog/SuperDrastic/tree/930c4c5bb3e904557cd8cc42a01a4e27834f5ba1) | `930c4c5bb3e904557cd8cc42a01a4e27834f5ba1` | Strongest candidate for a separate DraStic backend experiment. MIT source, direct display output, frame pacing, audio work and an optional rasterizer. The project documents regular RG DS testing on ROCKNIX; stock Anbernic acceptance is not established. |
| [Advanced DraStic](https://github.com/trngaje/advanced_drastic/tree/d5a16e027ba28c7f5edf61ca9ba0f69aa3d93cf4) | `d5a16e027ba28c7f5edf61ca9ba0f69aa3d93cf4` | Useful layout/input/settings reference. This snapshot publishes prebuilt `libadvdrastic.so` variants, including ROCKNIX, but its recursive tree contains no C/C++ sources or license/notice file. Source availability and redistribution terms need resolving before importing it. |

The selected source/reference files and their SHA-256 hashes are retained in
`build/drastic-settings-review-20261011/upstream/reviewed-files.json`. No upstream
installer was executed; no additional emulator binaries were downloaded.

The private local DraStic r2.5.2.2 binary's MD5 is
`17550db727f3b59d36b57746ad1944be`, matching Advanced DraStic's documented version.
Its SHA-256 is
`dab3275ef772451b9235b78e64b2d0d041a28ac22937668231088c76b709e218`.
The [recorded stock baseline](DEVICE_BASELINE_2026-10-09.md) reports glibc 2.41,
above SuperDrastic's stated 2.38 minimum. Our own application's glibc 2.17 target
is not the installed OS version. These checks make an experiment credible;
they do not establish full dependency closure or rendering compatibility.

## Why SuperDrastic needs a separate backend

Its [integration contract](https://github.com/JorreFog/SuperDrastic/blob/930c4c5bb3e904557cd8cc42a01a4e27834f5ba1/docs/INTEGRATION.md)
requires exclusive DRM display ownership and SDL's dummy video backend. It
also has its own input/menu, resume protocol and clock management. In RGDS,
the stock Wayland session, RGDS bridge and game supervisor currently own those
responsibilities. Loading both bridges together would create competing owners.

The [runner](https://github.com/JorreFog/SuperDrastic/blob/930c4c5bb3e904557cd8cc42a01a4e27834f5ba1/runner/superdrastic-run)
automatically chooses a VT handoff when Weston is present. Merely finding Weston
does not prove stock releases DRM correctly on that switch. An isolated trial
must explicitly test display release and restoration, including failure paths.
Use its readiness verdict, isolate configuration and logs, retain existing
saves, and give clock/menu/exit handling one owner. Do not enable an automatic
updater or replace the default as part of a trial.

The optional [rasterizer entry point](https://github.com/JorreFog/SuperDrastic/blob/930c4c5bb3e904557cd8cc42a01a4e27834f5ba1/src/rast/rast.c)
targets DraStic's high-resolution mode and patches native functions after
checking expected instructions. It is disabled
unless explicitly requested. This is a candidate for exact-binary validation
and frame comparisons, not a general-purpose acceleration flag. Upstream speed
claims are not measurements of RGDS on this handheld.

Acceptance must cover both panels, touch, controls, sound/mic, normal saves and
checkpoint restoration, menu pause/resume, lid sleep, return-to-launcher and
failed-start recovery. Compare the same games, resolution, shader and power
profile against current DraStic and DSperate before changing recommendations.

## Settings changes in this candidate

- Emulator pickers label DSperate recommended and melonDS experimental. Focused
  choices explain the trade-off without changing the saved selection.
- The running emulator's name stays visible above its settings. **Next emulator**
  has the same **Use default** option as the library's per-game editor. Restart
  guidance explains that saves and checkpoints remain separate per emulator.
- Unsupported settings cannot open misleading pickers: notably melonDS
  resolution/frameskip, DSperate resolution and NNDDSS frameskip.
- Resolution and frameskip values are selected before applying. Reapplying the
  current emulator does not rewrite its configuration. Frameskip defaults to Off.
- Game preference changes reach the UI/cache only after a successful file save;
  failure keeps previous values and displays an error. Global settings reload
  the stored files on failure, including a partial base/UI preference save.
- Live controls and restart-only controls have different guidance. DraStic still
  requires restart for Threaded 3D; melonDS has a live switch. The preview now
  reflects fast-forward toggles correctly.

Defaults remain native resolution, no shader, no frameskip, automatic game
pause in the RGDS menu and the stock power policy. Maximum clocks and untested
rendering hooks are not silently enabled. See [the device checklist](../NEXT_DEVICE_TEST.md)
for the remaining physical checks.

## Local validation

- 27 emulator/settings interaction checks passed, including real SDL button and
  touch input, unsupported controls, cancellation, inherited defaults and failed
  file writes. Another 87 settings/menu/control/shader checks passed. The
  runtime/packaging group passed 45 checks, including one overlapping idle UI
  check; these counts are test executions, not unique test coverage.
- Eight rendered screens were inspected: recommended/default emulator choices,
  default inheritance, disabled controls and frameskip, in rounded/light and
  pixel/dark modes. All eight reported zero filesystem reads or stamp checks
  during rendering. Their screenshots/reports are under the local review's
  `screens/` directory.
- Host and AArch64 builds passed. The launcher and unchanged bridge retain the
  glibc 2.17 target. Packaging validates source-build records and engine payloads;
  desktop Finder metadata is now excluded from the release.

These checks validate the local settings changes. They do not establish
SuperDrastic compatibility, faster retail gameplay, or physical power/sleep
behavior. Those require the next authorized handheld session.
