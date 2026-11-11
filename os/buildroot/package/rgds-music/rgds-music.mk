################################################################################
#
# rgds-music: the music player's engine, from this repository's src/music (FFmpeg for the files, SDL2 for the
# sound, which is ALSA here and so follows /etc/asound.conf to a Bluetooth headset). tools/own_os_build.py cleans
# this package before every image build: Buildroot copies a local package once and would keep an old engine.
#
################################################################################

RGDS_MUSIC_VERSION = local
RGDS_MUSIC_SITE = $(BR2_EXTERNAL_RGDS_PATH)/../../src/music
RGDS_MUSIC_SITE_METHOD = local
RGDS_MUSIC_LICENSE = Proprietary
RGDS_MUSIC_DEPENDENCIES = ffmpeg sdl2 host-pkgconf $(if $(BR2_PACKAGE_LIBOPENMPT),libopenmpt)
RGDS_MUSIC_LIBRARIES = libavformat libavcodec libswresample libswscale libavutil sdl2

# The sources include each other as "music/...": the copy stands in for that folder.
define RGDS_MUSIC_BUILD_CMDS
	ln -sfn . $(@D)/music
	$(TARGET_CXX) $(TARGET_CXXFLAGS) -std=c++17 -O2 -Wall -Wextra -I$(@D) \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags $(RGDS_MUSIC_LIBRARIES)) \
		$(@D)/engine.cpp -o $(@D)/rgds-music $(TARGET_LDFLAGS) \
		$$($(PKG_CONFIG_HOST_BINARY) --libs $(RGDS_MUSIC_LIBRARIES)) -lpthread
endef

define RGDS_MUSIC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rgds-music $(TARGET_DIR)/usr/bin/rgds-music
endef

$(eval $(generic-package))
