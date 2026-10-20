# RG DS Core upgrade — 2026-10-12

The launcher now uses the RG DS Core name and image-generated dual-screen
brand mark. Original art and its exact prompt are in `assets/brand/README.md`.
The status bar, early boot artwork and animated launcher loading scene share
the identity. The loader follows real library readiness without a minimum
display delay. The bootloader/kernel stage still displays static artwork;
the measured pre-update first launcher frame was about 21 seconds after boot.

Home and game cartridges share sizing, category icons use the native icon set,
and both carousels share a horizontal rail that reaches the bottom and side
edges. Rounded selection, raised buttons and visible Back press/release
feedback replace inconsistent square highlights. Settings has a dedicated
last Power off entry with confirmation.

English (US) and Norsk bokmål share catalogs across settings, library and
in-game screens. See `I18N.md` for adding languages. Large Favorites/Recents
collections filter in a cancellable worker; A–Z jumps avoid synchronous
per-ROM banner reads. Existing bounded artwork and viewport caches remain.

Opening the in-game menu requests one bounded screenshot before pausing.
The upper screen shows a faded top-screen still and the game title. Engines
that do not deliver a capture within 180 ms open without a backdrop. Save/exit
uses a dedicated animated overlay, and existing checkpoint verification,
failure recovery and explicit exit-without-save semantics remain intact.

The verified boot-branding updater retains vendor originals and archives the
previous branding record before installing new artwork. It changes known
logo bytes and userspace images; kernel and device-tree content stay intact.

Validation: native host and Linux AArch64 builds passed, as did 195 targeted
UI/runtime/bridge/integration tests. Branding tests cover original retention,
readback, restoration and rejection of external changes. The earlier full
suite exposed packaging fixture/source-database and missing Wayland utility
errors outside this pass; it is not recorded as fully green. Light/dark,
Bokmål, boot, library and menu screenshots were inspected at 640×960.

Installed on the standard RG DS at the handheld as
`0.0.1-dev-b44b4615b8d6` (initial visual deployment was
`0.0.1-dev-c04707f769fb`). Both physical displays rendered the new launcher
under the device's Wayland/GLES2 runtime, and the live screenshots were
inspected. Boot-artwork readback verified installed SHA-256
`fdf2424cec9b608e4487a1b7bff0f2fa9f0ac61632603eace762b567e2e37aaa`.
The prior branding is retained at
`/userdata/rgds-boot/branding.previous-d91af3f328519872`; the current branding
store also retains the original vendor image. Cold boot and physical buttons
still require owner acceptance.

Power diagnosis remains open. The owner reports that a held first Power press
produces no LED or image and a second held press starts the device. Read-only
RK817 diagnostics at the handheld showed saved rail registers 0x99 and 0xa4
both zero: the previously repaired stale-state fault is absent. No speculative
power-controller, bootloader or kernel change was made. This requires a
physical cold-start reproduction and earlier power/serial evidence.

The hardware pause test exposed a vendor Wayland/EGL teardown delay of one
second per menu window. The managed direct menu now finishes pending preference
and hardware writes, reaps its workers, then exits without traversing that GPU
teardown path. Process exit releases its display connection and GPU resources;
the session keeps game input blocked until the helper is reaped. Desktop,
embedded-overlay and long-lived launcher cleanup keep their normal lifecycle.
The change passed 79 targeted runtime/control tests plus 15 tests against the
rebuilt host UI, including screenshot handoff and language persistence.

Final device checks used the original silent RGDS Probe ROM in disposable
profiles, with two pause/resume cycles per engine. DSperate and NNDDSS each
captured a fresh 640×480 backdrop, presented the menu in 0.54–0.63 seconds,
resumed in 0.14–0.15 seconds (previously 2.09 seconds), and saved a nonempty
checkpoint before a clean exit. Neither helper needed the timeout/kill
fallback. These checks exercise both SDL-renderer and GL capture paths;
they are not a broad commercial-game compatibility or large-library benchmark.
Existing user saves were not loaded or modified. Evidence is retained locally
under `build/core-upgrade/`, including live screenshots, trace and test logs.
