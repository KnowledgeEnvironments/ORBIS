#!/bin/bash
# Make a release: the exact tree of one commit with a manifest, read-only, under <data>/releases/<commit>.
# Every instance carries the source it runs; the harness mount points here (docs/self-hosting.md).
#   scripts/release.sh --data <dir> [--repo <git dir>] [--rev <rev>]
# The first release of a data dir also becomes the running one, and <data>/src, the working copy,
# is cloned from the repo at that commit. Later releases are only made here: world.restart moves to them.
# Prints the commit.
set -euo pipefail
SELF="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$SELF"; REV=HEAD; DATA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --data) DATA="$2"; shift 2 ;;
    --repo) REPO="$2"; shift 2 ;;
    --rev) REV="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$DATA" ] || { echo "--data is required" >&2; exit 2; }
mkdir -p "$DATA/releases"
DATA="$(cd "$DATA" && pwd)"
COMMIT="$(git -C "$REPO" rev-parse "$REV^{commit}")"
DEST="$DATA/releases/$COMMIT"
if [ ! -d "$DEST" ]; then
  TMP="$DATA/releases/.tmp-$COMMIT-$$"
  mkdir -p "$TMP"
  trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
  git -C "$REPO" archive "$COMMIT" | tar -x -C "$TMP"
  LOGV="$(sed -n 's/^(def version \([0-9]*\)).*/\1/p' "$TMP/world/src/knowledge/world/log/segment.clj")"
  VERSION="$(git -C "$REPO" describe --tags --always "$COMMIT")"
  printf '{:commit "%s" :version "%s" :log-version %s :built-at "%s"}\n' \
    "$COMMIT" "$VERSION" "${LOGV:-0}" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$TMP/release.edn"
  # a stray edit fails: only the working copy changes
  chmod -R a-w "$TMP"
  chmod u+w "$TMP"
  mv "$TMP" "$DEST"
  chmod a-w "$DEST"
  trap - EXIT
fi
# The holder is a native binary built outside the tree (make holder): it runs from the data dir, so a
# release that has none keeps the one already here. A running holder is untouched until a full stop.
for H in "$REPO/holder/build/holder" "$SELF/holder/build/holder"; do
  if [ -x "$H" ]; then
    mkdir -p "$DATA/bin"
    cp "$H" "$DATA/bin/holder.new" && mv "$DATA/bin/holder.new" "$DATA/bin/holder"
    break
  fi
done
[ -f "$DATA/release" ] || echo "$COMMIT" > "$DATA/release"
if [ ! -d "$DATA/src" ]; then
  git clone -q "$REPO" "$DATA/src"
  git -C "$DATA/src" checkout -q -b work "$COMMIT"
fi
echo "$COMMIT"
