"""The Sensors switchboard: every sensor the ECU has, its enable, and the way in.

The page that was here listed twenty of them, in one panel, with no links — so nineteen out of every
twenty sensors could only be reached through the tree, and the page could not answer the question a
switchboard exists to answer: what is this car actually wired for?

The catalogue is the source. Reading generated/sensors_catalog.h rather than the schema is deliberate:
the schema's 97 rows expand to the 122 the firmware really has (eight EGTs, fifteen widebands), and it is
the expanded list that has one enable each. A row this file invented would be a row with no byte behind it.

Grouped the way the catalogue groups them, because that grouping is the firmware's own and already decides
which page a sensor's detail lives on.
"""
import json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from ruler import ruler

CATALOG = __import__('os').path.join(__import__('paths').GEN, 'sensors_catalog.h')
SENSORS = 'Configuration/Sensors'

def natural_key(name):
    '''Sort key for a display name: A-Z, but digit runs compare as NUMBERS.

    Plain alphabetical is wrong for this catalogue and obviously so — it files the twelve exhaust gas
    temperatures as 1, 10, 11, 12, 2, 3 and the fifteen widebands the same way, which is not a
    reader's idea of alphabetical order and is worse than the catalogue order it replaced.
    '''
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', name)]


def group_sensors(rows):
    '''{group key: [(id, name)]} with each group in NATURAL NAME ORDER.

    The order was the catalogue's, which is the order the firmware happens to declare its inputs in —
    meaningful to the C array and to nobody reading a page of a hundred and twenty-two names looking
    for one. Every place that groups sensors comes through here so the switchboard, the per-group
    pages and the tree cannot disagree about where a name is.
    '''
    out = {}
    for sid, name, group in rows:
        out.setdefault(group, []).append((sid, name))
    for g in out:
        out[g].sort(key=lambda t: natural_key(t[1]))
    return out


# Catalogue group -> the panel it appears under, in the order an installer works down: what the engine
# needs to run, then what it is breathing, then the car around it, then everything else.
GROUP_TITLE = [
    ('ENGINE',        'Engine'),
    ('ENGINE_SYNC',   'Engine Sync'),
    ('O2',            'O2 & Lambda'),
    ('BOOST',         'Boost & Intake'),
    ('ATMOSPHERIC',   'Atmospheric'),
    ('VEHICLE_SPEED', 'Vehicle Speed'),
    ('TRANSMISSION',  'Transmission'),
    ('CHASSIS',       'Chassis'),
    ('AIRCON',        'Air Conditioning'),
    ('NITROUS',       'Nitrous'),
    ('OTHER',         'Other'),
]


def catalog():
    """(id, name, group) for every sensor the firmware has, in catalogue order."""
    return [(i, n, g) for i, n, _t, g in catalog_full()]


def catalog_full():
    """…and the TYPE token with it. SENSOR_TYPE_NONE means the catalogue fixes no type, so the tune picks
    one — that is the aux inputs, the rotary trims and a couple of others. Every other row's type is a
    fact about the sensor, not a setting."""
    h = open(CATALOG, encoding='utf-8').read()
    return re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)",\s*SENSOR_TYPE_(\w+),\s*SENSOR_GROUP_(\w+)', h)


# The catalogue row's trailing DTC codes, in declaration order (SensorDescriptor): one per check, then
# the precondition and config codes. A ZERO means this sensor has no code for that check — and a check
# that cannot report what it finds is a check not worth offering, which is what keeps Detect Stuck off a
# throttle position and leaves it on a switch.
# THE ORDER COMES FROM THE HEADER, not from a copy of it kept here. This was a hand-written list of
# eight names matched against "the last eight hex values in the row", and the moment a ninth dtc field
# was added to the struct (dtc_no_band) every column slid one across: a drive shaft pickup — a FREQUENCY
# input, whose type declares only operating_min/max and max_derivative — was offered Detect Raw High and
# Detect Stuck, electrical and switch checks on an input that has neither, while Detect Reading High,
# which it really does have, was missing. Silent, and on every sensor page at once.
def _dtc_order():
    """The dtc_* fields of SensorDescriptor, in declaration order."""
    h = open(CATALOG, encoding='utf-8').read()
    i = h.index('struct SensorDescriptor')
    body = h[i:h.index('};', i)]
    return re.findall(r'\bdtc_(\w+)', body)   # the group already excludes the dtc_ prefix


DTC_ORDER = _dtc_order()


def catalog_dtcs():
    """{sensor id: {check: 'P0122'}} — only the checks that have a code."""
    h = open(CATALOG, encoding='utf-8').read()
    out = {}
    n = len(DTC_ORDER)
    for m in re.finditer(r'\{\s*"([^"]+)",\s*"[^"]+",\s*SENSOR_TYPE_\w+,\s*SENSOR_GROUP_\w+,'
                         r'[^}]*?((?:0x[0-9A-Fa-f]{4},\s*){%d})\}' % n, h):
        sid = m.group(1)
        codes = re.findall(r'0x([0-9A-Fa-f]{4})', m.group(2))
        out[sid] = {name: f'P{code.upper()}' for name, code in zip(DTC_ORDER, codes) if code != '0000'}
    return out


def node_paths(doc):
    """Every node path in the tree, so a link is only written when it names one that exists."""
    out = set()
    def walk(n, pre):
        for c in n.get('children', []):
            p = f'{pre}/{c["name"]}'
            out.add(p)
            walk(c, p)
    for t in doc['tree']:
        out.add(t['name'])
        walk(t, t['name'])
    return out


def link_for(name, sid, paths):
    """Where this sensor's own page lives. A sensor with no node gets no link rather than a dead one —
    the checkbox still works, which is the half of the row that always does something.

    The node lives under its GROUP now (Sensors / Engine / Coolant Temperature), so this matches on the
    LEAF name anywhere under Sensors rather than on a fixed path. That also keeps it working through the
    reshuffles this tree has already had — hand-authored pages directly under Sensors, then an array
    sub-branch, now groups — because what identifies a sensor's node is its name, not where it sits.

    The numbered fallback is for the catalogue rows that expand: `app_1` is named "Accelerator Pedal" in
    the catalogue and "Accelerator Pedal 1" in the tree, because the tree was written for a car with two
    of them. Matching on the name alone left the first pedal as the one row on this page with no way in.
    """
    names = [name]
    m = re.search(r'_(\d+)$', sid)
    if m:
        names.append(f'{name} {m.group(1)}')
    # Suffix match on the '/' boundary rather than a split: one sensor is called "Flex Fuel
    # Composition/Temperature", and a tree name is allowed to contain a slash — splitting the path on the
    # last one left that sensor as the single row on the switchboard with no way in.
    for want in names:
        for path in sorted(paths):
            if path.startswith(SENSORS + '/') and path.endswith('/' + want):
                return path
    return ''


def page(doc):
    p = A.Page(title='Sensors')
    # The head line carries the count. A footer used to, at a fixed y — and the columns pack to the page
    # bottom, so it printed across the last panel's final rows. There is no room down there by design.
    p.head(f'All {len(catalog())} inputs this ECU can read. Tick what the car has; the name is the way to '
           f'its calibration, wiring and diagnostics — one in grey is switched off.')

    paths = node_paths(doc)
    rows = catalog()
    by_group = group_sensors(rows)

    # ONE FLOW: top down, left to right, a heading where each group starts.
    #
    # This page was panels-per-group, packed by a planner that had to decide when a group could be split
    # and how to caption the halves — and that was the wrong problem to be solving. A switchboard is a
    # list. It runs down a column, wraps to the next, and says what it is at each group boundary; a group
    # that crosses a column carries its heading over so the column never starts with anonymous rows.
    #
    # PLANNED FIRST, MEASURED SECOND. Six fixed 205px columns left 175px for a sensor's name and the
    # catalogue's names do not fit that: "Flex Fuel Composition & Temperature" lost a third of itself and
    # every "Exhaust Gas Temperature 10" came out as "Exhaust Gas Temperature", losing the one part of the
    # name that tells it from its eleven neighbours. Nothing said so — a caption is clipped to its own box
    # and the box fitted its column perfectly.
    #
    # Column membership does not depend on width (the card's height and the row pitch decide it), so the
    # flow is planned with no width at all and each column is then made as wide as the widest name that
    # landed in it. At a 20px pitch the whole catalogue fits in FIVE such columns with room to spare, which
    # is why the pitch came down by two: the rows it buys are what let the names have their width.
    # THE 20px PITCH IS LOAD-BEARING: it is what fits the whole catalogue into FIVE columns of a 1280px
    # page. Widening it to clear the 22px tick box needs six columns and 1390px (21 already needs 1372),
    # and the page stops fitting at all. The box comes down to the row instead — see the checkbox below.
    # THE HEADING KEEPS ITS 30. Cataloguing the wheel-speed and GPS pickups took this page to 134 inputs,
    # which needed a SIXTH column and 1380px of a 1280px page, and the first answer was to shave the
    # headings to 26 — squeezing the page to pay for space that was being wasted above it. The band at the
    # top of every page reserved 30px for a title bar that is 22 (author.BAND), and giving those back is
    # worth more than four rows: five columns, 1196px, with the headings untouched.
    ROW_H, HEAD_H, GAP, COLS = 20, 30, 8, 6
    NAME_X = ROW_H + A.CHECK_GAP     # a name starts past its box, which is ROW_H here (see the checkbox)
    BOT = 8
    plan = [[]]                      # plan[col] = [('H', title) | ('R', (sid, name))]
    y = A.TOP
    linked = unlinked = 0

    def fits(h):
        return y + h <= A.CANVAS_H - BOT

    for key, title in GROUP_TITLE:
        items = list(by_group.get(key, []))
        if not items:
            continue
        # A heading with no room for rows under it belongs in the next column, not at the foot of this one.
        if not fits(HEAD_H + 2 * ROW_H):
            if len(plan) >= COLS:
                break
            plan.append([]); y = A.TOP
        plan[-1].append(('H', title))
        y += HEAD_H
        while items:
            if not fits(ROW_H):
                if len(plan) >= COLS:
                    break
                plan.append([]); y = A.TOP
                plan[-1].append(('H', f'{title}  (continued)'))
                y += HEAD_H
            plan[-1].append(('R', items.pop(0)))
            y += ROW_H
        if items:
            raise SystemExit(f'sensor switchboard: {len(items)} {title} input(s) did not fit in '
                             f'{COLS} columns at pitch {ROW_H}')

    r = ruler()
    x = 10
    for column in plan:
        # As wide as the widest thing in it: a name with its checkbox in front, or a group heading.
        # +8 on the measured text because a caption is drawn one 3px inset in from each edge of its box.
        rw = max((r.width(n) for k, (_i, n) in ((k, v) for k, v in column if k == 'R')), default=0.0)
        hw = max((r.width(t, 16) for k, t in column if k == 'H'), default=0.0)
        col_w = int(max(rw + NAME_X + 8, hw + 8)) + 2
        y = A.TOP
        for kind, item in column:
            if kind == 'H':
                # The heading is the way into the GROUP's own page — the roomier list, with each input's
                # live reading beside it. A switchboard says what is fitted; the group page says what it
                # is reading.
                base = item.split('  (continued)')[0]
                gpath = f'{SENSORS}/{base}'
                hp = {'labelText': item, 'align': 'Left', 'fontName': '|16|1|0'}
                if gpath in paths:
                    hp['link'] = gpath
                p.add(p._new('label', x, y + 4, col_w - 8, 18, hp))
                # A rule under the heading, which is what makes a flow read as sections rather than one
                # long list. Clear of the label's box, not merely of its ink: the checker compares
                # rectangles, and so does the eye when the text has a descender.
                p.add(p._new('panel', x, y + HEAD_H - 3, col_w - 8, 1,
                             {'bgColor': A.C_DIM, 'padding': '0'}))
                y += HEAD_H
                continue
            sid, name = item
            link = link_for(name, sid, paths)
            linked += 1 if link else 0
            unlinked += 0 if link else 1
            g = p.group()
            # The box is the ROW, not the style's 22px: JCheckBox draws a box of its rect's height, so a
            # 22px box on a 20px pitch overlapped its neighbour's hit rect by two pixels — and the LOWER
            # element wins a hit test, so the bottom two pixels of every box here toggled the sensor
            # BELOW it. Two pixels of ink nobody can see against a wrong sensor switched on. The pitch
            # cannot grow (20 is what fits 128 inputs in six columns of a 1280px page; 21 already needs
            # 1372), so the box comes down to meet it.
            p.add(p._new('checkbox', x, y, ROW_H, ROW_H,
                         {'signalName': f'sensors.sensor[{sid}].enabled'}, g))
            lp = {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL,
                  'enableCondition': f'[#sensors.sensor[{sid}].enabled] == 1'}
            if link:
                lp['link'] = link
            p.add(p._new('label', x + NAME_X, y - 2, col_w - NAME_X, 18, lp, g))
            y += ROW_H
        x += col_w + GAP
    if x - GAP > A.CANVAS_W - 10:
        raise SystemExit(f'sensor switchboard: {len(plan)} columns need {x - GAP}px of a '
                         f'{A.CANVAS_W}px page')

    return p, linked, unlinked
