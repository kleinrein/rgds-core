# U-Boot boot splash (shelved 2026-10-28)

Built and run on the handheld, then dropped by the owner to keep the boot fast. Not applied by Buildroot: the
patch is outside `uboot-patches/`. Measured: the splash cost 464 ms in U-Boot (kernel jump at 3.73 s instead of
3.07 s) for about 2 s of emblem on both screens, which then went dark when the kernel reset the panels, until
the kernel logo. Details and what a handover would need: docs/OWN_OS_PLAN.md, "U-Boot splash (2026-10-28)".

To try it again:
- move `004-rgds-boot-splash.patch` back to `board/rgds/uboot-patches/`;
- in `uboot-rgds.fragment`: prefix `CONFIG_BOOTCOMMAND` with `rgds_splash; ` and add `CONFIG_VIDEO=y`,
  `CONFIG_VIDEO_LOGO=n`, `CONFIG_VIDEO_ROCKCHIP=y`, `CONFIG_DISPLAY_ROCKCHIP_DW_MIPI=y`, `CONFIG_VIDEO_BRIDGE=y`,
  `CONFIG_VIDEO_ROCKCHIP_VOP2_RGDS=y`, `CONFIG_PANEL=y`, `CONFIG_BACKLIGHT=y`, `CONFIG_BACKLIGHT_PWM=y`,
  `CONFIG_VIDEO_LCD_GENERIC_DSI=y`, `CONFIG_PHY_ROCKCHIP_INNO_DSIDPHY=y`;
- put `/splash.bmp` on the boot partition (`ppm2bmp.py boot-logo.ppm splash.bmp`, from post-image.sh plus a
  `file splash.bmp` line in genimage.cfg.in);
- for the fail-safe, rcS prefixes `/boot/splash.log` with `ok` after mounting /boot (U-Boot skips the splash while
  it says `try` or `lit`).
