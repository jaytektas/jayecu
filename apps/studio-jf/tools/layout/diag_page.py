"""The Diagnostics workspace: no viewport at all, just the hardware saying what it sees.

Every other tab is built around a viewport, because it is for CHANGING something. This one is for
answering "is the hardware alright", which is a different question and wants no editing surface at all —
so it spends the whole canvas on readouts, grouped by subsystem, in the order you work down when
something is wrong: is it turning over, are the raw inputs sane, are the outputs doing as they are told.

The groups come from the telemetry descriptor's own `module` field, so this cannot drift from the
firmware: a channel added to a subsystem appears here, and one that moves subsystem moves here too.
"""
import json
import author as A
from author import C_DIM

META = None


def meta():
    global META
    if META is None:
        s = open(__import__('paths').META, encoding='utf-8', errors='replace').read()
        META = json.JSONDecoder().raw_decode(s, 0)[0]
    return META


def _chans():
    t = meta()['telemetry']
    return dict(t.items()) if isinstance(t, dict) else {x['id']: x for x in t}


def _natural(name):
    """Sort key that reads the digits in a channel id as NUMBERS.

    Plain string order puts out_10 between out_1 and out_2, so the outputs column ran
    1,10,11,...,19,2,20,... — and the same happens to every per-cylinder and per-slot group on this
    page (egt_1..12, wideband_1..12). The list is read down the column by someone looking for one
    output; scattering the order is the one thing that makes that slow.
    """
    out, num = [], ''
    for c in str(name):
        if c.isdigit():
            num += c
        else:
            if num: out.append((1, int(num), '')); num = ''
            out.append((0, 0, c))
    if num: out.append((1, int(num), ''))
    return out


# THE SUB-VIEWS. One tab, a chooser, and a page per subject — because the alternative was cramming 380
# channels into five columns, showing 157 of them and counting the rest in a footnote, which is the one
# thing a diagnostics page must not do.
#
# Sub-tabs need no new widget: a panel stacked per view, each gated on a host variable the chooser writes
# (see Page.subtabs). Splitting by SUBJECT rather than by overflow means the view you pick is usually the
# whole reason you opened the tab.
#
# The groups inside each view are the telemetry descriptor's own `module` field, so this cannot drift from
# the firmware: a channel added to a subsystem appears here, and one that moves subsystem moves here too.
VIEWS = [
    ('Engine & Trigger',   lambda m: m in ('Trigger', 'Engine', 'Engine Sync', 'Sensors - Engine Synchronous')),
    ('Fuel & Spark',       lambda m: m in ('FuelCalculator', 'Ignition', 'Lambda', 'Injection', 'Knock',
                                           'Misfire', 'TransientThrottle', 'Sensors - O2')),
    ('Air & Throttle',     lambda m: m in ('ElectronicThrottle', 'Idle', 'Stepper', 'Boost', 'Boost/Turbo',
                                           'Sensors - Boost/Turbo', 'Sensors - Atmospheric')),
    # OUTPUTS AND BRIDGES HAD NO VIEW AT ALL, so 46 of the 438 channels were never placed and the tab
    # reported them "DROPPED" every install — 42 output states and the four H-bridge channels, which is
    # the half of the diagnostics that says what the ECU is DRIVING. Two modules, one view: what a
    # generic output is doing now, and what the bridges under the throttle and the stepper are doing.
    ('Outputs & Bridges',  lambda m: m in ('Outputs', 'HBridge')),
    ('Sensors — Raw',      lambda m: m == 'Sensors - Raw'),
    ('Sensors — Engine',   lambda m: m in ('Sensors - Engine', 'Sensors - Air Con')),
    # TRACTION HAD NO VIEW EITHER — four channels the tab has been reporting DROPPED since the traction
    # rebuild, which is the whole of what that module says about itself. It belongs with the other
    # vehicle-dynamics readings rather than with the sensors that feed it.
    ('Vehicle & Traction', lambda m: m in ('Vehicle - Traction', 'Vehicle')),
    ('Sensors — Vehicle',  lambda m: m in ('Sensors - Chassis', 'Sensors - Transmission',
                                           'Sensors - Vehicle Speed', 'Sensors - Derived',
                                           'Sensors - Nitrous', 'Sensors - Other')),
    ('Protection & Faults', lambda m: m in ('Diagnostics', 'EngineProtection', 'RevLimiter', 'Launch',
                                            'Lua', 'Actuators', 'Alternator', 'TachOutput')),
    # THE BUS ITSELF, which had no view — so every CAN health channel would be reported DROPPED, the
    # same way Outputs and Traction were. It gets its own rather than joining Protection: "is the wire
    # alright" is a question you ask with a meter in your hand, not one you ask about the engine, and
    # the answer is per-bus. Load, throughput, the controller's error counters and the number of times
    # each bus has dropped out. There is no collision count because CAN has no collisions.
    ('CAN Bus',            lambda m: m == 'CAN'),
]

# ROOM, NOW THAT THERE ARE CARDS. This was one page holding all 380 channels, so every row was squeezed
# to 19px with the small font — legible only if you already knew what you were looking for. Split across
# seven views, the widest of them has a hundred channels to place and the whole card to place them in, so
# a row is a row: label and reading at the page's own font, at the pitch every other page uses.
ROW_H  = 26        # one channel: name, reading, unit
# THE READOUT IS SHORTER THAN A CONTROL, and has to be. author.CTL_H is 30 — the right height for
# something you click into — but the rows here are pitched at 26, so a 30px box overlapped the one
# below it by 4px on every channel of every view: 435 complaints across all 487 readouts. Nothing is
# editable on this page, so nothing here needs a control-sized target; 24 sits the box around its own
# 20px label with a pixel to spare at the pitch. Raising ROW_H instead would have cost four rows a
# column and pushed channels out of the five the card allows.
VAL_H  = 24
HEAD_H = 34        # a module heading and its rule
COL_W  = 300       # nominal only: a column is sized to its own widest name (see _fill)
VAL_MAX = 224      # see below
VAL_W  = 92        # the reading
# HOW MUCH OF THE CARD ONE READING MAY TAKE. Every state label fits inside this at its authored size
# except the CAN controller's last error, whose values are sentences ("Bit recessive - something
# driving against us") — 307px, which pushed five columns to 1643 against a 1580 card. Past the
# ceiling the readout shrinks to fit rather than clipping (ValueWidget::render), and 224 is chosen so
# that the longest of them is still above the 10px floor where that shrinking stops. The full-size
# reading is on the CAN bus page, which has room for one.
UNIT_W = 38        # its unit — a FLOOR, not the width; each column sizes its own (see _fill)
COLS   = 5
TOP    = 44        # under the chooser


def _fill(p, panel, groups, ch):
    """One view: the channels of its modules, flowing top down and left to right with a heading at each
    module. Returns how many it placed, so the caller can insist that it placed all of them.

    TWO PASSES, and the reason is the channel names. A fixed 300px column left 160px for the name, and
    the telemetry descriptor's own labels do not fit it — "Exhaust Gas Temperature 10" came out as
    "Exhaust Gas Temperature", which is the one part of that name a diagnostics page must not lose, and
    "Flex Fuel Composition & Temperature (fuel_temp_flex)" lost half of itself. Nothing said so: a
    caption is clipped to its own box and the box fitted the card perfectly.

    So the flow is planned first and MEASURED second. Column membership does not depend on width at all
    (it is decided by the card's height and the row pitch), so a column can be made as wide as the widest
    name that landed in it — which is what the readouts had spare room for all along, since most views
    never fill five columns.
    """
    from ruler import ruler
    r = ruler()
    H = panel['h']
    plan, placed = [[]], 0            # plan[col] = [('H', text) | ('R', channel id)]
    y = 8

    def fits(h):
        return y + h <= H - 30

    for mod in sorted(groups):
        names = list(groups[mod])
        if not fits(HEAD_H + 2 * ROW_H):
            if len(plan) >= COLS:
                break
            plan.append([]); y = 8
        plan[-1].append(('H', mod)); y += HEAD_H
        while names:
            if not fits(ROW_H):
                if len(plan) >= COLS:
                    break
                plan.append([]); y = 8
                plan[-1].append(('H', f'{mod}  (continued)')); y += HEAD_H
            plan[-1].append(('R', names.pop(0))); y += ROW_H
            placed += 1
        if names:
            return placed, len(names)

    # A column is as wide as the widest thing in it: a name plus the readout and unit beside it, or a
    # module heading, whichever asks for more. Measured for every column FIRST, because the readings
    # now compete for the card (see the shave below) and a column cannot be placed until that is settled.
    want = []
    for column in plan:
        name_w = max((r.width(ch[n].get('label') or n) for k, n in column if k == 'R'), default=0.0)
        head_w = max((r.width(t, 16) for k, t in column if k == 'H'), default=0.0)
        # THE UNIT COLUMN SIZES ITSELF TOO, for the same reason the name does. A flat 38px fitted every
        # unit in the catalog until "cc/min" arrived and overflowed it by 5px; widening the constant
        # instead pushed five columns to 1587px against a 1580px card, because every column paid for one
        # column's unit. Measured per column, only the column holding it grows.
        unit_w = max(UNIT_W, int(max((r.width(ch[n].get('units') or '', 15)
                                      for k, n in column if k == 'R'), default=0.0)) + 8)
        # …AND SO DOES THE READING, now that a STATE channel reads in words. 92px is the width of a
        # NUMBER; "Cranking Table" is 103 and "Predicted (sensor fault)" is 158, and a column holding
        # one of those clipped it. Capped, because one reading may not have the card: past the cap the
        # readout shrinks to fit rather than clipping (ValueWidget::render).
        val_w = max(VAL_W, min(VAL_MAX, int(max((A.Page.chan_value_px(n, 16, '')
                                                 for k, n in column if k == 'R'), default=0.0)) + 16))
        want.append([name_w, head_w, unit_w, val_w])

    # THE CARD IS FIXED AND THE WORDS ARE NOT, so what does not fit is taken back off the readings —
    # widest first, down to the width of a number, and no further. Everything else on this card is a
    # name or a unit, and neither has anywhere to go: a channel called "Exhaust Gas Temperature 10"
    # loses the 10, which is the one part of it a diagnostics page must not lose. A reading that has
    # been shaved is not lost, it is smaller.
    def _total():
        return sum(int(max(n + 8 + 4 + v + 4 + u, h + 8 + 8)) + 4 + 10 for n, h, u, v in want) - 10 + 8
    while _total() > panel['w'] - 8:
        i = max(range(len(want)), key=lambda k: want[k][3])
        if want[i][3] <= VAL_W:
            break                                  # nothing left to give: the guard below says so
        want[i][3] -= 2

    x = 8
    for column, (name_w, head_w, unit_w, val_w) in zip(plan, want):
        # +8 on the measured text, because a caption is drawn one 3px inset in from each edge of its box
        # (LabelWidget::kTextPad) — a box the exact width of the words still clips the last of them.
        col_w = int(max(name_w + 8 + 4 + val_w + 4 + unit_w, head_w + 8 + 8)) + 4
        name_w = col_w - val_w - unit_w - 12
        y = 8
        for kind, item in column:
            if kind == 'H':
                p.add(p._new('label', x, y + 2, col_w - 8, 22,
                             {'labelText': item, 'align': 'Left', 'fontName': '|16|1|0'}), into=panel)
                p.add(p._new('panel', x, y + HEAD_H - 4, col_w - 8, 1,
                             {'bgColor': C_DIM, 'padding': '0'}), into=panel)
                y += HEAD_H
                continue
            e = ch[item]
            g = p.group()
            p.add(p._new('label', x, y + 3, name_w, A.LBL_H,
                         {'labelText': e.get('label') or item, 'align': 'Left',
                          'fontName': A.FONT_LBL}, g), into=panel)
            # ITS OWN precision, from the descriptor: a blanket "%.2f" printed the 1 kHz frame counter
            # as 46254220.00 and gave a cam angle transmitted in tenths a second decimal it never had.
            p.add(p._new('value', x + name_w + 4, y + 1, val_w, VAL_H,
                         {'signalName': item, 'format': f'%.{int(e.get("digits", 2))}f',
                          'fontName': A.FONT_LBL, 'align': 'Right',
                          'showUnit': '0', 'borderWidth': '1', 'borderRadius': '3'}, g), into=panel)
            if e.get('units'):
                p.add(p._new('label', x + name_w + 8 + val_w, y + 3, unit_w, A.LBL_H,
                             {'labelText': e['units'], 'align': 'Left', 'fontName': A.FONT_SMALL,
                              'fgColor': C_DIM}, g), into=panel)
            y += ROW_H
        x += col_w + 10
    if x - 10 > panel['w'] - 8:
        raise SystemExit(f'diagnostics: {len(plan)} columns need {x - 10}px, card is {panel["w"]}px')
    return placed, 0


def page():
    """The Diagnostics tab: a chooser and one page per subject, all 380 channels, nothing dropped."""
    p = A.Page(w=1600, h=800, title='Diagnostics')
    ch = _chans()
    conds = p.subtabs(10, 8, 1580, 784, 'diag_view', [nm for nm, _ in VIEWS])
    # One container per view, hidden unless its view is chosen — the channels inside are leaves, so a
    # panel is exactly the right holder for them.
    panels = [p.panel(10, TOP + 4, 1580, A.CANVAS_H if False else 800 - TOP - 12, name, cond=c)
              for (name, _), c in zip(VIEWS, conds)]

    placed = short = 0
    for (name, belongs), panel in zip(VIEWS, panels):
        groups = {}
        for n, e in sorted(ch.items(), key=lambda kv: _natural(kv[0])):
            mod = e.get('module') or 'Other'
            if belongs(mod):
                groups.setdefault(mod, []).append(n)
        got, left = _fill(p, panel, groups, ch)
        placed += got
        short += left
        if left:
            print(f'  ! {name}: {left} channel(s) did not fit')
        # WHERE THE TIME GOES, beside the two numbers that say there is a problem. MCU Load and Engine
        # Frame Load are channels like any other and flow into this view with the rest of Engine; what
        # they cannot show is WHICH of the ECU's dozen tasks is eating it, or which one is closest to
        # running out of stack. That is a table, not a gauge, so it stays a CLI command — and this is
        # the button that runs it. The reply lands in Diagnostics ▸ Console.
        if name == 'Engine & Trigger':
            p.add(p._new('command', 1180, 8, 170, 26,
                         {'labelText': 'Task Load…', 'command': 'cpu',
                          'toolTip': 'Per-task CPU share and stack headroom — the answer lands in the '
                                     'Console. "cpu reset" re-arms the frame peak.'}), into=panel)
    return p, placed, len(ch)
