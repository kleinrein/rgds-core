################################################################################
#
# libretro-ruffle: Flash games (Ruffle 0.8.0 behind libretro, this repository's play/ruffle). Cross-built on the build
# machine by tools/build_ruffle.py into build/ruffle (Rust 1.99.1, zig as the linker, glibc 2.17): Buildroot 2026.02's
# Rust 1.88 is too old for Ruffle 0.8.0, and the crates would need the network and gigabytes of the VM's disk.
# tools/own_os_build.py builds it first and cleans this package before every image build.
#
################################################################################

LIBRETRO_RUFFLE_VERSION = local
LIBRETRO_RUFFLE_SITE = $(BR2_EXTERNAL_RGDS_PATH)/../../build/ruffle
LIBRETRO_RUFFLE_SITE_METHOD = local
LIBRETRO_RUFFLE_LICENSE = MIT or Apache-2.0
LIBRETRO_RUFFLE_DEPENDENCIES = mesa3d-panfrost

define LIBRETRO_RUFFLE_INSTALL_TARGET_CMDS
	test -f $(@D)/ruffle_libretro.so || { echo "build/ruffle/ruffle_libretro.so missing: python3 tools/build_ruffle.py"; exit 1; }
	$(INSTALL) -D -m 0644 $(@D)/ruffle_libretro.so $(TARGET_DIR)/usr/lib/libretro/ruffle_libretro.so
endef

$(eval $(generic-package))
