PYTHON ?= python3
DEVICE ?=
SD_ROOT ?= /mnt/mmc
THEME ?= themes/default/light.ini

.PHONY: preview settings dev dev-device build-host build-device package test probe deploy logs run run-dev rollback status doctor
build-host:
	$(PYTHON) tools/build.py host
build-device:
	$(PYTHON) tools/build.py device
preview: build-host
	build/host/rgds-launcher --preview --watch-assets --theme "$(THEME)"
settings: build-host
	build/host/rgds-launcher --preview --settings --watch-assets --theme "$(THEME)"
dev:
	$(PYTHON) tools/dev.py --theme "$(THEME)"
dev-device:
	$(PYTHON) tools/dev.py --device "$(DEVICE)" --sd-root "$(SD_ROOT)" --theme "$(THEME)"
package: build-device
	$(PYTHON) tools/package.py
test: build-host
	$(PYTHON) -m unittest discover -s tests -v
	build/host/rgds-launcher --self-test
	SDL_VIDEODRIVER=dummy build/host/rgds-launcher --preview --frames 2 --screenshot build/preview.bmp --theme themes/default/light.ini
probe:
	$(PYTHON) tools/device.py inventory --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
deploy: package
	$(PYTHON) tools/device.py deploy --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
run:
	$(PYTHON) tools/device.py run --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
run-dev:
	$(PYTHON) tools/device.py run --dev --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
status:
	$(PYTHON) tools/device.py status --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
doctor:
	$(PYTHON) tools/device.py doctor --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
logs:
	$(PYTHON) tools/device.py logs --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
rollback:
	$(PYTHON) tools/device.py rollback --device "$(DEVICE)" --sd-root "$(SD_ROOT)"
