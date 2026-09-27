#!/usr/bin/env bash
# Audit every page of the shipped layout, the way it actually draws — the studio's --audit.
#
#   apps/studio-jf/tools/layout/audit_pages.sh [board] > report.txt      (board: jaytek_v1)
#
# Opens the demo project offline (HOME is a throwaway folder, exactly as manual/tools/shoot.sh does),
# visits every navigation node one per tick and measures what the layout produced: things off the
# page, controls outside their container, captions that do not fit their label, overlaps, controls the
# keyboard cannot reach, bindings that name nothing. Prints the findings, one per line.
set -euo pipefail
BOARD="${1:-jaytek_v1}"
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
STUDIO="${STUDIO_BIN:-$ROOT/apps/studio-jf/build/studio}"
GUI="$ROOT/definition/boards/$BOARD.dashboard.gui"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
DATA="$T/.local/share/jayecu/jayecu Studio"
mkdir -p "$DATA/dashboards" "$T/.config/jayecu-studio"
cp "$GUI" "$DATA/dashboards/$BOARD.gui"
echo '{ "saveOnExit": 2, "window.w": 1600, "window.h": 1000 }' > "$T/.config/jayecu-studio/settings.json"

python3 - "$GUI" > "$T/pages.txt" <<'PY'
import json, sys
g = json.load(open(sys.argv[1]))
def walk(n, path):
    name = n.get("name") or n.get("label") or n.get("title", "")
    p = f"{path}/{name}" if path else name
    print(p)
    for c in n.get("children", []): walk(c, p)
for n in g.get("tree", []): walk(n, "")
PY
N=$(wc -l < "$T/pages.txt")
# One page per tick at 4 Hz, measured on the next: allow three ticks a page and a margin.
QUIT=$(( (N * 3 + 60) * 250 ))
HOME="$T" timeout $(( QUIT / 1000 + 120 )) xvfb-run -a -s "-screen 0 1600x1000x24" \
    "$STUDIO" --open-meta "$ROOT/shared/tuneit-meta.json" --audit "$T/pages.txt" --quit-after "$QUIT" \
    > "$T/log" 2>&1 || true
echo "# $N pages"
grep "ui.audit" "$T/log" | sed 's/^\[[A-Z]*\]\[ui.audit\] //'
