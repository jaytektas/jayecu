#!/usr/bin/env bash
# The screenshots of chapter 3 (Installing jayecu Studio), reproducibly. Re-run after a UI change:
#   manual/tools/shots/installing-studio.sh
#
# This chapter photographs a FIRST START and the PREFERENCES DIALOG, which shoot.sh cannot reach: shoot.sh
# always opens a demo project, selects a navigation page and lets the studio capture its own main window
# (--shot), and a modal dialog is a window of its own. So this script starts the studio under Xvfb with a
# throwaway HOME (as shoot.sh does), drives the mouse with xdotool, and photographs the X screen with
# ImageMagick. Coordinates are for the 1600x1000 window shoot.sh also uses. --quit-after makes it a
# scripted run, so the studio makes no update check and touches no network.
#
# Needs: a built studio (make studio), Xvfb (xvfb-run), xdotool, ImageMagick.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
STUDIO="${STUDIO_BIN:-$ROOT/apps/studio-jf/build/studio}"
META="$ROOT/shared/tuneit-meta.json"
OUT="$ROOT/manual/docs/img/studio"

fail() { echo "installing-studio shots: $*" >&2; exit 1; }
[ -x "$STUDIO" ] || fail "no studio at $STUDIO — run make studio"
for t in xvfb-run xdotool import convert; do command -v "$t" >/dev/null || fail "no $t"; done
mkdir -p "$OUT"

# drive <fresh|demo> '<shell snippet>'
#   fresh: an empty data folder and nothing open — what a new user sees.
#   demo:  the offline demo project shoot.sh opens (the shipped jaytek_v1 layout).
# The snippet runs once the window has settled; it sees $OUT and $TMP.
drive() {
    local MODE="$1" SNIPPET="$2"
    local TMP; TMP="$(mktemp -d)"
    mkdir -p "$TMP/.config/jayecu-studio"
    echo '{ "saveOnExit": 2, "window.w": 1600, "window.h": 1000, "window.x": 0, "window.y": 0, "dock.dtc": 0, "dock.properties": 0 }' \
        > "$TMP/.config/jayecu-studio/settings.json"
    local ARGS=()
    if [ "$MODE" = demo ]; then
        local DATA="$TMP/.local/share/jayecu/jayecu Studio"
        mkdir -p "$DATA/dashboards"
        cp "$ROOT/definition/boards/jaytek_v1.dashboard.gui" "$DATA/dashboards/jaytek_v1.gui"
        ARGS=(--open-meta "$META")
    fi
    cat > "$TMP/run.sh" <<EOF
"$STUDIO" ${ARGS[@]+"${ARGS[@]}"} --quit-after 60000 > "$TMP/studio.log" 2>&1 &
SP=\$!
sleep 10                                                  # the window opens and settles
MAIN=\$(xdotool search --name "studio" | head -1)
xdotool windowfocus --sync "\$MAIN"; sleep 0.5            # no window manager: focus by hand
OUT="$OUT"; TMP="$TMP"
$SNIPPET
kill \$SP 2>/dev/null || true
EOF
    HOME="$TMP" timeout 120 xvfb-run -a -s "-screen 0 1600x1000x24" bash -e "$TMP/run.sh" \
        || { tail -20 "$TMP/studio.log" >&2; rm -rf "$TMP"; fail "the driven run failed"; }
    rm -rf "$TMP"
}

# Figure 3.4 — the first start: an empty data folder, nothing open.
drive fresh '
    import -window root "$TMP/first.png"
    convert "$TMP/first.png" "$OUT/installing-studio-first-start.png"
'
echo "shot: manual/docs/img/studio/installing-studio-first-start.png"

# Figure 3.5 — Edit > Preferences > Updates.
drive demo '
    xdotool mousemove 59 46 click 1; sleep 1.5              # Edit
    xdotool mousemove 84 273; sleep 0.5; xdotool click 1    # Preferences...
    sleep 3
    PREF=$(xdotool search --name "^Preferences$" | head -1)
    [ -n "$PREF" ] || { echo "no Preferences window" >&2; exit 1; }
    xdotool windowfocus --sync "$PREF"; sleep 0.3
    xdotool mousemove 470 420 click 1; sleep 1.5            # Updates, in the left rail
    import -window root "$TMP/prefs.png"
    convert "$TMP/prefs.png" -crop 720x560+440+220 +repage "$OUT/installing-studio-updates.png"
'
echo "shot: manual/docs/img/studio/installing-studio-updates.png"

# No shot of the Help menu yet: its F1 shortcut is drawn as "JKey" (JFramework MenuSystem.h,
# JMenuShortcut::toString names no F-keys). Add one here once that is fixed.
