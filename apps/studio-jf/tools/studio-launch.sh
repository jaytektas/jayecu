#!/usr/bin/env bash
# Launch the studio under gdb so a crash leaves a BACKTRACE instead of a window that vanished.
#
# This is what the desktop launcher runs. gdb sits above the process doing nothing until something
# goes wrong; on an abnormal exit it dumps every thread's stack into the session log, and the log is
# renamed CRASH-<stamp>.log so it stands out from the ordinary ones. Without this a crash from the
# launcher leaves nothing at all: there is no terminal to print to, and a rebuild voids the apport
# report that would otherwise have caught it.
#
# The app's own JLOGC output goes into the same file, ahead of the trace — which is usually the half
# that says what it was doing. stdbuf keeps that output line-buffered so the last lines before the
# fault are actually on disk rather than sitting in a block buffer that dies with the process.
#
#   STUDIO_BIN=/path/to/other  — override the binary (used by the wrapper's own test)
#   STUDIO_LOG_DIR=/path       — override where logs are kept
#   STUDIO_NO_GDB=1            — launch bare, no debugger

set -u

# FRAME TIMING IN EVERY SESSION LOG. "It has gone slow" is unanswerable after the fact unless the numbers
# were being written down while it was slow — and the answer is always one of two very different things:
# BUILD (the widget tree, i.e. the studio's own code) or SUBMIT (the GPU and the present). The perf
# category prints both every couple of seconds, at a cost of a few lines a minute in a log that is kept
# anyway. JF_LOG only RAISES levels, so an existing setting is preserved and nothing is silenced.
export JF_LOG="${JF_LOG:+$JF_LOG,}perf=info"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${STUDIO_BIN:-$HERE/../build/studio}"
LOG_DIR="${STUDIO_LOG_DIR:-$HOME/.local/share/jayecu/jayecu Studio/logs}"
KEEP=10                          # session logs to keep; CRASH-*.log are never pruned

[ -x "$BIN" ] || { echo "studio-launch: no binary at $BIN" >&2; exit 127; }

# No gdb (or explicitly not wanted): the app still has to start. A missing debugger is a worse log,
# never a launcher that does nothing.
if [ -n "${STUDIO_NO_GDB:-}" ] || ! command -v gdb >/dev/null 2>&1; then
    exec "$BIN" "$@"
fi

mkdir -p "$LOG_DIR" || exec "$BIN" "$@"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"     # pid too: two launches can share a second
LOG="$LOG_DIR/studio-$STAMP.log"
MARK="===== ABNORMAL EXIT — thread backtraces follow ====="

GDB_CMDS=$(mktemp /tmp/studio-gdb.XXXXXX)
trap 'rm -f "$GDB_CMDS"' EXIT
cat > "$GDB_CMDS" <<EOF
set pagination off
set confirm off
# Never reach for the network mid-crash, and keep its banner out of the log.
set debuginfod enabled off
set print thread-events off
# A GUI app takes these in normal operation; stopping on them would freeze it under the debugger.
handle SIGPIPE nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
run
# \$_exitcode is void when the inferior did NOT exit of its own accord — it took a fatal signal.
if \$_isvoid(\$_exitcode)
  echo \n$MARK\n
  info program
  echo \n--- all threads, full frames ---\n
  thread apply all bt full
  echo \n--- registers (crashing thread) ---\n
  info registers
end
quit
EOF

# stdbuf's LD_PRELOAD is inherited by the inferior, so the app's own stderr is line-buffered too.
stdbuf -oL -eL gdb -q -batch -x "$GDB_CMDS" --args "$BIN" "$@" > "$LOG" 2>&1
status=$?

if grep -q "$MARK" "$LOG" 2>/dev/null; then
    CRASH="$LOG_DIR/CRASH-$STAMP.log"
    mv "$LOG" "$CRASH"
    echo "studio-launch: crashed — backtrace in $CRASH" >&2
    status=1                     # gdb's own exit code says nothing about the inferior's fate
    # Say so on the desktop as well: nothing else is watching this stderr.
    command -v notify-send >/dev/null 2>&1 &&
        notify-send -u critical "jayECU Studio crashed" "Backtrace: $CRASH"
else
    # Ordinary session: keep the newest few for context, drop the rest. Crash logs are never pruned.
    ls -1t "$LOG_DIR"/studio-*.log 2>/dev/null | tail -n +$((KEEP + 1)) | while read -r old; do
        rm -f "$old"
    done
fi

exit $status
