################################################################################
#
# rgds-media: films and books (play/src/media). Built from this repository's play/src, with the launcher's shared
# headers from src/ (its look, the touch reader, the music player's equaliser). tools/own_os_build.py cleans this
# package before every image build: Buildroot copies a local package once and would keep an old program.
#
################################################################################

RGDS_MEDIA_VERSION = local
RGDS_MEDIA_SITE = $(BR2_EXTERNAL_RGDS_PATH)/../../play/src
RGDS_MEDIA_SITE_METHOD = local
RGDS_MEDIA_LICENSE = GPL-3.0+ (MuPDF: AGPL-3.0+)
RGDS_MEDIA_DEPENDENCIES = ffmpeg libass mupdf sdl2 zlib host-pkgconf
RGDS_MEDIA_LIBRARIES = libavformat libavcodec libavfilter libswscale libswresample libavutil libass sdl2 zlib
RGDS_MEDIA_REPO_SRC = $(BR2_EXTERNAL_RGDS_PATH)/../../src
RGDS_MEDIA_SOURCES = media/main.cpp media/video.cpp media/book.cpp media/library.cpp media/gpu.cpp \
	media/shared.cpp media/library_screen.cpp media/watch_screen.cpp media/read_screen.cpp ui/canvas.cpp \
	ui/look.cpp ui/kit.cpp ui/words.cpp ui/png.cpp reader/pages.cpp third_party/stb_image.cpp

define RGDS_MEDIA_BUILD_CMDS
	cd $(@D) && $(TARGET_CXX) $(TARGET_CXXFLAGS) -std=c++17 -O2 -mcpu=cortex-a55 -Wall -Wextra \
		-I$(@D) -I$(RGDS_MEDIA_REPO_SRC) -I$(STAGING_DIR)/usr/include \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags $(RGDS_MEDIA_LIBRARIES)) \
		$(RGDS_MEDIA_SOURCES) -o $(@D)/rgds-media $(TARGET_LDFLAGS) \
		$$($(PKG_CONFIG_HOST_BINARY) --libs $(RGDS_MEDIA_LIBRARIES)) -lmupdf -lpthread -ldl
endef

define RGDS_MEDIA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rgds-media $(TARGET_DIR)/usr/bin/rgds-media
endef

$(eval $(generic-package))
