#!/usr/bin/env bash
# Produce the distributable binary at release/ABSClient.app.
#
# The name never changes -- README and installation instructions link to a
# fixed path, so a release replaces the file rather than adding a new one.
#
#   scripts/release.sh                 build, verify, write release/ABSClient.app
#   scripts/release.sh --allow-dirty   ...even with uncommitted changes
#
# Refuses to produce a binary that fails the tests or the checks below, because
# this is the file people install without reading the source.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

APP_NAME="ABSClient.app"
OUT="release/$APP_NAME"
ALLOW_DIRTY=0
[ "${1:-}" = "--allow-dirty" ] && ALLOW_DIRTY=1

fail() { echo "release: $*" >&2; exit 1; }

# --- 1. the tree must correspond to something ------------------------------
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
    if [ "$ALLOW_DIRTY" -eq 0 ]; then
        echo "Uncommitted changes. A released binary should match a commit," >&2
        echo "or nobody can tell what is in it. Commit first, or pass" >&2
        echo "--allow-dirty if you know what you are doing." >&2
        exit 1
    fi
    echo "==> WARNING: building from a dirty tree"
fi

COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

# --- 2. never ship something the tests reject ------------------------------
echo "==> Running host tests"
make test >/dev/null || fail "tests failed"

# --- 3. build from scratch --------------------------------------------------
echo "==> Building"
./scripts/build.sh clean >/dev/null 2>&1 || true
./scripts/build.sh >/dev/null || fail "build failed"
./scripts/build.sh release >/dev/null || fail "strip/copy failed"

[ -f "$OUT" ] || fail "$OUT was not produced"

# --- 4. verification gates --------------------------------------------------
# Everything below runs inside the SDK container, which has the ARM binutils.
SDK="$REPO_ROOT/sdk/SDK-B288"
run_in_sdk() {
    docker run --rm --platform linux/amd64 \
        -v "$REPO_ROOT:/work" -v "$SDK:/SDK:ro" \
        -e LD_LIBRARY_PATH=/SDK/usr/lib -w /work \
        abs-pocketbook-build sh -c "$1"
}

echo "==> Verifying"

HDR="$(run_in_sdk "/SDK/usr/bin/arm-obreey-linux-gnueabi-readelf -h $OUT")"
echo "$HDR" | grep -q "Machine:.*ARM" || fail "not an ARM binary"
echo "$HDR" | grep -q "soft-float ABI" || fail "wrong float ABI -- it will not load"
echo "   ok  ARM, soft-float"

NEEDED="$(run_in_sdk "/SDK/usr/bin/arm-obreey-linux-gnueabi-readelf -d $OUT" | grep NEEDED || true)"
for lib in libinkview.so libcurl.so libfreetype.so libc.so; do
    echo "$NEEDED" | grep -q "$lib" || fail "missing dependency: $lib"
done
# sqlite is linked statically on purpose; a dynamic one would not resolve on
# the device, whose rootfs we cannot inspect.
echo "$NEEDED" | grep -q "libsqlite3" && fail "sqlite must be linked statically"
echo "   ok  dependencies"

# Scan the strings once. Note the deliberate avoidance of `strings | grep -q`:
# under `set -o pipefail`, grep -q exits on the first match, strings takes
# SIGPIPE, and the pipeline reports failure precisely when the match SUCCEEDS.
SYMS="$(strings "$OUT")"

# Security regression gate: an API key must never reach a URL. This exact
# mistake shipped once already, in a committed binary.
if grep -qE "(cover|download)\?token=" <<<"$SYMS"; then
    fail "binary contains a token-in-URL format string"
fi
grep -q "REDACTED" <<<"$SYMS" || fail "log redaction is missing"
echo "   ok  no credentials in URLs, redaction present"

if grep -qE "eyJ[A-Za-z0-9_-]{16,}" <<<"$SYMS"; then
    fail "binary appears to contain a JWT"
fi
echo "   ok  no embedded key material"

# --- 5. report --------------------------------------------------------------
BUILD_ID="$(grep -E '^[0-9]{6}$' <<<"$SYMS" | head -1 || echo "?")"
SIZE="$(ls -lh "$OUT" | awk '{print $5}')"
SHA="$(shasum -a 256 "$OUT" | cut -c1-16)"

echo
echo "==> $OUT"
echo "    commit    $COMMIT"
echo "    build id  $BUILD_ID   (shown in the app's footer)"
echo "    size      $SIZE"
echo "    sha256    $SHA..."
echo
echo "Install: copy to the device's applications/ folder, or run scripts/deploy.sh"
