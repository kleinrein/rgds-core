include $(sort $(wildcard $(BR2_EXTERNAL_RGDS_PATH)/package/*/*.mk))

# The RG DS hardware layer comes from BananaLinux (MIT, LICENSE.BananaLinux), which takes it from ROCKNIX
# (board/rgds/ROCKNIX pins the commits): the generic DSI panel driver both panels use, the ADC joypad driver, the
# device trees with the panels retuned to 60 Hz, and ROCKNIX's kernel patches applied as ROCKNIX applies them
# (patch -p1 with fuzz: a few only fit that way, and Buildroot's own patch step refuses fuzz).
RGDS_BOARD = $(BR2_EXTERNAL_RGDS_PATH)/board/rgds
define RGDS_LINUX_ROCKNIX_PATCHES
	for p in $(sort $(wildcard $(RGDS_BOARD)/linux-patches/*.patch)); do \
		echo "Applying $$(basename $$p)"; \
		patch -p1 -N --no-backup-if-mismatch -d $(LINUX_DIR) < $$p || exit 1; \
	done
endef
LINUX_POST_PATCH_HOOKS += RGDS_LINUX_ROCKNIX_PATCHES
define RGDS_LINUX_DRIVERS
	cp $(RGDS_BOARD)/kernel/panel-generic-dsi.c $(LINUX_DIR)/drivers/gpu/drm/panel/
	grep -q panel-generic-dsi.o $(LINUX_DIR)/drivers/gpu/drm/panel/Makefile || \
		echo 'obj-$$(CONFIG_DRM_MIPI_DSI) += panel-generic-dsi.o' >> $(LINUX_DIR)/drivers/gpu/drm/panel/Makefile
	cp $(RGDS_BOARD)/kernel/rocknix-singleadc-joypad.c $(RGDS_BOARD)/kernel/rocknix-joypad.h $(LINUX_DIR)/drivers/input/joystick/
	grep -q rocknix-singleadc-joypad.o $(LINUX_DIR)/drivers/input/joystick/Makefile || \
		echo 'obj-$$(CONFIG_INPUT_JOYSTICK) += rocknix-singleadc-joypad.o' >> $(LINUX_DIR)/drivers/input/joystick/Makefile
	cp $(RGDS_BOARD)/dts/*.dts $(LINUX_DIR)/arch/arm64/boot/dts/rockchip/
	for p in $(sort $(wildcard $(RGDS_BOARD)/dts-patches/*.patch)); do \
		patch -p1 -N -d $(LINUX_DIR)/arch/arm64/boot/dts/rockchip < $$p || exit 1; \
	done
	for d in $(notdir $(basename $(wildcard $(RGDS_BOARD)/dts/*.dts))); do \
		grep -q "$$d.dtb" $(LINUX_DIR)/arch/arm64/boot/dts/rockchip/Makefile || \
			echo "dtb-\$$(CONFIG_ARCH_ROCKCHIP) += $$d.dtb" >> $(LINUX_DIR)/arch/arm64/boot/dts/rockchip/Makefile; \
	done
	$(RGDS_LINUX_SLEEP_CONFIG)
endef
LINUX_POST_PATCH_HOOKS += RGDS_LINUX_DRIVERS
# The drivers again before every kernel build: the post-patch hook runs once, at extraction, so a driver edited
# after that (panel-generic-dsi.c on 2026-10-27) would otherwise never reach the image. cp keeps make's view right.
# The Wi-Fi regulatory database the kernel carries (CONFIG_EXTRA_FIRMWARE, linux-rgds.fragment) comes along: the
# kernel reads it from firmware/ in its own tree.
define RGDS_LINUX_DRIVER_SOURCES
	cp $(RGDS_BOARD)/kernel/panel-generic-dsi.c $(LINUX_DIR)/drivers/gpu/drm/panel/
	cp $(RGDS_BOARD)/kernel/rocknix-singleadc-joypad.c $(RGDS_BOARD)/kernel/rocknix-joypad.h $(LINUX_DIR)/drivers/input/joystick/
	mkdir -p $(LINUX_DIR)/firmware
	cp $(RGDS_BOARD)/firmware/regulatory.db $(RGDS_BOARD)/firmware/regulatory.db.p7s $(LINUX_DIR)/firmware/
	$(RGDS_LINUX_SLEEP_CONFIG)
	$(RGDS_LINUX_DTS_REFRESH)
endef
LINUX_PRE_BUILD_HOOKS += RGDS_LINUX_DRIVER_SOURCES
# True sleep (docs/OWN_OS_PLAN.md "True sleep"): the driver that hands BL31 the sleep mode and wake sources for the
# "mem" state from the device tree's rockchip-suspend node (the vendor kernel's rockchip_pm_config, cut down), with
# the vendor's dt-bindings header the node uses. Built in: nothing of the suspend path is a module.
define RGDS_LINUX_SLEEP_CONFIG
	cp $(RGDS_BOARD)/kernel/rockchip_pm_config.c $(LINUX_DIR)/drivers/soc/rockchip/
	grep -q rockchip_pm_config.o $(LINUX_DIR)/drivers/soc/rockchip/Makefile || \
		echo 'obj-$$(CONFIG_ARCH_ROCKCHIP) += rockchip_pm_config.o' >> $(LINUX_DIR)/drivers/soc/rockchip/Makefile
	mkdir -p $(LINUX_DIR)/include/dt-bindings/suspend
	cp $(RGDS_BOARD)/kernel/rockchip-rk3568-suspend.h $(LINUX_DIR)/include/dt-bindings/suspend/rockchip-rk3568.h
endef
# The device trees again before every kernel build, like the drivers: board/rgds/dts copied afresh and the dts
# patches re-applied (a patch already in the tree, such as 0005 on rk356x-base.dtsi, is skipped; a patch that no
# longer fits fails the build).
define RGDS_LINUX_DTS_REFRESH
	cp $(RGDS_BOARD)/dts/*.dts $(LINUX_DIR)/arch/arm64/boot/dts/rockchip/
	for p in $(sort $(wildcard $(RGDS_BOARD)/dts-patches/*.patch)); do \
		out=$$(patch -p1 -N -r - -d $(LINUX_DIR)/arch/arm64/boot/dts/rockchip < $$p 2>&1); rc=$$?; \
		case "$$rc$$out" in 0*) ;; *"previously applied"*) ;; *) echo "$$(basename $$p): $$out"; exit 1;; esac; \
	done
endef
