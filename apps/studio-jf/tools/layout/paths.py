"""Where the layout toolchain's inputs and outputs live. One definition, resolved from the repo.

Every one of these used to be an absolute path into one developer's home directory, repeated in six files:
the dashboard in a studio's live directory under one ECU's serial number, the meta in shared/.
Nobody else could run the scripts, the shipped artifact was version controlled nowhere, and the
board the pages were authored against was whichever one happened to be built last.

  BOARD   which board the pages are being authored for        (JAYECU_BOARD, default jaytek_v1)
  DOC     the dashboard for that board — AUTHORED, in git     (JAYECU_DASHBOARD to override)
  META    the descriptor the pages bind against               (JAYECU_META to override)
  GEN     generated/, for the few scripts that read a header

THE META IS NOT PER-BOARD ON DISK, WHICH IS THE TRAP. codegen writes shared/tuneit-meta.json for
whichever board it was last run for, so authoring jaytek_v1's pages straight after a proteus_f7
build would silently bind them to proteus_f7's channels. check_board() reads the board out of the
meta and refuses the mismatch, because the pages that come out of it look perfectly fine.
"""
import json
import os
import pathlib

REPO  = pathlib.Path(__file__).resolve().parents[4]
BOARD = os.environ.get('JAYECU_BOARD', 'jaytek_v1')

DOC = os.environ.get('JAYECU_DASHBOARD') or str(REPO / 'definition' / 'boards' / f'{BOARD}.dashboard.gui')
META = os.environ.get('JAYECU_META') or str(REPO / 'shared' / 'tuneit-meta.json')
GEN = str(REPO / 'generated')


def check_board(meta: dict | None = None) -> str:
    """Confirm the meta on disk is the board we are authoring for. Returns the board name."""
    if meta is None:
        raw = pathlib.Path(META).read_bytes()
        meta = json.loads(raw[:-4] if raw[-1:] != b'}' else raw)
    got = meta.get('meta', {}).get('board')
    if got != BOARD:
        raise SystemExit(
            f"  {META}\n"
            f"  is {got}'s meta, but JAYECU_BOARD is {BOARD}.\n"
            f"  run: make codegen BOARD={BOARD}   (or set JAYECU_BOARD={got})")
    return got
