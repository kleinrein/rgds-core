################################################################################
#
# rgds-pad: the RG DS's controls as one game pad, from this repository's src/pad. tools/own_os_build.py cleans this
# package before every image build: Buildroot copies a local package once and would keep an old daemon.
#
################################################################################

RGDS_PAD_VERSION = local
RGDS_PAD_SITE = $(BR2_EXTERNAL_RGDS_PATH)/../../src/pad
RGDS_PAD_SITE_METHOD = local
RGDS_PAD_LICENSE = Proprietary

define RGDS_PAD_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -O2 -Wall -Wextra -Werror -I$(@D) \
		$(@D)/rgds-pad.c -o $(@D)/rgds-pad $(TARGET_LDFLAGS)
endef

define RGDS_PAD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/rgds-pad $(TARGET_DIR)/usr/bin/rgds-pad
endef

$(eval $(generic-package))
