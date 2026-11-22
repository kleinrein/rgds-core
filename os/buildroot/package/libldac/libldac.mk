################################################################################
#
# libldac
#
################################################################################

# the release tarball: the git archive lacks the libldac submodule (AOSP's platform/external/libldac)
LIBLDAC_VERSION = 2.0.2.3
LIBLDAC_SOURCE = ldacBT-$(LIBLDAC_VERSION).tar.gz
LIBLDAC_SITE = https://github.com/EHfive/ldacBT/releases/download/v$(LIBLDAC_VERSION)
LIBLDAC_LICENSE = Apache-2.0
LIBLDAC_LICENSE_FILES = libldac/LICENSE
LIBLDAC_INSTALL_STAGING = YES
# the A55 has an FPU: the floating-point encoder (LDAC_SOFT_FLOAT is for cores without one)
# CMakeLists.txt asks for CMake 3.0, which CMake 4 refuses without the policy floor
LIBLDAC_CONF_OPTS = -DLDAC_SOFT_FLOAT=OFF -DINSTALL_LIBDIR=/usr/lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5

$(eval $(cmake-package))

# bluez-alsa (Buildroot's own package) knows no LDAC option: its configure flag and the build order from here.
# Its configure commands read CONF_OPTS when they run, so this append after its eval still counts; the dependency
# goes on its configure stamp the way pkg-generic.mk orders FINAL_DEPENDENCIES (no per-package directories here).
ifeq ($(BR2_PACKAGE_LIBLDAC),y)
BLUEZ_ALSA_CONF_OPTS += --enable-ldac
$(BLUEZ_ALSA_TARGET_CONFIGURE): | libldac
endif
