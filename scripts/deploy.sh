#!/usr/bin/env bash
# Copy the built app onto a PocketBook mounted over USB.
#
# Connect the device, choose "USB mass storage" / "Connect" on its screen, then
# run this. Eject before unplugging or the FAT32 write may not be flushed.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$REPO_ROOT/build/ABSClient.app"

if [ ! -f "$APP" ]; then
    echo "No build/ABSClient.app -- run scripts/build.sh first" >&2
    exit 1
fi

VOLUME="${1:-}"
if [ -z "$VOLUME" ]; then
    # Match on system/.pocketbook, the firmware's own marker file. Do NOT
    # probe for an "applications" directory: macOS volumes are case-insensitive,
    # so that also matches /Applications on the boot disk and on mounted DMGs.
    for v in /Volumes/*; do
        [ -f "$v/system/.pocketbook" ] && VOLUME="$v" && break
    done
fi

if [ -z "$VOLUME" ] || [ ! -f "$VOLUME/system/.pocketbook" ]; then
    echo "Could not find a mounted PocketBook (no /Volumes/*/system/.pocketbook)." >&2
    echo "Pass the volume explicitly: scripts/deploy.sh /Volumes/PB634" >&2
    exit 1
fi

echo "==> Deploying to $VOLUME"
cp "$APP" "$VOLUME/applications/ABSClient.app"

# App state dir. Creating it here means a first run finds it already present.
mkdir -p "$VOLUME/applications/ABSClient"

# macOS sprinkles AppleDouble sidecars (._ABSClient.app) onto FAT32. Harmless,
# but they clutter the device's applications folder -- drop ours and flush.
rm -f "$VOLUME/applications/._ABSClient.app" "$VOLUME/applications/._ABSClient"
sync

echo "==> Copied. Eject the device, then look under Applications."
echo "    Verbose logging: touch '$VOLUME/applications/ABSClient/LOGTRIGGER.TXT'"
