################################################################################
#
# libretro-gw: gw-libretro, the Game & Watch simulators' core. Our 0001 keeps Game A, Game B and Time on their keys
# beside the core's own menu; its Lua sits in the source as C arrays too, written again here.
#
################################################################################

LIBRETRO_GW_VERSION = 91d599b951e7bfe7e040347f58667cba20074adc
LIBRETRO_GW_SITE = $(call github,libretro,gw-libretro,$(LIBRETRO_GW_VERSION))
LIBRETRO_GW_LICENSE = Zlib
LIBRETRO_GW_LICENSE_FILES = LICENSE

define LIBRETRO_GW_EMBED_LUA
	python3 $(LIBRETRO_GW_PKGDIR)/embed_lua.py $(@D)/gwlua/lua/compatinit.lua
endef
LIBRETRO_GW_POST_PATCH_HOOKS += LIBRETRO_GW_EMBED_LUA

define LIBRETRO_GW_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) -f Makefile.libretro platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_GW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/gw_libretro.so $(TARGET_DIR)/usr/lib/libretro/gw_libretro.so
endef

$(eval $(generic-package))
