# Cross-build for PocketBook (firmware 6.x, B288 platform).
#
# Not run directly on macOS -- see scripts/build.sh, which invokes this inside
# the amd64 container where the SDK toolchain can execute.

SDK_ROOT ?= /SDK
SDK_PATH  = $(SDK_ROOT)/usr
SYSROOT   = $(SDK_PATH)/arm-obreey-linux-gnueabi/sysroot
CC        = $(SDK_PATH)/bin/arm-obreey-linux-gnueabi-gcc

APP_NAME  = ABSClient.app
BUILD_DIR = build

CORE_SRC   = $(wildcard src/core/*.c)
VENDOR_SRC = $(wildcard src/vendor/*.c)
UI_SRC     = $(wildcard src/ui/*.c)
SRC        = $(CORE_SRC) $(VENDOR_SRC) $(UI_SRC)
OBJ      = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRC))

# libinkview lives under /usr/local in the sysroot; everything else under /usr.
# Stamped into the binary and shown on screen, so the device can tell you
# which build it is actually running.
BUILD_ID := $(shell date +%H%M%S)

CFLAGS = -Wall -Wextra -O2 -std=gnu99 \
         -DABS_BUILD_ID=\"$(BUILD_ID)\" \
         -Isrc \
         -I$(SYSROOT)/usr/local/include \
         -I$(SYSROOT)/usr/include \
         -I$(SYSROOT)/usr/include/freetype2

# sqlite3 is linked statically: the firmware plainly uses SQLite, but its
# rootfs is not visible over USB so we cannot confirm the runtime .so is
# present, and ~800 KB removes the doubt.
LDFLAGS = -L$(SYSROOT)/usr/local/lib \
          -L$(SYSROOT)/usr/lib \
          -linkview -lcurl -lfreetype \
          $(SYSROOT)/usr/lib/libsqlite3.a \
          -lpthread -lm -ldl

.PHONY: all clean test spike release FORCE

all: $(BUILD_DIR)/$(APP_NAME)

# Throwaway hardware probe for the firmware audio player -- see spike/.
# Deliberately a separate binary so it cannot destabilise the real app.
SPIKE_NAME = ABSSpike.app

spike: $(BUILD_DIR)/$(SPIKE_NAME)

$(BUILD_DIR)/$(SPIKE_NAME): spike/spike_main.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "--- built $@ ---"
	@file $@

$(BUILD_DIR)/$(APP_NAME): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "--- built $@ ---"
	@file $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# version.o carries BUILD_ID, so it must be rebuilt every time or the stamp
# shown on the device keeps reporting whichever build first compiled it.
$(BUILD_DIR)/core/version.o: FORCE

FORCE:

# Host-side tests for src/core (no inkview, no cross-compiler, no device).
# Run these on macOS directly: make test
HOST_CC ?= cc
test:
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -Wall -Wextra -std=gnu99 -Isrc -DABS_BUILD_ID=\"test\" \
		tests/test_core.c $(CORE_SRC) $(VENDOR_SRC) -o $(BUILD_DIR)/test_core
	@$(BUILD_DIR)/test_core

# The stripped binary checked into release/, so the app can be installed
# without setting up the SDK.
release: $(BUILD_DIR)/$(APP_NAME)
	@mkdir -p release
	cp $(BUILD_DIR)/$(APP_NAME) release/$(APP_NAME)
	$(SDK_PATH)/bin/arm-obreey-linux-gnueabi-strip release/$(APP_NAME)
	@echo "--- release/$(APP_NAME) ---"
	@file release/$(APP_NAME)

clean:
	rm -rf $(BUILD_DIR)
