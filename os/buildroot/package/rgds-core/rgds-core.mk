RGDS_CORE_VERSION = local
RGDS_CORE_SITE = $(call qstrip,$(BR2_PACKAGE_RGDS_CORE_STAGE))
RGDS_CORE_SITE_METHOD = local
RGDS_CORE_LICENSE = Proprietary
define RGDS_CORE_INSTALL_TARGET_CMDS
	rm -rf $(TARGET_DIR)/usr/share/rgds/app $(TARGET_DIR)/usr/share/rgds/releases
	mkdir -p $(TARGET_DIR)/usr/share/rgds/releases
	cp -a $(@D)/app $(TARGET_DIR)/usr/share/rgds/releases/$$(cat $(@D)/build-id)
	cp $(@D)/build-id $(TARGET_DIR)/usr/share/rgds/active
	rm -rf $(TARGET_DIR)/usr/lib/rgds-panvk
	cp -a $(@D)/panvk $(TARGET_DIR)/usr/lib/rgds-panvk
endef
$(eval $(generic-package))
