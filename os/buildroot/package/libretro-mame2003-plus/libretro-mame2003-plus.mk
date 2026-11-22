################################################################################
#
# libretro-mame2003-plus: the arcade core RG DS Play loads for the sets in /data/roms/arcade, cut down by
# two_monitors.py to Nintendo's two-monitor cabinets (play/profiles/arcade puts one monitor on each panel)
#
################################################################################

LIBRETRO_MAME2003_PLUS_VERSION = 1bfe3c273ac2bb5ee3ca37e589127b83a8341305
LIBRETRO_MAME2003_PLUS_SITE = $(call github,libretro,mame2003-plus-libretro,$(LIBRETRO_MAME2003_PLUS_VERSION))
LIBRETRO_MAME2003_PLUS_LICENSE = MAME (non-commercial)
LIBRETRO_MAME2003_PLUS_LICENSE_FILES = LICENSE.md

define LIBRETRO_MAME2003_PLUS_TWO_MONITORS
	python3 $(LIBRETRO_MAME2003_PLUS_PKGDIR)/two_monitors.py $(@D)
endef
LIBRETRO_MAME2003_PLUS_POST_PATCH_HOOKS += LIBRETRO_MAME2003_PLUS_TWO_MONITORS

define LIBRETRO_MAME2003_PLUS_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CC)"
endef

define LIBRETRO_MAME2003_PLUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mame2003_plus_libretro.so $(TARGET_DIR)/usr/lib/libretro/mame2003_plus_libretro.so
endef

$(eval $(generic-package))
