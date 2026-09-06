#!/usr/bin/env bash
# Build inside the amd64 container that can run the SDK's x86_64 toolchain.
# Usage: scripts/build.sh [make-target...]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$REPO_ROOT/scripts/sdk-env.sh"

${ABS_SDK_RUN[@]+"${ABS_SDK_RUN[@]}"} make BUILD_ID="$ABS_BUILD_ID" "$@"
