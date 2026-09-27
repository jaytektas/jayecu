"""A page per SENSOR GROUP: the inputs of one kind, with what each is reading right now.

The switchboard answers "what is this car wired for" for all 122 inputs at once, and it has to be dense
to do that — a name and a tick, five columns of them. That density is the price of being the index, and
it is the wrong shape for the other question an installer asks constantly: are the engine's sensors
actually reading sensible numbers?

So the tree gains a node per catalogue group, and a group's page has room to show, for each of its
inputs, the enable, the way in, and the LIVE READING with its units. Twelve rows instead of a hundred and
twenty-two means each row can carry the number, and a group is small enough that a wrong one stands out.

The groups are the catalogue's own — the same grouping the firmware uses and the switchboard already
prints as headings — so nothing here invents a taxonomy.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from ruler import ruler
from author import C_DIM

COL_W, GAP, ROW_H = 410, 12, 26
COLS = 3


def page(title, items, link_of, sig_of, tel):
    """One group. `items` — [(sensor id, name)]; `link_of(sid, name)` — its node path or ''; `sig_of(sid)`
    — the telemetry channel it publishes, or ''; `tel` — the meta's telemetry table, for units and
    precision (a reading printed at the channel's own digits, so a lambda is not shown to three places
    and a temperature is not shown to none)."""
    p = A.Page(title=title)
    y0 = p.head(f'{len(items)} input{"" if len(items) == 1 else "s"} of this kind. Tick what the car has; '
                f'the name opens its calibration, wiring and checks. The number on the right is what the '
                f'ECU is reading from it now — blank means the input publishes nothing.')

    # SPREAD ACROSS THE PAGE RATHER THAN DOWN IT. The page would hold twenty-two rows in a column, but a
    # group of thirty-five as two long columns leaves a third of the page blank, and a group of ten as one
    # column leaves two thirds. Aiming for three columns — with a floor, so a group of four does not
    # become three columns of one — spends the width the page has and keeps every name a short glance
    # from the last.
    fit = max(1, (A.CANVAS_H - y0 - 8 - A.PANEL_TITLE - 16) // ROW_H)
    rows_per_col = min(fit, max(6, (len(items) + COLS - 1) // COLS))
    ncols = min(COLS, max(1, (len(items) + rows_per_col - 1) // rows_per_col))
    per_col = (len(items) + ncols - 1) // ncols
    h = A.panel_h(per_col, ROW_H, top=8, bottom=8)

    # THE NAME COLUMN IS AS WIDE AS THE WIDEST NAME IN THIS GROUP. It was a flat 218px, which every
    # catalogue name fits but one: "Flex Fuel Composition & Temperature" is 242px and came out as
    # "Flex Fuel Composition &" — clipped mid-name with nothing to say so. The reading beside it gives
    # up the difference, since a temperature or a percent never needed 140px to print.
    # (+8: a caption is drawn one 3px inset in from each edge of its box.)
    name_x = 10 + A.CHECK_H + A.CHECK_GAP
    name_w = max(218, min(int(max(ruler().width(n) for _s, n in items)) + 8,
                          COL_W - name_x - 8 - 100 - 10))
    val_w = COL_W - name_x - name_w - 8 - 10

    for c in range(ncols):
        chunk = items[c * per_col:(c + 1) * per_col]
        if not chunk:
            break
        box = p.panel(10 + c * (COL_W + GAP), y0, COL_W, h,
                      title if ncols == 1 else f'{title}  ({c + 1} of {ncols})')
        y = 8
        for sid, name in chunk:
            on = f'[#sensors.sensor[{sid}].enabled] == 1'
            g = p.group()
            p.add(p._new('checkbox', 10, y + 3, A.CHECK_H, A.CHECK_H, {'signalName': f'sensors.sensor[{sid}].enabled'}, g),
                  into=box)
            lp = {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL, 'enableCondition': on}
            link = link_of(sid, name)
            if link:
                lp['link'] = link
            p.add(p._new('label', name_x, y + 2, name_w, A.LBL_H, lp, g), into=box)
            sig = sig_of(sid)
            if sig:
                t = tel.get(sig, {})
                # The reading is only shown while the input is ON. A disabled sensor's channel holds
                # whatever it last published, and a number that is not being updated is worse than no
                # number: it reads as a live value.
                #
                # No unit label beside it: the value widget prints the unit itself, and a second one
                # after it read "82.2 %  %".
                p.add(p._new('value', name_x + name_w + 8, y + 1, val_w, A.LBL_H,
                             {'signalName': sig, 'format': f'%.{t.get("digits", 1)}f', 'align': 'Left',
                              'fontName': A.FONT_LBL, 'displayUnit': 'Auto', 'condition': on}, g), into=box)
            y += ROW_H
    return p
