# RG DS Plus community preview

RG DS Plus compatibility is experimental. This work was done without a Plus unit; simulated panels and input fixtures cannot validate its stock firmware, drivers or game performance.

Anbernic's [official specifications](https://win.anbernic.com/product/763.html) list the same RK3568 Cortex-A55 processor, two 1024×768 IPS panels, lower-panel multitouch/stylus input, 1 GB RAM, Linux on a TF card and a lid Hall switch. The [product page](https://anbernic.com/products/rg-ds-plus) also shows its single analog stick. These are the hardware facts used for this compatibility pass.

## Implemented

- The launcher requests each panel's native dimensions and scales its 640×480 logical UI to fit. Larger panels use filtered UI textures; touch and hit areas remain in logical coordinates. The shared in-game menu also stays at its bounded logical resolution and scales through the existing overlay.
- melonDS requests native panel sizes while keeping its 256×192 game textures. DSperate already derives game scaling from its fullscreen output dimensions. Both can use 4× DS scaling on a 1024×768 panel.
- Stock sessions identify Plus board aliases from the device tree, or infer a Plus profile when both connected DSI panels advertise 1024×768. The inference is a compatibility hint, not firmware certification.
- Capability discovery selects the single lower touchscreen and the console controller even when event numbers change. The existing raw decoder reads calibration ranges from evdev, including 1024×768 coordinates. Multiple unresolved touch candidates use SDL fallback in the launcher; NNDDSS refuses unresolved raw input instead of assuming event1/event4.
- The bridge resolves the Plus's controller GUID at runtime for recognized console names and selects DSperate's displays by their actual SDL indices. Button semantics still assume the existing RK3568 stock layout and need verification on the Plus. No action requires a right analog stick.
- The launcher retains its 16 MiB artwork texture budget and the shared menu retains its 640×480 buffers. Higher panel resolution does not multiply these caches. The inventory now includes board identification and memory totals.
- Independent Linux hardware profiles accept `model: "rg-ds-plus"`; this does not make a standard-model OS image compatible with the Plus.

## First test on hardware

Use the **Plus's own official stock Linux card**, with a backup of saves. This is an application update; use the normal installer ZIP at the games-card root. Do not flash the original RG DS image onto the Plus.

1. Before installation, run the bundled **RGDS Inventory** entry if available. Otherwise install, then run **APPS → RGDS Inventory**. Keep `rgds/userdata/inventory.txt` and note the stock firmware version.
2. Run **APPS → RGDS Probe**. Verify top/bottom labels, a full-screen picture on both panels and a crosshair under the stylus at the center and all four corners. Check D-pad, A/B/X/Y, Start/Select, both shoulder pairs, the analog stick and the Anbernic button. The probe returns to stock after 90 seconds or B.
3. Open **RG DS Core**. Check library scrolling, settings, brightness, volume and return to stock. `rgds/userdata/launch.log` records the detected profile and selected input nodes; `rgds/userdata/session.log` records SDL panel dimensions and launcher errors.
4. Try a DS game in **DSCore** (the default) at **Native** first. Check both screens, stylus input, audio, the shared pause menu, checkpoint save/load and return to the library. Confirm an ordinary in-game save survives relaunch. Then try **In-game menu → Graphics → 3D resolution → 2×**: the Plus has the same GPU but only 1 GB of RAM, so note the frame rate (turn on **Game FPS** in the in-game menu's emulator options) and anything odd. In heavy scenes 2× briefly drops to native resolution to hold 60 fps (adaptive 2×): a picture that softens for a few seconds is expected; report scenes where it stays soft or the frame rate still falls. Test other engines individually afterward.
5. Check lid close/open and sleep/wake, then an ordinary shutdown and cold boot. Direct startup uses the existing stock hook only if that firmware exposes it. Keep the recovery APPS entry available while testing.
6. Report the firmware version, `inventory.txt`, `launch.log`, `session.log` and which checklist step failed. A photo of the two panels is useful for display or scaling faults. Inventory contains network interface addresses; review it before posting publicly.

## Still unverified

Controller button indices, display order, stock supervisor/hook compatibility, audio/microphone routing, wake events, NNDDSS's supplied loader and DraStic's private touch adapter all need a real Plus test. SuperDrastic remains an experimental backend tied to an exact DraStic build; start with DSCore. The original controller fallback is retained for the original model.

OTA still requires the previously validated stock Buildroot version and 640×480 panels. Use the complete SD installer for manual Plus application updates until a Plus firmware profile is verified. Early KMS splash and boot-image/logo tooling retain their original-model guards; this pass does not patch Plus boot partitions. The launcher loading animation works through the scaled UI.
