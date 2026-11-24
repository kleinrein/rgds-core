################################################################################
#
# libretro-fbneo: FinalBurn Neo, the arcade core RG DS Play loads for every arcade set but Nintendo's two-monitor
# cabinets (libretro-mame2003-plus). Its vertical games come sideways and RG DS Play turns them upright and stands
# them over both panels (the tall screen). assets/arcade/sets.tsv lists this commit's sets (tools/arcade_sets.py):
# bump both together.
#
################################################################################

LIBRETRO_FBNEO_VERSION = 92f33adcd5418cc96831788fd71e00884b9920f3
LIBRETRO_FBNEO_SITE = $(call github,libretro,FBNeo,$(LIBRETRO_FBNEO_VERSION))
LIBRETRO_FBNEO_LICENSE = FBNeo (non-commercial)
LIBRETRO_FBNEO_LICENSE_FILES = LICENSE.md

# The generated driver list and tables come with the source; generate-files makes them again from the drivers.
define LIBRETRO_FBNEO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src/burner/libretro generate-files
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src/burner/libretro platform=unix USE_CYCLONE=0 \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" LD="$(TARGET_CXX)"
endef

define LIBRETRO_FBNEO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/burner/libretro/fbneo_libretro.so $(TARGET_DIR)/usr/lib/libretro/fbneo_libretro.so
endef

$(eval $(generic-package))
