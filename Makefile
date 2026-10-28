PYTHON ?= python3
DEVICE ?= rgds.local
VERSION_NUMBER := $(shell cat VERSION)
THEME ?= themes/default/light.ini
# gcc-pgo: GCC 13.3 + upstream PGO in the Lima ARM64 VM (~8% less emulation time); zig: host cross build
ENGINE_COMPILER ?= gcc-pgo

.PHONY: preview settings game-menu dev build-host build-device build-engines build-melonds build-melonds-experiment build-rgds-experimental build-rgds-vulkan build-superdrastic build-samba package image push release test shot format format-check
build-host:
	$(PYTHON) tools/build.py host
build-device:
	$(PYTHON) tools/build.py device
build-engines:
	$(PYTHON) tools/build_engines.py --fetch --compiler $(ENGINE_COMPILER)
build-melonds: build-engines
	$(PYTHON) tools/build_melonds.py --fetch --frontend
build-rgds-experimental: build-engines
	$(PYTHON) tools/build_rgds_experimental.py --compiler $(ENGINE_COMPILER)
build-rgds-vulkan: build-rgds-experimental
	$(PYTHON) tools/build_rgds_vulkan.py
	$(PYTHON) tools/package_rgds_vulkan.py
build-melonds-experiment:
	$(PYTHON) tools/build_melonds.py --fetch
build-superdrastic:
	$(PYTHON) tools/build_superdrastic.py --fetch
preview: build-host
	build/host/rgds-launcher --preview --watch-assets --theme "$(THEME)"
settings: build-host
	build/host/rgds-launcher --preview --settings --watch-assets --theme "$(THEME)"
game-menu: build-host
	build/host/rgds-launcher --preview --game-menu --game-core dsperate --watch-assets --theme "$(THEME)"
dev:
	$(PYTHON) tools/dev.py --theme "$(THEME)"
build-samba:
	$(PYTHON) tools/build_samba.py --fetch
package: build-device build-melonds build-superdrastic build-rgds-vulkan build-samba
	$(PYTHON) tools/package.py
# RG DS OS: the card image (Buildroot in the Lima VM, docs/OWN_OS_PLAN.md), a push of it to the handheld's other
# slot over SSH, and the release folder (card image, signed OTA package and feed; uploads nothing)
image: package
	$(PYTHON) tools/own_os_build.py
push:
	$(PYTHON) tools/own_os_push.py $(DEVICE) --reboot
release:
	$(PYTHON) tools/own_os_release.py --version $(VERSION_NUMBER) $(if $(NOTES),--notes "$(NOTES)",)
# Pinned yapf + clang-format (requirements-format.txt); format-check changes nothing.
format:
	$(PYTHON) tools/format.py
format-check:
	$(PYTHON) tools/format.py --check
test: build-host
	$(PYTHON) -m unittest discover -s tests -v
	build/host/rgds-launcher --self-test
	SDL_VIDEODRIVER=dummy build/host/rgds-launcher --preview --frames 2 --screenshot build/preview.bmp --theme themes/default/light.ini
shot:
	$(PYTHON) tools/device_shot.py --device "$(DEVICE)" $(SHOT_ARGS)
