################################################################################
#
# libretro-gearcoleco: the ColecoVision core RG DS Play loads for .col games
#
################################################################################

# Release 1.8.0
LIBRETRO_GEARCOLECO_VERSION = 07e2c057767315b8395606b77951c8dd6141c5ce
LIBRETRO_GEARCOLECO_SITE = $(call github,drhelius,Gearcoleco,$(LIBRETRO_GEARCOLECO_VERSION))
LIBRETRO_GEARCOLECO_LICENSE = GPL-3.0
LIBRETRO_GEARCOLECO_LICENSE_FILES = LICENSE

define LIBRETRO_GEARCOLECO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/platforms/libretro platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_GEARCOLECO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/platforms/libretro/gearcoleco_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/gearcoleco_libretro.so
endef

$(eval $(generic-package))
