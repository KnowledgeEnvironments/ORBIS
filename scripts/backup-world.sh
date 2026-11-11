#!/bin/zsh
# A world's history, for what pruning takes and a disk could lose: its log, snapshots, anchor and the
# blobs still held. The newest few are kept. Restore by stopping the world and copying them back.
#   scripts/backup-world.sh <data dir> [dest]     default KE_BACKUPS, else backups/ beside the data dir
set -eu
DATA="${1:?a world's data directory}"
NAME="$(basename "$DATA")"
DEST="${2:-${KE_BACKUPS:-$(dirname "$D")/backups}}/$NAME"
KEEP=3
mkdir -p "$DEST"
# at most one a day, however often a self-hosting world restarts
[ -n "$(find "$DEST" -maxdepth 1 -name '20*' -mtime -1 2>/dev/null | head -1)" ] && exit 0
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
TMP="$DEST/.$STAMP"
mkdir -p "$TMP"
for d in log snapshots anchor blobs; do
  [ -d "$DATA/$d" ] && rsync -a "$DATA/$d" "$TMP/"
done
for f in world.edn; do [ -f "$DATA/$f" ] && cp -p "$DATA/$f" "$TMP/"; done
mv "$TMP" "$DEST/$STAMP"
ls -1d "$DEST"/20* | sort -r | tail -n +$((KEEP + 1)) | while read -r old; do rm -rf "$old"; done
echo "backup: $DEST/$STAMP $(du -sh "$DEST/$STAMP" | cut -f1)"
