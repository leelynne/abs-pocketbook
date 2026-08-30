# PocketBook SDK toolchain image, for CI and reproducible local builds.
#
# Built from the official PocketBook release rather than a third-party image:
# this toolchain compiles the binary people install, so its provenance should
# be as auditable as the source.
#
# Everything happens in one RUN so the 6 GB intermediate never becomes a layer.
FROM debian:bookworm-slim

ARG SDK_URL=https://github.com/pocketbook/SDK_6.3.0/releases/download/6.8/SDK-B288-6.8.7z
ARG SDK_SHA256=

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        make build-essential file ca-certificates curl p7zip-full xz-utils; \
    rm -rf /var/lib/apt/lists/*; \
    \
    cd /tmp; \
    curl -fL --retry 3 -o sdk.7z "$SDK_URL"; \
    if [ -n "$SDK_SHA256" ]; then echo "$SDK_SHA256  sdk.7z" | sha256sum -c -; fi; \
    \
    # 7-Zip refuses relative symlinks that escape the extraction root and
    # leaves 0-byte files behind. Those become "file not recognized: File
    # truncated" at link time, so repair them before anything else.
    # 7-Zip exits 2 because it refuses ~86 relative symlinks that escape the
    # extraction root. Extraction itself succeeds, so check for the result
    # rather than trusting the exit code.
    7z x -y sdk.7z >/dev/null || echo "7z reported errors (expected)"; \
    test -d SDK-B288 || { echo "extraction produced nothing"; exit 1; }; \
    test -x SDK-B288/usr/bin/arm-obreey-linux-gnueabi-gcc \
        || { echo "toolchain missing after extraction"; exit 1; }; \
    mv SDK-B288 /SDK; \
    rm -f sdk.7z; \
    SYSROOT=/SDK/usr/arm-obreey-linux-gnueabi/sysroot; \
    repaired=0; \
    for stub in $(find "$SYSROOT" -type f -size 0 -name '*.so'); do \
        base=$(basename "$stub" .so); \
        target=$(ls -1 "$(dirname "$stub")/$base.so."* 2>/dev/null | head -1 || true); \
        if [ -z "$target" ]; then \
            target=$(ls -1 "$SYSROOT/lib/$base.so."* 2>/dev/null | head -1 || true); \
            [ -n "$target" ] && target="../../lib/$(basename "$target")"; \
        else \
            target=$(basename "$target"); \
        fi; \
        if [ -n "$target" ]; then ln -sf "$target" "$stub"; repaired=$((repaired+1)); fi; \
    done; \
    echo "repaired $repaired sysroot symlinks"; \
    \
    # Prune what we never build against. Qt5 alone is 3.9 GB, and the bundled
    # clang/LLVM another 546 MB; this project uses gcc and no Qt.
    rm -rf /SDK/local/qt5; \
    rm -rf /SDK/usr/share/doc /SDK/usr/share/gtk-doc /SDK/usr/share/info \
           /SDK/usr/share/locale /SDK/usr/share/man; \
    find /SDK/usr/arm-obreey-linux-gnueabi/bin /SDK/usr/bin \
        \( -name 'clang*' -o -name 'llvm-*' -o -name 'bugpoint' \
           -o -name 'c-index-test' -o -name 'opt' -o -name 'lli' -o -name 'llc' \
           -o -name 'dsymutil' -o -name 'sancov' -o -name 'sanstats' \
           -o -name 'obj2yaml' -o -name 'yaml2obj' -o -name 'diagtool' \
           -o -name 'verify-uselistorder' -o -name 'scan-*' -o -name 'git-clang-format' \) \
        -delete || true; \
    \
    du -sh /SDK

ENV SDK_ROOT=/SDK
ENV LD_LIBRARY_PATH=/SDK/usr/lib

# Canary: prove the pruned toolchain still compiles and links against inkview.
# If the prune above removed something load-bearing, the image build fails here
# rather than in someone's release.
RUN set -eux; \
    printf '#include <inkview.h>\nint main(void){ ScreenWidth(); return 0; }\n' > /tmp/canary.c; \
    SYSROOT=/SDK/usr/arm-obreey-linux-gnueabi/sysroot; \
    /SDK/usr/bin/arm-obreey-linux-gnueabi-gcc -Wall -O2 \
        -I"$SYSROOT/usr/local/include" -I"$SYSROOT/usr/include" \
        -o /tmp/canary /tmp/canary.c \
        -L"$SYSROOT/usr/local/lib" -L"$SYSROOT/usr/lib" \
        -linkview -lcurl -lfreetype "$SYSROOT/usr/lib/libsqlite3.a" -lpthread -lm -ldl; \
    # The float ABI lives in the ELF header flags, which `file` does not
    # report -- it needs readelf.
    READELF=/SDK/usr/bin/arm-obreey-linux-gnueabi-readelf; \
    $READELF -h /tmp/canary | grep -q 'Machine:.*ARM' \
        || { echo "canary is not an ARM binary"; exit 1; }; \
    $READELF -h /tmp/canary | grep -q 'soft-float ABI' \
        || { echo "wrong float ABI"; $READELF -h /tmp/canary; exit 1; }; \
    rm -f /tmp/canary /tmp/canary.c; \
    echo "toolchain canary OK"

WORKDIR /work
