#!/usr/bin/env bash
# Write abs_client.cfg onto a mounted PocketBook, so the long ABS API key never
# has to be typed on the device's touch keyboard.
#
#   scripts/configure-device.sh https://abs.example.com
#       -> prompts for the key (input hidden, not echoed)
#
#   scripts/configure-device.sh https://abs.example.com ~/abs-key.txt
#       -> reads the key from a file
#
# The key is deliberately NOT accepted as a command-line argument: argv shows up
# in shell history and in `ps` output.
set -euo pipefail

SERVER="${1:-}"
KEY_FILE="${2:-}"

if [ -z "$SERVER" ]; then
    echo "usage: $(basename "$0") <server-url> [key-file]" >&2
    echo "  e.g. $(basename "$0") https://abs.example.com" >&2
    exit 1
fi

case "$SERVER" in
    http://*|https://*) ;;
    *) echo "Server must start with http:// or https://" >&2; exit 1 ;;
esac
SERVER="${SERVER%/}"   # the app expects no trailing slash

VOLUME=""
for v in /Volumes/*; do
    [ -f "$v/system/.pocketbook" ] && VOLUME="$v" && break
done
if [ -z "$VOLUME" ]; then
    echo "No mounted PocketBook found. Connect it and choose USB mass storage." >&2
    exit 1
fi

if [ -n "$KEY_FILE" ]; then
    [ -r "$KEY_FILE" ] || { echo "Cannot read $KEY_FILE" >&2; exit 1; }
    TOKEN="$(tr -d ' \t\r\n' < "$KEY_FILE")"
else
    printf 'Paste the ABS API key (input hidden), then press Enter:\n> ' >&2
    read -rs TOKEN
    printf '\n' >&2
    TOKEN="$(printf '%s' "$TOKEN" | tr -d ' \t\r\n')"
fi

if [ -z "$TOKEN" ]; then
    echo "No key given." >&2
    exit 1
fi

INSECURE=0
case "${ABS_INSECURE:-}" in 1|true|yes) INSECURE=1 ;; esac

APP_DIR="$VOLUME/applications/ABSClient"
mkdir -p "$APP_DIR"

umask 077
cat > "$APP_DIR/abs_client.cfg" <<CFG
# Audiobookshelf client configuration
server=$SERVER
token=$TOKEN
# insecure=1 skips TLS certificate verification.
# Only for a self-signed certificate on a server you control.
insecure=$INSECURE
CFG

rm -f "$APP_DIR/._abs_client.cfg"
sync

echo "==> Wrote $APP_DIR/abs_client.cfg"
echo "    server   = $SERVER"
echo "    token    = ${#TOKEN} characters"
echo "    insecure = $INSECURE"
echo "    (set ABS_INSECURE=1 to allow a self-signed certificate)"
echo "==> Eject the device and launch ABSClient."
