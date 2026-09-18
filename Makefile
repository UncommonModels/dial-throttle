# Dial Throttle firmware and documentation site.
#
# The board, core and library versions are pinned in DialThrottle/sketch.yaml,
# so the first build downloads everything it needs.

SKETCH      := DialThrottle
PROFILE     ?= m5dial

# The native USB Serial/JTAG port enumerates as ttyACM, not ttyUSB. With more
# than one board attached, pass the stable name instead:
#   PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00
PORT        ?= /dev/ttyACM0
BAUD        ?= 115200
ARDUINO_CLI ?= arduino-cli
PYTHON      ?= python3

# Extra preprocessor flags, e.g. DEFINES=-DSERIAL_OUTPUT_ONLY_DEFAULT=1
DEFINES     ?=
BUILD_DIR   := build
BUILDFLAGS  := --profile $(PROFILE) --build-path $(BUILD_DIR) \
	$(if $(DEFINES),--build-property 'compiler.cpp.extra_flags=$(DEFINES)',)

DOCS        := docs
SITE_DIR    := $(DOCS)/public
HUGO        ?= hugo
SERVE_PORT  ?= 1313

# The parts list is maintained as CSV and staged into the site, so the page and
# the order form cannot drift apart.
PARTS       := parts/partslist.csv
PARTS_ASSET := $(DOCS)/assets/parts/partslist.csv

# Rebuild whenever DEFINES changes, so `make flash` never uploads a build made with other flags.
DEFINES_FILE := $(BUILD_DIR)/.defines
$(shell mkdir -p $(BUILD_DIR); echo '$(DEFINES)' | cmp -s - $(DEFINES_FILE) || echo '$(DEFINES)' > $(DEFINES_FILE))

SOURCES := $(wildcard $(SKETCH)/*.ino $(SKETCH)/*.cpp $(SKETCH)/*.h $(SKETCH)/sketch.yaml)

.DEFAULT_GOAL := help

## help: list the available targets
help:
	@echo 'Dial Throttle targets:'
	@sed -n 's/^## \([a-z-]*\): /  \1|/p' $(MAKEFILE_LIST) | column -t -s'|'
	@echo
	@echo 'Override PORT=, DEFINES= as needed.'

# --------------------------------------------------------------------------
# Firmware
# --------------------------------------------------------------------------

## build: compile the firmware
build: $(BUILD_DIR)/.stamp
$(BUILD_DIR)/.stamp: $(SOURCES) $(DEFINES_FILE) Makefile
	$(ARDUINO_CLI) compile $(BUILDFLAGS) $(SKETCH)
	@touch $@

## flash: build, then upload over USB
flash: build
	$(ARDUINO_CLI) upload --profile $(PROFILE) -p $(PORT) \
		--input-dir $(BUILD_DIR) $(SKETCH)

## monitor: open the serial console
monitor:
	$(ARDUINO_CLI) monitor -p $(PORT) --config baudrate=$(BAUD)

## size: report flash and RAM use
size: build
	@$(ARDUINO_CLI) compile $(BUILDFLAGS) $(SKETCH) 2>&1 \
		| grep -E 'Sketch uses|Global variables'

## deps: install the core and libraries globally, for the Arduino IDE
# Large: the ESP32 core is several hundred MB of downloads. `build` does not
# need this; the profile installs its own pinned copies.
deps:
	$(ARDUINO_CLI) core update-index --additional-urls \
		https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
	$(ARDUINO_CLI) core install esp32:esp32@3.1.3 --additional-urls \
		https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
	$(ARDUINO_CLI) lib update-index
	$(ARDUINO_CLI) lib install M5Dial@1.0.3 M5Unified@0.2.22 M5GFX@0.2.29 \
		'SparkFun Qwiic Keypad Arduino Library@1.2.0' 'MySQL Connector Arduino@1.2.0'

## lint: compile with warnings turned up
lint:
	$(ARDUINO_CLI) compile --profile $(PROFILE) --warnings all \
		--build-path $(BUILD_DIR)/lint $(SKETCH)

## screenshot: capture the screen to screenshot.png (needs pyserial and Pillow)
screenshot:
	$(PYTHON) tools/screenshot.py --port $(PORT) screenshot.png

## clean: remove build artefacts
clean:
	rm -rf $(BUILD_DIR) $(SITE_DIR) $(DOCS)/.hugo_build.lock

# --------------------------------------------------------------------------
# Documentation site
# --------------------------------------------------------------------------

## site-assets: stage the parts list into the site
site-assets: $(PARTS_ASSET)
$(PARTS_ASSET): $(PARTS)
	@mkdir -p $(dir $@)
	cp $< $@

## docs: build the Hugo docs site into docs/public
# --cleanDestinationDir removes output for pages that no longer exist; without
# it a deleted page lingers in docs/public and keeps being served.
docs: site-assets
	$(HUGO) --source $(DOCS) --minify --cleanDestinationDir

## serve: run the Hugo dev server with live reload (override with SERVE_PORT=)
serve: site-assets
	$(HUGO) server --source $(DOCS) --disableFastRender --port $(SERVE_PORT)

.PHONY: help build flash monitor size deps lint screenshot clean site-assets docs serve
