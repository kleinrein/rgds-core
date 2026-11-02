################################################################################
#
# libretro-vice: VICE's fast C64 (x64), the Commodore 64 core RG DS Play loads (x64sc, the cycle-exact one, is
# too slow for the RG DS's A55s)
#
################################################################################

LIBRETRO_VICE_VERSION = 5ebe7d333ab15be57a9dde5f46c29a5bbd5f44f8
LIBRETRO_VICE_SITE = $(call github,libretro,vice-libretro,$(LIBRETRO_VICE_VERSION))
LIBRETRO_VICE_LICENSE = GPL-2.0
LIBRETRO_VICE_LICENSE_FILES = vice/COPYING

define LIBRETRO_VICE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile EMUTYPE=x64 platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_VICE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/vice_x64_libretro.so $(TARGET_DIR)/usr/lib/libretro/vice_x64_libretro.so
endef

$(eval $(generic-package))
