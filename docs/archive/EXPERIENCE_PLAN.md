# RGDS development direction

Develop the launcher and engine integration on the working Anbernic Linux card.
Retain the stock kernel, bootloader and hardware services. The failed Armbian
prototype and flashing workflow are retired; no device deployment is authorized
in the current local work session.

1. Finish and verify local application changes, with DSperate as the default
   and DraStic, NNDDSS and melonDS available as alternatives.
2. At the next authorized hardware session, inspect shutdown scripts, free space
   and NNDDSS settings/logs before changing anything. Preserve saves and rollback.
3. Deploy one verified application package over SSH. Check screens, controls,
   touch, sound, native saves and actual checkpoint restoration in each engine.
4. Measure the same games/settings on RGDS, SimpleOS and ROCKNIX DS. Record game
   FPS separately from display FPS, menu latency, clocks, temperature and memory.
5. Test lid sleep/wake, battery/charging, Wi-Fi and shutdown on battery. Measure
   power-button-to-LED, Linux boot and launcher first frame separately.

The early Anbernic logo and power-off restart are unresolved. A passed local
build or VM test is not physical acceptance. See [next checks](../NEXT_DEVICE_TEST.md).
