################################################################################
#
# mesa3d-panfrost
#
################################################################################

MESA3D_PANFROST_VERSION = 25.0.7
MESA3D_PANFROST_SOURCE = mesa-$(MESA3D_PANFROST_VERSION).tar.xz
MESA3D_PANFROST_SITE = https://archive.mesa3d.org
MESA3D_PANFROST_LICENSE = MIT, SGI, Khronos
MESA3D_PANFROST_LICENSE_FILES = docs/license.rst licenses/MIT licenses/SGI-B-2.0
MESA3D_PANFROST_INSTALL_STAGING = YES
MESA3D_PANFROST_PROVIDES = libegl libgles libgbm
MESA3D_PANFROST_DEPENDENCIES = host-bison host-flex host-python-mako host-python-packaging host-python-pyyaml libdrm zlib

# Panfrost alone, on DRM with no window system; no LLVM (Bifrost's compiler is Mesa's own), no OpenCL, no video
# GLES 1 for PortMaster ports that link libGLESv1_CM.so.1 (Descent, BZFlag); the stock firmware's libmali has it.
MESA3D_PANFROST_CONF_OPTS = \
	-Dgallium-drivers=panfrost \
	-Dvulkan-drivers= \
	-Dplatforms= \
	-Dllvm=disabled \
	-Dglx=disabled \
	-Dglvnd=disabled \
	-Degl=enabled \
	-Dgbm=enabled \
	-Dopengl=true \
	-Dgles1=enabled \
	-Dgles2=enabled \
	-Dshared-glapi=enabled \
	-Dgallium-va=disabled \
	-Dgallium-vdpau=disabled \
	-Dgallium-xa=disabled \
	-Dgallium-nine=false \
	-Dgallium-opencl=disabled \
	-Dgallium-rusticl=false \
	-Dmicrosoft-clc=disabled \
	-Dvideo-codecs= \
	-Dxmlconfig=disabled \
	-Dexpat=disabled \
	-Dzstd=disabled \
	-Dvalgrind=disabled \
	-Dlibunwind=disabled \
	-Dlmsensors=disabled \
	-Dbuild-tests=false \
	-Dtools= \
	-Db_lto=true \
	-Db_pgo=use

# A profile trained on OutRun 2006 (pgo/README.md). Code it never ran keeps its plain -O2, not built for size, so other
# GL games lose nothing. Bump the profile with the version: Mesa's source has to match it. A generated header's absolute
# path differs from the training build's, which GCC calls a mismatch though the counters still apply: a warning, not an error.
MESA3D_PANFROST_PGO_FLAGS = -fprofile-partial-training -Wno-missing-profile -Wno-error=coverage-mismatch
MESA3D_PANFROST_CFLAGS = $(TARGET_CFLAGS) $(MESA3D_PANFROST_PGO_FLAGS)
MESA3D_PANFROST_CXXFLAGS = $(TARGET_CXXFLAGS) $(MESA3D_PANFROST_PGO_FLAGS)

define MESA3D_PANFROST_UNPACK_PROFILE
	tar -xzf $(MESA3D_PANFROST_PKGDIR)/pgo/profile-$(MESA3D_PANFROST_VERSION).tar.gz -C $(@D)/buildroot-build
endef
MESA3D_PANFROST_PRE_BUILD_HOOKS += MESA3D_PANFROST_UNPACK_PROFILE

$(eval $(meson-package))
