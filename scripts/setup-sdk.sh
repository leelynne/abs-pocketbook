#!/usr/bin/env bash
# Fetch and unpack the PocketBook SDK into sdk/.
#
# Verse Pro (PB634) is a dual-core Allwinner device -> B288 platform.
# (B300 is the quad-core SDK; swap SDK_ARCHIVE if you retarget.)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="$REPO_ROOT/sdk"
SDK_ARCHIVE="SDK-B288-6.8.7z"
SDK_URL="https://github.com/pocketbook/SDK_6.3.0/releases/download/6.8/$SDK_ARCHIVE"

mkdir -p "$SDK_DIR"
cd "$SDK_DIR"

if [ ! -f "$SDK_ARCHIVE" ]; then
    echo "==> Downloading $SDK_ARCHIVE (~1.3 GB)"
    curl -L -C - --retry 3 -o "$SDK_ARCHIVE" "$SDK_URL"
fi

if [ -d "SDK-B288" ]; then
    echo "==> sdk/SDK-B288 already extracted, nothing to do"
    exit 0
fi

echo "==> Extracting"
if command -v 7zz >/dev/null 2>&1; then
    7zz x -y "$SDK_ARCHIVE"
elif command -v 7z >/dev/null 2>&1; then
    7z x -y "$SDK_ARCHIVE"
else
    # No host 7-Zip: unpack inside a throwaway container instead of asking the
    # user to install one. Deliberately NOT --platform linux/amd64 -- unpacking
    # is arch-agnostic, so use the host's native arch and skip emulation.
    echo "    (no host 7z found -- extracting via Docker)"
    docker run --rm \
        -v "$SDK_DIR:/sdk" -w /sdk debian:bookworm-slim \
        sh -c "apt-get update -qq && apt-get install -y -qq p7zip-full >/dev/null && 7z x -y '$SDK_ARCHIVE'"
fi

echo "==> Done. Toolchain at sdk/SDK-B288/usr/bin/arm-obreey-linux-gnueabi-gcc"

# --- repair symlinks 7-Zip refused to create -------------------------------
#
# The SDK's sysroot uses relative symlinks that point above the extraction root
# (e.g. usr/lib/libm.so -> ../../lib/libm.so.6). 7-Zip treats those as
# "dangerous link paths", skips them, and leaves a 0-byte file behind -- which
# the linker then rejects with "file not recognized: File truncated".
#
# Recreate them by matching each empty libFOO.so to its versioned soname.
repair_sysroot_symlinks() {
    local sysroot="$SDK_DIR/SDK-B288/usr/arm-obreey-linux-gnueabi/sysroot"
    local repaired=0 stub base target

    [ -d "$sysroot" ] || return 0

    while IFS= read -r stub; do
        base="$(basename "$stub" .so)"
        # Prefer the real library next to the stub, then the sysroot's /lib.
        target="$(ls -1 "$(dirname "$stub")/$base.so."* 2>/dev/null | head -1)"
        [ -n "$target" ] || target="$(ls -1 "$sysroot/lib/$base.so."* 2>/dev/null | head -1)"
        [ -n "$target" ] || continue

        ln -sf "$(python3 -c 'import os,sys;print(os.path.relpath(sys.argv[1],sys.argv[2]))' \
                    "$target" "$(dirname "$stub")")" "$stub"
        repaired=$((repaired + 1))
    done < <(find "$sysroot" -type f -size 0 -name "*.so" 2>/dev/null)

    echo "==> Repaired $repaired sysroot symlink(s)"
}

repair_sysroot_symlinks
