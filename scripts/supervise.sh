#!/bin/bash
# The parent of a world (docs/self-hosting.md). A world cannot restart itself, it can only ask:
# world.restart writes <data>/restart.request and exits 75, and this starts the release it names.
#   KE_PORT=7777 scripts/supervise.sh <data-dir>            run, in the foreground
#   scripts/supervise.sh --stop <data-dir>                  stop the supervisor and its world
# A release is good once it has passed the health check and stayed up KE_SETTLE_S seconds. Anything
# else on a new release goes back to the last good one, by itself, and the world that comes up tells
# the person. A crash on a good release restarts with backoff. A clean stop stays stopped.
# Knobs: KE_HEALTH_S, KE_SETTLE_S, KE_CRASH_MAX; <data>/world.env is sourced for the Model's key.
# It also keeps <data>/bin/holder running, which holds the terminals through a restart.
set -u
if [ "${1:-}" = "--stop" ]; then
  P="$(cat "$2/supervisor.pid" 2>/dev/null || true)"
  if [ -n "$P" ] && kill "$P" 2>/dev/null; then
    echo "supervisor $P stopping"
    # back only when it has gone, so what follows a stop finds the port and the pidfile free
    for _ in $(seq 1 60); do kill -0 "$P" 2>/dev/null || exit 0; sleep 1; done
    echo "supervisor $P did not stop in 60 s"; exit 1
  fi
  echo "no supervisor for $2"
  exit 0
fi
[ -n "${1:-}" ] || { echo "usage: supervise.sh <data-dir>"; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
D="$(cd "$1" && pwd)"
[ -f "$D/release" ] || { echo "no $D/release: run scripts/release.sh --data $D first"; exit 2; }
# one supervisor per world: a second one must not take the pidfile from the one that is running
if P="$(cat "$D/supervisor.pid" 2>/dev/null)" && [ -n "$P" ] && kill -0 "$P" 2>/dev/null; then
  echo "supervisor $P already runs $D"; exit 3
fi
[ -f "$D/world.env" ] && source "$D/world.env"
[ -z "${JAVA_HOME:-}" ] && [ -f "$HOME/opt/env.sh" ] && source "$HOME/opt/env.sh"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@25}"
export PATH="$JAVA_HOME/bin:$HOME/opt/clojure/bin:/opt/homebrew/bin:$PATH"
export KE_PORT="${KE_PORT:-7777}" KE_DATA="$D" KE_WORK="$D/src"
# the classpath cache cannot live in a read-only release
export CLJ_CACHE="$D/cpcache"
HEALTH_S="${KE_HEALTH_S:-120}"; SETTLE_S="${KE_SETTLE_S:-30}"; CRASH_MAX="${KE_CRASH_MAX:-3}"
RESTART_CODE=75
mkdir -p "$D/logs"
echo $$ > "$D/supervisor.pid"
say() { echo "supervise $(date -u +%H:%M:%SZ) $*" | tee -a "$D/logs/supervise.log"; }

CHILD=""; STOPPING=""
stop() { STOPPING=1; [ -n "$CHILD" ] && kill "$CHILD" 2>/dev/null; }
trap stop TERM INT
# closing the Terminal that started this does not stop the world
trap '' HUP

# The terminal holder (docs/panes.md): it outlives every restart of the world and ends with this
# script. It gets a bare environment, so the Model's key is not in what it can pass on.
SOCK="$D/holder.sock"
if [ "${#SOCK}" -ge 100 ]; then
  SOCK="$(mktemp -d /tmp/ke-holder.XXXXXX)/holder.sock" # a unix socket path has a short limit
fi
export KE_HOLDER_SOCK="$SOCK"
holder_alive() { [ -f "$D/holder.pid" ] && kill -0 "$(cat "$D/holder.pid")" 2>/dev/null; }
start_holder() {
  [ -x "$D/bin/holder" ] || return 0
  holder_alive && return 0
  env -i PATH="$PATH" HOME="$HOME" "$D/bin/holder" "$SOCK" >> "$D/logs/holder.log" 2>&1 &
  echo $! > "$D/holder.pid"
  disown
  for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.1; done
  say "holder $(cat "$D/holder.pid")"
}
stop_holder() {
  holder_alive && kill "$(cat "$D/holder.pid")" 2>/dev/null
  rm -f "$D/holder.pid"
}

log_version() { # the format of the newest segment, big-endian u16 after the magic
  local seg; seg="$(ls "$D"/log/seg-*.klog 2>/dev/null | tail -1)"
  [ -n "$seg" ] || { echo 0; return; }
  set -- $(od -An -tu1 -j4 -N2 "$seg"); echo $(( $1 * 256 + $2 ))
}
manifest_log_version() { sed -n 's/.*:log-version \([0-9]*\).*/\1/p' "$D/releases/$1/release.edn" 2>/dev/null; }
outcome() { # result from to reason
  printf 'result=%s\nfrom=%s\nto=%s\nreason=%s\n' "$1" "$2" "$3" "$4" > "$D/restart.outcome.tmp"
  mv "$D/restart.outcome.tmp" "$D/restart.outcome"
}
refused() { # why a release cannot run here, or nothing
  [ -f "$D/releases/$1/release.edn" ] || { echo "no such release"; return; }
  local want have; want="$(manifest_log_version "$1")"; have="$(log_version)"
  [ "${want:-0}" -ge "$have" ] || echo "log format $have is newer than the release's ${want:-0}"
}
back() { # why: return to the last good release; what the failed one wrote as a cache goes with it
  local bad="$1" why="$2" good; good="$(cat "$D/release.good" 2>/dev/null || true)"
  if [ -z "$good" ] || [ "$good" = "$bad" ]; then return 1; fi
  say "back to ${good:0:12}: $why"
  if [ -d "$D/snapshots" ] && [ -f "$D/.started" ]; then
    mkdir -p "$D/snapshots.rolled-back"
    find "$D/snapshots" -type f -newer "$D/.started" -exec mv {} "$D/snapshots.rolled-back/" \;
  fi
  echo "$good" > "$D/release"
  outcome rolled-back "$bad" "$good" "$why"
  return 0
}

CRASHES=0; BACKOFF=1
while [ -z "$STOPPING" ]; do
  CUR="$(cat "$D/release")"
  why="$(refused "$CUR")"
  if [ -n "$why" ]; then
    back "$CUR" "$why" && continue
    say "cannot start ${CUR:0:12}: $why"; exit 1
  fi
  start_holder
  say "starting ${CUR:0:12} on $KE_PORT"
  ( "$(dirname "$0")/backup-world.sh" "$D" >> "$D/logs/supervise.log" 2>&1 & )
  touch "$D/.started"
  START=$SECONDS
  ( cd "$D/releases/$CUR/world" && export KE_HARNESS_ROOT="$D/releases/$CUR" KE_RELEASE="$CUR" && exec clojure -M:run ) >> "$D/logs/world.log" 2>&1 &
  CHILD=$!
  HEALTHY=""; GOOD=""
  while kill -0 "$CHILD" 2>/dev/null; do
    sleep 1
    start_holder # its terminals are gone with it; the next ones need one
    if [ -z "$HEALTHY" ]; then
      if python3 "$HERE/world-health.py" "$KE_PORT" "$D/root.token" >/dev/null 2>&1; then
        HEALTHY=1; say "healthy ${CUR:0:12} after $((SECONDS-START))s"
      elif [ $((SECONDS-START)) -ge "$HEALTH_S" ]; then
        say "no health from ${CUR:0:12} in ${HEALTH_S}s"; kill "$CHILD" 2>/dev/null; break
      fi
    elif [ -z "$GOOD" ] && [ $((SECONDS-START)) -ge "$SETTLE_S" ]; then
      GOOD=1; echo "$CUR" > "$D/release.good"; CRASHES=0; BACKOFF=1; say "good ${CUR:0:12}"
    fi
  done
  wait "$CHILD"; CODE=$?
  CHILD=""
  [ -n "$STOPPING" ] && break
  if [ "$CODE" = "$RESTART_CODE" ]; then
    # a restart counts as settled: it asked to go
    [ -n "$HEALTHY" ] && echo "$CUR" > "$D/release.good"
    REQ="$D/restart.request"
    NEXT="$(sed -n 's/^release=//p' "$REQ" 2>/dev/null)"; REASON="$(sed -n 's/^reason=//p' "$REQ" 2>/dev/null)"
    rm -f "$REQ"
    NEXT="${NEXT:-$CUR}"
    why="$(refused "$NEXT")"
    if [ -n "$why" ]; then
      say "refused ${NEXT:0:12}: $why"; outcome refused "$NEXT" "$CUR" "$why"; continue
    fi
    echo "$NEXT" > "$D/release"
    outcome restarted "$CUR" "$NEXT" "$REASON"
    say "restart to ${NEXT:0:12}: $REASON"
    continue
  fi
  case "$CODE" in 0|130|143) if [ -n "$HEALTHY" ]; then say "world stopped ($CODE)"; break; fi ;; esac
  # a crash, or a failed start
  if [ -z "$GOOD" ]; then
    why="exit $CODE before it was good"; [ -z "$HEALTHY" ] && why="failed its health check (exit $CODE)"
    back "$CUR" "$why" && continue
  fi
  CRASHES=$((CRASHES+1))
  say "crash $CRASHES of ${CUR:0:12} (exit $CODE); again in ${BACKOFF}s"
  if [ "$CRASHES" -ge "$CRASH_MAX" ] && [ -z "$GOOD" ]; then say "giving up"; exit 1; fi
  sleep "$BACKOFF"
  BACKOFF=$(( BACKOFF < 60 ? BACKOFF * 2 : 60 ))
done
stop_holder
say "stopped"
rm -f "$D/supervisor.pid"
