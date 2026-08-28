#!/usr/bin/env bash
# Put the launcher config back the way it was.
#   scripts/restore-desktop.sh [backup-stamp]     (default: most recent)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

STAMP="${1:-}"
if [ -z "$STAMP" ]; then
    STAMP="$(ls -1 "$REPO_ROOT/device-backup" 2>/dev/null | sort | tail -1)"
fi
BACKUP="$REPO_ROOT/device-backup/$STAMP"
[ -d "$BACKUP" ] || { echo "No backup at $BACKUP" >&2; exit 1; }

VOLUME=""
for v in /Volumes/*; do
    [ -f "$v/system/.pocketbook" ] && VOLUME="$v" && break
done
[ -n "$VOLUME" ] || { echo "No mounted PocketBook found." >&2; exit 1; }

cp "$BACKUP/view.json" "$VOLUME/system/config/desktop/view.json"
cp "$BACKUP/hash.txt"  "$VOLUME/system/config/desktop/hash.txt"
rm -f "$VOLUME/applications/icons/absclient.bmp"
sync

echo "==> Restored launcher config from $STAMP"
