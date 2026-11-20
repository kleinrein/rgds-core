################################################################################
#
# libretro-cap32: the Amstrad CPC core RG DS Play loads for .dsk, .cdt, .sna, .cpr and .m3u games
#
################################################################################

LIBRETRO_CAP32_VERSION = 06ad0da5d003ab8f2359300ebce4cf31d4c55b6e
LIBRETRO_CAP32_SITE = $(call github,libretro,libretro-cap32,$(LIBRETRO_CAP32_VERSION))
LIBRETRO_CAP32_LICENSE = Caprice32 non-commercial (libretro port), GPL-2.0 (Caprice32)
LIBRETRO_CAP32_LICENSE_FILES = cap32/COPYING.txt

define LIBRETRO_CAP32_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_CAP32_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/cap32_libretro.so $(TARGET_DIR)/usr/lib/libretro/cap32_libretro.so
endef

$(eval $(generic-package))
