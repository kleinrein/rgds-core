################################################################################
#
# libretro-fake08
#
################################################################################

# fake-08, a free PICO-8 player (its Lua fork z8lua is a submodule: fetched with git). Dropped 2026-10-28, back
# 2026-11-18 at the owner's request.
LIBRETRO_FAKE08_VERSION = 814991a2571ad3970e386cef48f3b148aa1c27b9
LIBRETRO_FAKE08_SITE = https://github.com/jtothebell/fake-08.git
LIBRETRO_FAKE08_SITE_METHOD = git
LIBRETRO_FAKE08_GIT_SUBMODULES = YES
LIBRETRO_FAKE08_LICENSE = MIT
LIBRETRO_FAKE08_LICENSE_FILES = LICENSE.MD

# z8lua is compiled as C++ here, so Lua errors were C++ exceptions, and the first one in a cartridge (Lab Cat, 2026-11-18)
# ended in "terminate called after throwing an instance of 'lua_longjmp*'" and a SIGABRT (launch error 134). setjmp and
# longjmp, Lua's own way, need no unwinding through the core's frames.
define LIBRETRO_FAKE08_BUILD_CMDS
	$(TARGET_MAKE_ENV) CFLAGS="-DLUA_USE_LONGJMP" CXXFLAGS="-DLUA_USE_LONGJMP" $(MAKE) -C $(@D)/platform/libretro platform=unix CC="$(TARGET_CC)" CXX="$(TARGET_CXX)"
endef

define LIBRETRO_FAKE08_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/platform/libretro/fake08_libretro.so $(TARGET_DIR)/usr/lib/libretro/fake08_libretro.so
endef

$(eval $(generic-package))
