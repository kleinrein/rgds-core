################################################################################
#
# libretro-bluemsx: the MSX and MSX2 core RG DS Play loads for .rom, .mx1, .mx2, .dsk and .cas games
#
################################################################################

LIBRETRO_BLUEMSX_VERSION = e3086eb5d36d77fa11704cf53dc176686e70127d
LIBRETRO_BLUEMSX_SITE = $(call github,libretro,blueMSX-libretro,$(LIBRETRO_BLUEMSX_VERSION))
LIBRETRO_BLUEMSX_LICENSE = BSD-like, GPL-2.0+ (openMSX parts), C-BIOS BSD-2-Clause
LIBRETRO_BLUEMSX_LICENSE_FILES = license.txt

# Only the C-BIOS machines: every other machine in the core's system folder names ROMs that are not free to give away.
LIBRETRO_BLUEMSX_MACHINES = MSX\ -\ C-BIOS MSX2\ -\ C-BIOS MSX2+\ -\ C-BIOS

define LIBRETRO_BLUEMSX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_BLUEMSX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/bluemsx_libretro.so $(TARGET_DIR)/usr/lib/libretro/bluemsx_libretro.so
	rm -rf $(TARGET_DIR)/usr/share/libretro/bluemsx
	mkdir -p $(TARGET_DIR)/usr/share/libretro/bluemsx/Machines
	cp -r $(@D)/system/bluemsx/Databases $(TARGET_DIR)/usr/share/libretro/bluemsx/
	cd $(@D)/system/bluemsx/Machines && \
		cp -r $(LIBRETRO_BLUEMSX_MACHINES) $(TARGET_DIR)/usr/share/libretro/bluemsx/Machines/
endef

$(eval $(generic-package))
