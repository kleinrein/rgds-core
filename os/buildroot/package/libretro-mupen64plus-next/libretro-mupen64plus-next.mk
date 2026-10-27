################################################################################
#
# libretro-mupen64plus-next
#
################################################################################

LIBRETRO_MUPEN64PLUS_NEXT_VERSION = 15bada85906593376e8fd697f1b0b3af3a20f512
LIBRETRO_MUPEN64PLUS_NEXT_SITE = $(call github,libretro,mupen64plus-libretro-nx,$(LIBRETRO_MUPEN64PLUS_NEXT_VERSION))
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE = GPL-2.0
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE_FILES = LICENSE
LIBRETRO_MUPEN64PLUS_NEXT_DEPENDENCIES = mesa3d-panfrost

# GLideN64 on GLES 3 (Panfrost; RG DS Play makes the context) with the aarch64 dynarec and the HLE RSP. No ParaLLEl
# RDP/RSP (Vulkan, and too heavy for the A55s) and no NEON objects: the Makefile's NEON sources are 32-bit ARM.
define LIBRETRO_MUPEN64PLUS_NEXT_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) platform=unix \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR)" \
		ARCH=aarch64 WITH_DYNAREC=aarch64 FORCE_GLES3=1 HAVE_NEON=0 \
		HAVE_PARALLEL_RDP=0 HAVE_PARALLEL_RSP=0 HAVE_THR_AL=0 LLE=0
endef

define LIBRETRO_MUPEN64PLUS_NEXT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/mupen64plus_next_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mupen64plus_next_libretro.so
endef

$(eval $(generic-package))
