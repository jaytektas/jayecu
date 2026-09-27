#!/bin/bash
# Hammer the quit-with-unsaved-changes path on the RELEASE build.
#   S=/dir overrides where the per-run logs go (default: a fresh temp directory).
cd "$(dirname "$0")/.." || exit 1
S=${S:-$(mktemp -d)}
export DISPLAY=:99 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
N=${1:-5}
for i in $(seq 1 $N); do
    LOG=$S/loop_$i.log
    script -q -e -c "./build/studio --dirty --quit-after 4000" /dev/null > "$LOG" 2>&1 &
    PID=$!
    WIN=""
    for j in $(seq 1 40); do
        sleep 0.5
        WIN=$(xdotool search --name "Unsaved changes" 2>/dev/null | head -1)
        [ -n "$WIN" ] && break
    done
    if [ -n "$WIN" ]; then
        sleep 0.4
        xdotool key --window "$WIN" Return
    fi
    for j in $(seq 1 30); do kill -0 $PID 2>/dev/null || break; sleep 0.5; done
    STATE="exited-ok"
    if kill -0 $PID 2>/dev/null; then kill -9 $PID; STATE="HUNG"; fi
    wait $PID 2>/dev/null; RC=$?
    SURF=$(grep -c "Swapchain surface 1 420x168" "$LOG")
    CRASH=$(grep -ciE "double free|corruption|signal 6|Aborted|backtrace" "$LOG")
    echo "run $i: $STATE rc=$RC  dialog-surfaces=$SURF  crash-markers=$CRASH"
done
