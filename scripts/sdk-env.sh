# Shared toolchain selection, sourced by build.sh and release.sh.
#
# Sets:
#   ABS_SDK_RUN   an array: the docker invocation prefix, ready to append a
#                 command to. Use as ${ABS_SDK_RUN[@]+"${ABS_SDK_RUN[@]}"} --
#                 macOS ships bash 3.2, where "${arr[@]}" on an empty array
#                 trips `set -u`.
#   ABS_BUILD_ID  the commit stamp, computed out here because the container
#                 has no git.
#
# A local sdk/SDK-B288 wins if present (offline builds, or a modified SDK);
# otherwise the published image CI uses, so the two cannot drift.

_sdk_dir="$REPO_ROOT/sdk/SDK-B288"
_sdk_image="${ABS_SDK_IMAGE:-ghcr.io/leelynne/pocketbook-sdk:6.8-b288}"

ABS_BUILD_ID="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)"
git -C "$REPO_ROOT" diff --quiet HEAD 2>/dev/null || ABS_BUILD_ID="$ABS_BUILD_ID+"

# Test for the compiler, not the directory: an empty or half-extracted
# sdk/SDK-B288 would otherwise be taken for a usable SDK and fail at the first
# compile.
if [ -x "$_sdk_dir/usr/bin/arm-obreey-linux-gnueabi-gcc" ]; then
    _image="abs-pocketbook-build"
    if ! docker image inspect "$_image" >/dev/null 2>&1; then
        echo "==> Building $_image"
        docker build --platform linux/amd64 -t "$_image" "$REPO_ROOT"
    fi
    ABS_SDK_RUN=(docker run --rm --platform linux/amd64
        -v "$REPO_ROOT:/work" -v "$_sdk_dir:/SDK:ro"
        -w /work -e LD_LIBRARY_PATH=/SDK/usr/lib "$_image")
else
    _image="$_sdk_image"
    if ! docker image inspect "$_image" >/dev/null 2>&1; then
        echo "==> Pulling $_image (once, ~1.7 GB)"
        docker pull --platform linux/amd64 -q "$_image" >/dev/null \
            || { echo "Could not pull $_image. Run scripts/setup-sdk.sh for a local SDK." >&2; exit 1; }
    fi
    # The image already carries /SDK.
    ABS_SDK_RUN=(docker run --rm --platform linux/amd64
        -v "$REPO_ROOT:/work"
        -w /work -e LD_LIBRARY_PATH=/SDK/usr/lib "$_image")
fi
