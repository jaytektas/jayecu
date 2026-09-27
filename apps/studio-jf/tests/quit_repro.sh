#!/bin/bash
# Reproduce the quit-with-unsaved-changes crash headlessly under ASan.
#   $1 = which answer to give: discard | save | cancel
#   S=/dir overrides where logs and screenshots go (default: a fresh temp directory).
cd "$(dirname "$0")/.." || exit 1
S=${S:-$(mktemp -d)}
ANSWER=${1:-discard}
LOG=$S/quit_$ANSWER.log
rm -f "$LOG"

export DISPLAY=:99
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=$S/asan_$ANSWER

script -q -e -c "LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.8 ./build-asan/studio --dirty --quit-after 5000" /dev/null > "$LOG" 2>&1 &
PID=$!

# Wait for the prompt window to exist (it is a borderless child window titled "Unsaved changes").
WIN=""
for i in $(seq 1 60); do
    sleep 0.5
    WIN=$(xdotool search --name "Unsaved changes" 2>/dev/null | head -1)
    [ -n "$WIN" ] && break
    kill -0 $PID 2>/dev/null || { echo "studio exited before the prompt appeared"; break; }
done

if [ -n "$WIN" ]; then
    echo "prompt window = $WIN"
    xdotool windowactivate --sync "$WIN" 2>/dev/null
    sleep 0.5
    import -window "$WIN" $S/prompt_$ANSWER.png 2>/dev/null
    eval "$(xdotool getwindowgeometry --shell "$WIN")"
    echo "dialog geom: X=$X Y=$Y W=$WIDTH H=$HEIGHT"
    case "$ANSWER" in
        save)    xdotool key --window "$WIN" Return ;;   # Accept
        cancel)  xdotool key --window "$WIN" Escape ;;   # Reject
        discard) # Destructive button — click it. It sits left in the footer.
                 eval "$(xdotool getwindowgeometry --shell "$WIN")"
                 xdotool mousemove $((X + 185)) $((Y + 148)) click 1 ;;
        cancel-then-save)
                 # Cancel the first prompt, then ask to quit AGAIN — two dialogs in one session, which is
                 # what a real user does when they change their mind.
                 xdotool key --window "$WIN" Escape
                 sleep 1.5
                 MAIN=$(xdotool search --name "jayecu" 2>/dev/null | head -1)
                 echo "main window = $MAIN"
                 xdotool windowactivate --sync "$MAIN" 2>/dev/null
                 xdotool key --window "$MAIN" ctrl+q
                 sleep 1.5
                 WIN2=$(xdotool search --name "Unsaved changes" 2>/dev/null | head -1)
                 echo "second prompt = $WIN2"
                 [ -n "$WIN2" ] && { xdotool windowactivate --sync "$WIN2"; sleep 0.4; xdotool key --window "$WIN2" Return; } ;;
    esac
else
    echo "NO PROMPT WINDOW FOUND"
fi

# Let it finish (or hang), then make sure it is gone.
for i in $(seq 1 40); do kill -0 $PID 2>/dev/null || break; sleep 0.5; done
if kill -0 $PID 2>/dev/null; then echo "STILL RUNNING — killing"; kill -9 $PID; fi
wait $PID 2>/dev/null
echo "exit=$?"
echo "--- asan reports ---"
cat $S/asan_$ANSWER.* 2>/dev/null | head -40
echo "--- log tail ---"
tail -6 "$LOG"
