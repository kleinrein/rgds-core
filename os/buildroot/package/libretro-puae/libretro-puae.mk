################################################################################
#
# libretro-puae: the Commodore Amiga core RG DS Play loads for .adf, .ipf, .hdf, .lha and .uae games
#
################################################################################

LIBRETRO_PUAE_VERSION = d366f77c60e5a5f059f129cc3773d3048306083b
LIBRETRO_PUAE_SITE = $(call github,libretro,libretro-uae,$(LIBRETRO_PUAE_VERSION))
LIBRETRO_PUAE_LICENSE = GPL-2.0
LIBRETRO_PUAE_LICENSE_FILES = COPYING

define LIBRETRO_PUAE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_PUAE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/puae_libretro.so $(TARGET_DIR)/usr/lib/libretro/puae_libretro.so
endef

$(eval $(generic-package))
