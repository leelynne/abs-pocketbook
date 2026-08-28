#!/usr/bin/env bash
# Build inside the amd64 container that can run the SDK's x86_64 toolchain.
# Usage: scripts/build.sh [make-target...]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="abs-pocketbook-build"
SDK="$REPO_ROOT/sdk/SDK-B288"

if [ ! -d "$SDK" ]; then
    echo "SDK not found at $SDK -- run scripts/setup-sdk.sh first" >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> Building $IMAGE"
    docker build --platform linux/amd64 -t "$IMAGE" "$REPO_ROOT"
fi

# The SDK bundles its own host libraries (libmpfr, libgmp, libmpc ...) that
# cc1 needs at runtime; the container's linker won't find them otherwise.
docker run --rm --platform linux/amd64 \
    -v "$REPO_ROOT:/work" \
    -v "$SDK:/SDK:ro" \
    -w /work \
    -e LD_LIBRARY_PATH=/SDK/usr/lib \
    "$IMAGE" make "$@"
