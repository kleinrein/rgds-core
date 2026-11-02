################################################################################
#
# libretro-ppsspp: the PlayStation Portable core RG DS Play loads
#
################################################################################

LIBRETRO_PPSSPP_VERSION = fa50bb1976065c4f8b1b47af227d367fe9771555
LIBRETRO_PPSSPP_SITE = https://github.com/hrydgard/ppsspp.git
LIBRETRO_PPSSPP_SITE_METHOD = git
LIBRETRO_PPSSPP_GIT_SUBMODULES = YES
LIBRETRO_PPSSPP_LICENSE = GPL-2.0+
LIBRETRO_PPSSPP_LICENSE_FILES = LICENSE.TXT
LIBRETRO_PPSSPP_DEPENDENCIES = mesa3d-panfrost zlib
LIBRETRO_PPSSPP_SUPPORTS_IN_SOURCE_BUILD = NO

# GLES 3 on Panfrost (RG DS Play makes the context) with the ARM64 JIT, tuned for the A55s; PPSSPP's prebuilt FFmpeg
# for this architecture is linked in. Its CMake finds ccache on its own, which breaks under Buildroot's wrapper, and
# its own libraries (glslang, cpu_features) must go into the core rather than beside it.
LIBRETRO_PPSSPP_CONF_OPTS = -DLIBRETRO=ON -DUSE_CCACHE=OFF -DBUILD_SHARED_LIBS=OFF -DUSING_GLES2=ON -DUSING_EGL=OFF -DUSING_X11_VULKAN=OFF \
	-DUSE_DISCORD=OFF -DUSE_MINIUPNPC=OFF -DUSE_WAYLAND_WSI=OFF \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -mcpu=cortex-a55" -DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -mcpu=cortex-a55"

# The other platforms' FFmpeg builds are most of the 2.4 GB checkout.
define LIBRETRO_PPSSPP_PRUNE_FFMPEG
	rm -rf $(addprefix $(@D)/ffmpeg/,Windows Windows10 android ios macosx wiiu) \
		$(addprefix $(@D)/ffmpeg/linux/,armv7 loongarch64 mips32 riscv64 webos x86 x86_64)
endef
LIBRETRO_PPSSPP_POST_EXTRACT_HOOKS += LIBRETRO_PPSSPP_PRUNE_FFMPEG

# Its own install rule is not for libretro: the core and, where its system folder is looked up, the assets its
# configure step gathered.
define LIBRETRO_PPSSPP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/buildroot-build/lib/ppsspp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/ppsspp_libretro.so
	mkdir -p $(TARGET_DIR)/usr/share/libretro/PPSSPP
	cp -a $(@D)/buildroot-build/assets/. $(TARGET_DIR)/usr/share/libretro/PPSSPP/
endef

$(eval $(cmake-package))
