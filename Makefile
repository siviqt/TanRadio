# Production target and pinned SDK. Environment overrides are supported.
DEVICE ?= tanmatsu
IDF_TARGET ?= esp32p4
IDF_VERSION ?= v6.0.2
BUILD ?= build/$(DEVICE)
SDKCONFIG_DEFAULTS ?= sdkconfigs/general;sdkconfigs/$(DEVICE)
SDKCONFIG ?= sdkconfig_$(DEVICE)
IDF_PATH ?= $(shell if test -f .IDF_PATH; then cat .IDF_PATH; else printf '%s' '$(HOME)/.espressif/$(IDF_VERSION)/esp-idf'; fi)
IDF_TOOLS_PATH ?= $(shell if test -f .IDF_TOOLS_PATH; then cat .IDF_TOOLS_PATH; else printf '%s' '$(HOME)/.espressif/tools'; fi)
IDF_SOURCE ?= $(IDF_PATH)/export.sh
BADGELINK ?= badgelink
SOURCE_EXPORT ?= dist/TanRadio-source
PYTHON ?= python3
APP_REVISION = $(shell $(PYTHON) tools/generate_build_info.py --metadata metadata.json --print-revision)

SHELL := /usr/bin/env bash
MAKEFLAGS += --silent
export IDF_PATH IDF_TOOLS_PATH
IDF_PARAMS = -B "$(BUILD)" -DDEVICE=$(DEVICE) -DSDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" -DSDKCONFIG="$(SDKCONFIG)" -DIDF_TARGET=$(IDF_TARGET) -DFAT=0

.PHONY: all help check-sdk print-sdk build release package test source-export clean fullclean menuconfig install run
all: build

help:
	printf '%s\n' 'make build          Build the Tanmatsu application with ESP-IDF v6.0.2' \
	  'make release        Refresh releases/TanRadio.bin and its SHA-256 checksum' \
	  'make package        Build, check and prepare repository/source upload packages' \
	  'make test           Run all regression checks (requires a build)' \
	  'make source-export  Create a clean Git-sharing folder in dist/TanRadio-source' \
	  'make install        Build and upload through BadgeLink (set BADGELINK)' \
	  'make run            Start the installed tanradio application' \
	  'make clean          Remove build output; keep the packaged application' \
	  'make fullclean      Also remove downloaded components and Python caches'

check-sdk:
	@test -f "$(IDF_SOURCE)" || { printf '%s\n' 'Install ESP-IDF v6.0.2 and set IDF_PATH / IDF_TOOLS_PATH; see README.md.' >&2; exit 1; }

print-sdk:
	printf 'ESP-IDF: %s\nTools: %s\n' "$(IDF_PATH)" "$(IDF_TOOLS_PATH)"

build: check-sdk
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) build

release: build
	$(MAKE) test
	mkdir -p releases
	cp "$(BUILD)/application.bin" releases/TanRadio.bin
	cd releases && sha256sum TanRadio.bin > TanRadio.bin.sha256

package: release
	$(PYTHON) tools/package_release.py

test:
	@set -e; for check in app_startup network_startup audio_shutdown audio_gain audio_pcm audio_pipeline icy exit external_display now_playing_scroll about controls station_removal display_updates display_variants radio_browser ui_stack bsp_integration privacy; do \
	  $(PYTHON) "tools/tests/test_$$check.py"; \
	done

source-export:
	$(PYTHON) tools/export_source.py "$(SOURCE_EXPORT)"

clean:
	rm -rf -- "$(BUILD)"

fullclean: clean
	rm -rf -- build managed_components tools/__pycache__ tools/tests/__pycache__
	rm -f -- sdkconfig.old sdkconfig_*.old

menuconfig: check-sdk
	source "$(IDF_SOURCE)" >/dev/null && idf.py $(IDF_PARAMS) menuconfig

# The launcher must already be in BadgeLink mode. Install the application only.
install: build
	"$(BADGELINK)" appfs upload tanradio TanRadio "$(APP_REVISION)" "$(BUILD)/application.bin"

run:
	"$(BADGELINK)" start tanradio
