PYTHON ?= python3
DEVICE ?=
SD_ROOT ?=
THEME ?= themes/default/light.ini
# gcc-pgo: GCC 13.3 + upstream PGO in the Lima ARM64 VM (~8% less emulation time); zig: host cross build
ENGINE_COMPILER ?= gcc-pgo

.PHONY: preview settings game-menu dev dev-device build-host build-device build-engines build-melonds build-melonds-experiment build-rgds-experimental build-rgds-vulkan build-superdrastic build-samba package installer test probe deploy logs run run-dev rollback status doctor os-audit
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
build-freebios:
	$(PYTHON) tools/build_freebios.py
preview: build-host
	build/host/rgds-launcher --preview --watch-assets --theme "$(THEME)"
settings: build-host
	build/host/rgds-launcher --preview --settings --watch-assets --theme "$(THEME)"
game-menu: build-host
	build/host/rgds-launcher --preview --game-menu --game-core dsperate --watch-assets --theme "$(THEME)"
dev:
	$(PYTHON) tools/dev.py --theme "$(THEME)"
dev-device:
	$(PYTHON) tools/dev.py --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",) --theme "$(THEME)"
build-samba:
	$(PYTHON) tools/build_samba.py --fetch
package: build-device build-melonds build-superdrastic build-rgds-vulkan build-samba
	$(PYTHON) tools/package.py
installer: package
	$(PYTHON) tools/package_installer.py
test: build-host
	$(PYTHON) -m unittest discover -s tests -v
	build/host/rgds-launcher --self-test
	SDL_VIDEODRIVER=dummy build/host/rgds-launcher --preview --frames 2 --screenshot build/preview.bmp --theme themes/default/light.ini
probe:
	$(PYTHON) tools/device.py inventory --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
os-audit:
	$(PYTHON) tools/os_audit.py $(if $(DEVICE),--device "$(DEVICE)" --install-root "$(SD_ROOT)/rgds",)
deploy: package
	$(PYTHON) tools/device.py deploy --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
run:
	$(PYTHON) tools/device.py run --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
run-dev:
	$(PYTHON) tools/device.py run --dev --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
status:
	$(PYTHON) tools/device.py status --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
doctor:
	$(PYTHON) tools/device.py doctor --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
logs:
	$(PYTHON) tools/device.py logs --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
rollback:
	$(PYTHON) tools/device.py rollback --device "$(DEVICE)" $(if $(SD_ROOT),--sd-root "$(SD_ROOT)",)
