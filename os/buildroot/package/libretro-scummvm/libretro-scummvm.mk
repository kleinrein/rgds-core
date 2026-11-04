################################################################################
#
# libretro-scummvm: the lite engines (LITE=1: lite_engines.list). The Makefile fetches its own dependencies at this
# pinned commit, with git, which needs Buildroot's GIT_DIR out of the way. Data: its scummvm.zip, without what only left-out engines and CJK text use.
#
################################################################################

LIBRETRO_SCUMMVM_VERSION = e0ddc60bfabb1efdf1e135006c8914ca59d0596c
LIBRETRO_SCUMMVM_SITE = $(call github,libretro,scummvm,$(LIBRETRO_SCUMMVM_VERSION))
LIBRETRO_SCUMMVM_LICENSE = GPL-3.0+
LIBRETRO_SCUMMVM_LICENSE_FILES = COPYING
LIBRETRO_SCUMMVM_DIR_LIBRETRO = $(@D)/backends/platform/libretro
LIBRETRO_SCUMMVM_LEFT_OUT = fonts-cjk.dat ultima.dat titanic.dat fonts-imgui.dat Roland_SC-55.sf2 neverhood.dat access.dat tony.dat toon.dat grim-patch.lab hadesch_translations.dat

define LIBRETRO_SCUMMVM_BUILD_CMDS
	$(TARGET_MAKE_ENV) env -u GIT_DIR $(MAKE) -C $(LIBRETRO_SCUMMVM_DIR_LIBRETRO) LITE=1 platform=unix HAVE_OPENGL=0 HAVE_OPENGLES2=0 USE_CLOUD=0 USE_MT32EMU=0 DEBUG_ALLOW_DIRTY_SUBMODULES=0 \
		CC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR) cru" RANLIB="$(TARGET_RANLIB)"
	$(TARGET_MAKE_ENV) env -u GIT_DIR $(MAKE) -C $(LIBRETRO_SCUMMVM_DIR_LIBRETRO) datafiles LITE=1 platform=unix DEBUG_ALLOW_DIRTY_SUBMODULES=0
endef

define LIBRETRO_SCUMMVM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(LIBRETRO_SCUMMVM_DIR_LIBRETRO)/scummvm_libretro.so $(TARGET_DIR)/usr/lib/libretro/scummvm_libretro.so
	mkdir -p $(TARGET_DIR)/usr/share/libretro
	unzip -qo $(LIBRETRO_SCUMMVM_DIR_LIBRETRO)/scummvm.zip -d $(TARGET_DIR)/usr/share/libretro
	cd $(TARGET_DIR)/usr/share/libretro/scummvm/extra && rm -f $(LIBRETRO_SCUMMVM_LEFT_OUT)
endef

$(eval $(generic-package))
