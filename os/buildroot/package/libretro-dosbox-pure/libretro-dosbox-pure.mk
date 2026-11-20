################################################################################
#
# libretro-dosbox-pure: DOSBox Pure, the MS-DOS core RG DS Play loads for .zip, .dosz and disc images in a dos folder
#
################################################################################

# 1.0-preview7 (libretro's mirror; upstream moved to codeberg.org/schelling/dosbox-pure)
LIBRETRO_DOSBOX_PURE_VERSION = cdaea10ce8cac231bc341ca05d4c5e4e5fe0fbd5
LIBRETRO_DOSBOX_PURE_SITE = $(call github,libretro,dosbox-pure,$(LIBRETRO_DOSBOX_PURE_VERSION))
LIBRETRO_DOSBOX_PURE_LICENSE = GPL-2.0+
LIBRETRO_DOSBOX_PURE_LICENSE_FILES = LICENSE

# The Makefile picks its ARM flags from the build machine's uname: the RK3568's A55s are aarch64 with 4 KiB pages
# (the dynamic recompiler's page size), so they are given here whatever machine builds the image.
define LIBRETRO_DOSBOX_PURE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) platform=unix CXX="$(TARGET_CXX)" STRIP="$(TARGET_STRIP)" \
		MAKE_CPUFLAGS="-DPAGESIZE=4096 -mcpu=cortex-a55"
endef

define LIBRETRO_DOSBOX_PURE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/dosbox_pure_libretro.so $(TARGET_DIR)/usr/lib/libretro/dosbox_pure_libretro.so
endef

$(eval $(generic-package))
