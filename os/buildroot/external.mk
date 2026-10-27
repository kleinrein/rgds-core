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
endef
LINUX_POST_PATCH_HOOKS += RGDS_LINUX_DRIVERS
