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
CFLAGS = -Wall -Wextra -O2 -std=gnu99 \
         -Isrc \
         -I$(SYSROOT)/usr/local/include \
         -I$(SYSROOT)/usr/include \
         -I$(SYSROOT)/usr/include/freetype2

LDFLAGS = -L$(SYSROOT)/usr/local/lib \
          -L$(SYSROOT)/usr/lib \
          -linkview -lcurl -lfreetype -lm -ldl

.PHONY: all clean test

all: $(BUILD_DIR)/$(APP_NAME)

$(BUILD_DIR)/$(APP_NAME): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "--- built $@ ---"
	@file $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Host-side tests for src/core (no inkview, no cross-compiler, no device).
# Run these on macOS directly: make test
HOST_CC ?= cc
test:
	@mkdir -p $(BUILD_DIR)
	$(HOST_CC) -Wall -Wextra -std=gnu99 -Isrc \
		tests/test_core.c $(CORE_SRC) $(VENDOR_SRC) -o $(BUILD_DIR)/test_core
	@$(BUILD_DIR)/test_core

clean:
	rm -rf $(BUILD_DIR)
