################################################################################
#
# libretro-flycast: the Dreamcast core RG DS Play loads
#
################################################################################

LIBRETRO_FLYCAST_VERSION = 3a5267a5e32438026e1c5b97d5de6cd77236f72c
LIBRETRO_FLYCAST_SITE = https://github.com/flyinghead/flycast.git
LIBRETRO_FLYCAST_SITE_METHOD = git
LIBRETRO_FLYCAST_GIT_SUBMODULES = YES
LIBRETRO_FLYCAST_LICENSE = GPL-2.0+
LIBRETRO_FLYCAST_LICENSE_FILES = LICENSE
LIBRETRO_FLYCAST_DEPENDENCIES = mesa3d-panfrost zlib
LIBRETRO_FLYCAST_SUPPORTS_IN_SOURCE_BUILD = NO

# GLES 3 on Panfrost (RG DS Play makes the context) with the ARM64 SH4 dynarec, tuned for the A55s. No Vulkan: the
# player offers no Vulkan context, and PanVK was no faster than GL for PSP. Its own libchdr, zip and glslang stay
# inside the core. No OpenMP: its texture decoding threads would take the cores the SH4 and render threads need.
LIBRETRO_FLYCAST_CONF_OPTS = -DLIBRETRO=ON -DUSE_GLES=ON -DUSE_VULKAN=OFF -DUSE_OPENMP=OFF -DUSE_HOST_LIBZIP=OFF \
	-DUSE_LUA=OFF -DUSE_BREAKPAD=OFF -DUSE_DISCORD=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -mcpu=cortex-a55" -DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -mcpu=cortex-a55"

define LIBRETRO_FLYCAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/buildroot-build/flycast_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/flycast_libretro.so
endef

$(eval $(cmake-package))
