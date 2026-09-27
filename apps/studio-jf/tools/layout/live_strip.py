"""The live strip belongs to the CANVAS, not to any page.

It sits on the Main surface above the viewport, and the viewport is shortened to fit under it — so the
numbers you are steering by (RPM, load, throttle, temperatures, lambda, injector time) stay put while the
tree moves the page underneath. A strip authored onto each page instead would be the same widgets copied
a hundred and sixty times, jumping a pixel as you navigate, and dying with the page it sat on.

Every page's canvas is shortened to match, so a page still fills its viewport exactly rather than being
scaled down into it.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from ruler import ruler
from apply_fuel import DOC

CANVAS_W, CANVAS_H = 1600, 800    # the surface itself — the room everything below is carved out of
STRIP_H = 86                      # the fixed strip across the top
VIEW_Y = STRIP_H + 6
# THE WIDTH IS SHARED OUT, and it has to add up exactly: a left margin, the page, a matching gap, the
# rail. It used to be page + 10 + rail with nothing on the left, so a window opened hard against the dock
# edge on one side and 10px clear of the rail on the other — and there was no room for the page's own
# width, which is why one appeared with a horizontal scroll bar it did not need.
PAGE_PAD = 8                      # left of the page's WINDOW, and again between that window and the rail
# The studio's window chrome, from jf::JMdiChild: the page sits inside a frame with a title bar above it
# and a border down each side. The rail has to clear the FRAME, not the page, or the page loses those
# pixels and grows a scroll bar it does not need.
FRAME_TITLE, FRAME_BORDER = 26, 6
RAIL_W = 292                      # the right-hand rail: the watch list, and a trace under it
PAGE_W = CANVAS_W - RAIL_W - 2 * (PAGE_PAD + FRAME_BORDER)   # what a page gets to be…
PAGE_H = CANVAS_H - VIEW_Y - 8    # …once the canvas has taken its share
# THESE TWO NUMBERS ARE THE PAGE CANVAS, and author.CANVAS_W/H must equal them. They did not: pages were
# authored to 1280x720 and then declared 1000x614 here, so every page was a fifth wider and taller than
# the box it claimed — the right-hand column fell outside the viewport and the top row went under the
# viewport's own title bar, which is drawn over the content by design.
assert (PAGE_W, PAGE_H) == (A.CANVAS_W, A.CANVAS_H), \
    f'page canvas {A.CANVAS_W}x{A.CANVAS_H} != the viewport {PAGE_W}x{PAGE_H}'
RAIL_X = CANVAS_W - RAIL_W
WATCH_H = 420                     # the list gets the top of the rail…
TRACE_Y = VIEW_Y + WATCH_H + 10   # …and a trace view the rest of it

# What you steer by. Order runs left to right the way the engine is read: speed, load, throttle, then the
# temperatures, then the mixture, then what the injectors and coils are actually being told to do.
CHANNELS = [
    ('Engine RPM',  'rpm',           '%.0f', 'RPM'),
    ('Fuel Load',   'fuel_load',     '%.0f', 'kPa'),
    ('MAP',         'map',           '%.0f', 'kPa'),
    ('Throttle',    'tps',           '%.1f', '%'),
    ('Coolant',     'clt',           '%.0f', '°C'),
    ('Air Temp',    'iat',           '%.0f', '°C'),
    ('Lambda',      'lambda_1',      '%.2f', 'λ'),
    ('Target',      'lambda_target', '%.2f', 'λ'),
    ('Advance',     'advance',       '%.1f', '°'),
    ('Dwell',       'dwell',         '%.0f', 'us'),
    # THREE DECIMALS, and stated here because neither default gets there: the channel declares 3
    # digits but UnitManager registers "ms" at %.1f (UnitManager.cpp:92), and the unit wins. One
    # decimal is right for a dwell in ms and useless for an injector pulse — 0.8 where the tune is
    # working in thousandths.
    ('Inj PW',      'inj_pw',        '%.3f', 'ms'),
    ('Battery',     'battery',       '%.2f', 'V'),
]

# WHAT THE RAIL WATCHES, and therefore what the strip does NOT.
#
# Oil pressure, fuel pressure and knock level were in both — the same channel, drawn as a number, twice
# on one screen. Three cells of fifteen spent saying what was already said two hundred pixels to the
# right, and the strip was paying for them in width: at fifteen cells a value box is 62px and holds
# about 50px of text, so map, inj_pw and both pressures CLIPPED at their own declared maximum
# ("1000.0" and "60.000" are 60.2px at the strip's font). Twelve cells make the box 88px and every
# reading fits with room to spare.
#
# The two lists must stay disjoint, so they are checked against each other at install time rather than
# by whoever next reads the screen (see main()).
RAIL_CHANNELS = ['oil_pressure', 'fuel_pressure', 'stft_pct', 'ltft_pct',
                 'knock_level', 'egt_1', 'vehicle_spd']


def strip_widgets(next_id):
    """The strip as canvas widgets: a boxed number per channel, name above, unit beside.

    The number box shows the NUMBER ONLY. A value widget prints its unit with the reading unless told
    otherwise, and the strip prints the unit itself in grey beside the box — so every cell said its unit
    twice and paid for it in width: "0.00 lambda" ran out of box at idle, and 9000 RPM would have gone the
    same way the moment the engine was revved. One of the two had to go, and the grey one is the one that
    lines up down the strip.
    """
    out, i = [], next_id
    cell = (CANVAS_W - 20) // len(CHANNELS)
    # THE UNIT SLOT IS AS WIDE AS THE WIDEST UNIT IN THE STRIP, measured, not 24px flat: "RPM" is 28px
    # of text and came out as "RP" on the first cell of the tab everybody looks at. One width for all
    # fifteen, because a strip whose grey suffixes do not line up reads as fifteen different things.
    # (+8: a caption is drawn one 3px inset in from each edge of its box.)
    unit_w = int(max(ruler().width(u) for *_r, u in CHANNELS)) + 8
    for n, (label, ch, fmt, unit) in enumerate(CHANNELS):
        x = 10 + n * cell
        g = 900 + n
        def w(type_, dx, dy, dw, dh, props):
            nonlocal i
            i += 1
            return {'id': i - 1, 'uid': str(uuid.uuid4()), 'type': type_, 'x': x + dx, 'y': dy,
                    'w': dw, 'h': dh, 'groupId': g, 'props': props}
        out.append(w('label', 0, 6, cell - 8, 18,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': A.C_DIM}))
        out.append(w('value', 0, 26, cell - unit_w - 6, 34,
                     {'signalName': ch, 'format': fmt, 'fontName': '|22|0|0', 'align': 'Left',
                      'showUnit': '0', 'borderWidth': '1', 'borderRadius': '3', 'padding': '3'}))
        out.append(w('label', cell - unit_w - 4, 34, unit_w, 18,
                     {'labelText': unit, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': A.C_DIM}))
    return out, i


# --- THE STATUS LAMPS, AS A PAGE THE DOCK HOSTS ------------------------------------------------
#
# An imported TunerStudio definition files its [FrontPage] lamps under "%Status" and the studio hosts
# that page in the Status Lamps dock — sized, hidden and closed like any other. A native ECU has lamps
# too; they were authored onto a pooled surface that is not even in the open list, so nothing ever
# showed them. Same page, same key, same dock.
#
# NO OFF COLOURS. "Not lit" is not information, and stating it as a literal (#2a2a2e, as the pooled
# surface did) bakes one scheme into the document: correct on the dark theme and a hole punched in the
# light one. Left unset the widget takes the scheme's own surface. The LIT colour is the definition's
# business and stays.
STATUS_PAGE = '%Status'
LAMP_H      = 30                  # one control row, so a lamp matches every other control's height
LAMP_PAD    = 3                   # the gutter, so two lit neighbours read as two lamps and not one bar
PANEL_INSET = 4                   # what a panel keeps for itself before handing the rest to a child
C_LIVE, C_WARN, C_INFO = '#30d158', '#ff453a', '#0a84ff'

STATUS_LAMPS = (
    ('[$synchronized]',          'NO SYNC',     'SYNC',        C_LIVE),
    ('[$engine_state] == 2',     'STOPPED',     'RUNNING',     C_LIVE),
    ('[$fuel_cut] || [$ign_cut]', '\u2014',      'CUT',         C_WARN),
    ('[$dtc_active] > 0',        'OK',          'DTC',         C_WARN),
    ('[$idle_active]',           'IDLE',        'IDLE',        C_INFO),
    ('[$dfco_active]',           'OVERRUN',     'OVERRUN',     C_INFO),
    ('[$transient_active]',      'TRANSIENT',   'TRANSIENT',   C_INFO),
    ('[$prot_status] > 0',       'PROTECTION',  'PROTECTION',  C_WARN),
    ('[$misfire_count] > 0',     'MISFIRE',     'MISFIRE',     C_WARN),
    ('[$trigger_error_pct] > 0', 'TRIGGER ERR', 'TRIGGER ERR', C_WARN),
    ('[$etb_state_1] == 3',      'ETB FAULT',   'ETB FAULT',   C_WARN),
)


def status_page(next_id):
    """The lamp grid as a reflowing page: a Wrap of equal cells, one lamp inset in each.

    The cell is as wide as the widest caption a lamp can show, and the row is then divided EXACTLY
    between as many as fit — so the grid reaches both edges rather than stopping in mid-air, and adding
    a lamp re-fits it instead of clipping one. Wrap means the dock re-wraps it to whatever width it is
    given, which is the whole point of it being a page rather than a strip at a fixed size.
    """
    r = ruler()
    widest = max(max(r.width(off), r.width(on)) for _s, off, on, _c in STATUS_LAMPS)
    cols   = max(1, int(PAGE_W / max(90.0, widest + 24.0)))
    cellw  = PAGE_W / cols
    rows   = (len(STATUS_LAMPS) + cols - 1) // cols

    nid, cells = next_id, []
    for sig, off, on, colour in STATUS_LAMPS:
        lamp = {'id': nid, 'uid': str(uuid.uuid4()), 'type': 'indicator',
                # THE CELL'S CONTENT BOX, not its outer rect: a panel hands its children w-4 and h-4, so a
                # lamp inset by the gutter on both sides is one pixel past the box it is actually given.
                'x': LAMP_PAD, 'y': LAMP_PAD,
                'w': cellw - LAMP_PAD - PANEL_INSET, 'h': LAMP_H - LAMP_PAD - PANEL_INSET,
                'props': {'signalName': sig, 'offTitle': off, 'onTitle': on,
                          'onBg': colour, 'onFg': '#000000'}}
        nid += 1
        cells.append({'id': nid, 'uid': str(uuid.uuid4()), 'type': 'panel',
                      'x': 0, 'y': 0, 'w': cellw, 'h': LAMP_H,
                      'props': {'layoutMode': '0', 'showBorder': '0',
                                'children': json.dumps([lamp])}})
        nid += 1

    # The strip is its rows PLUS what it keeps for itself, for the same reason the lamp is the cell's
    # content box: a panel hands its children h-4, so a strip exactly as tall as its rows is a strip
    # whose last row is one inset short of fitting.
    strip_h = rows * LAMP_H + PANEL_INSET
    strip = {'id': nid, 'uid': str(uuid.uuid4()), 'type': 'panel',
             'x': 0, 'y': 0, 'w': PAGE_W, 'h': strip_h,
             'props': {'layoutMode': '7', 'showBorder': '0', 'children': json.dumps(cells)}}
    nid += 1
    page = {'canvasWidth': PAGE_W, 'canvasHeight': strip_h,
            'canvasStatic': 3,          # reflow: the page is the view, so it re-wraps to the dock
            'layout': 8,                # Column: the strip takes the width and its own height
            'guideW': 0, 'guideH': 0, 'widgets': [strip]}
    return page, nid, cols, rows


def rail_widgets(next_id):
    """The right-hand rail: the operator's own watch list, and a trace under it.

    Both are on the CANVAS rather than on any page, so what someone chose to watch while chasing a problem
    survives every page they navigate through while chasing it. The trace is there because a number tells
    you where a thing is and a trace tells you where it is going, and the question while tuning is nearly
    always the second one.
    """
    watch = {'id': next_id, 'uid': str(uuid.uuid4()), 'type': 'channels',
             'x': RAIL_X, 'y': VIEW_Y, 'w': RAIL_W - 10, 'h': WATCH_H, 'groupId': 901,
             'props': {'labelText': 'Channels', 'channels': ';'.join(RAIL_CHANNELS),
                       'rowHeight': '24', 'showUnits': '1', 'editable': '1', 'format': '%.2f',
                       'borderWidth': '1', 'borderRadius': '3'}}
    trace = {'id': next_id + 1, 'uid': str(uuid.uuid4()), 'type': 'livegraph',
             'x': RAIL_X, 'y': TRACE_Y, 'w': RAIL_W - 10, 'h': (VIEW_Y + PAGE_H) - TRACE_Y,
             'groupId': 902,
             'props': {'labelText': 'Trace', 'showLegend': '1', 'displayUnit': 'Auto',
                       'borderWidth': '1', 'borderRadius': '3',
                       'lines': 'rpm,0,8000,0,0,0\nmap,0,300,0,0,0\ntps,0,100,0,0,0'}}
    return [watch, trace]


def stamp_built_for(d):
    """Record WHAT THIS LAYOUT WAS DRAWN AGAINST — the schema the bindings were generated from.

    The studio writes this when it saves; these installers write the document directly and were leaving
    whatever the studio last stamped, so the file claimed a schema three layout hashes old while every
    binding in it had been generated against the current one. The load side uses it to say which bindings
    no longer resolve, and a stale value makes that report a work of fiction.
    """
    meta = json.JSONDecoder().raw_decode(
        open(__import__('paths').META,
             encoding='utf-8', errors='replace').read(), 0)[0]
    mb = meta.get('meta') or {}
    was = (d.get('builtFor') or {}).get('layout_hash')
    d['builtFor'] = {'layout_hash': mb.get('layout_hash', ''),
                     'telemetry_size': mb.get('telemetry_size', 0)}
    return was, d['builtFor']['layout_hash']


def main():
    # SAID ONCE. A channel in both lists is drawn twice on the same screen, and the strip is the one
    # that pays: every cell it spends on a repeat narrows all fifteen. Caught here because the cost is
    # invisible on the page — the strip just quietly clips a digit off somewhere else.
    dupes = [ch for _l, ch, _f, _u in CHANNELS if ch in RAIL_CHANNELS]
    if dupes:
        raise SystemExit(f'live_strip: {dupes} are in BOTH the strip and the rail — pick one')

    d = json.load(open(DOC))
    main_model = d['surfaces']['pool'][0]['model']

    # Replace any strip a previous run left, so this is repeatable.
    #
    # BY THE BAND AS WELL AS BY THE GROUP. Keying only on groupId >= 900 misses anything authored
    # before the strip was grouped: those carry groupId 0, which is indistinguishable from every other
    # ungrouped widget, so they survived every "repeatable" run while a fresh grouped copy was laid on
    # top of them. That is exactly what happened to Inj PW — two boxes of different widths, two grey
    # "ms" labels 15px apart, and two numbers at different precisions printed over each other.
    #
    # The strip OWNS y < STRIP_H: the viewports are moved to VIEW_Y below it and the rail starts lower
    # still, so nothing else belongs in that band and anything found there is a leftover.
    before = len(main_model['widgets'])
    main_model['widgets'] = [w for w in main_model['widgets']
                             if w.get('groupId', 0) < 900 and w.get('y', 10 ** 6) >= STRIP_H]
    removed = before - len(main_model['widgets'])

    nid = max([w['id'] for w in main_model['widgets']] + [0]) + 1
    strip, nid = strip_widgets(nid)
    strip += rail_widgets(nid); nid += 2

    moved = 0
    # A viewport's caption is painted OVER the page it hosts (a title must never cost the page a scroll
    # bar), so a captioned viewport ate the page's first row: the heading came back clipped, under a bar
    # saying the same word. Cleared on every surface, not just Main — the workspaces host the same pages.
    capped = 0
    for surf in d['surfaces']['pool']:
        for w in surf.get('model', {}).get('widgets', []):
            if w['type'] == 'viewport' and w['props'].get('labelText'):
                w['props']['labelText'] = ''
                capped += 1
    # THE MAIN SURFACE NO LONGER HOLDS THE PAGE. A page opens in a window of its own now, and the viewport
    # here used to be the page view — so the selected page was drawn twice, once on this surface and again
    # in the window over it: the same controls in two places, either of which could be clicked. What is
    # left is what belongs behind a window and nowhere else: the readout strip along the top and the
    # channel rail down the side, with the middle clear for the window to land in.
    kept = [w for w in main_model['widgets'] if w['type'] != 'viewport']
    moved = len(main_model['widgets']) - len(kept)
    main_model['widgets'] = kept
    # The strip goes UNDER the viewports in paint order so nothing overlaps it; it is above them on screen.
    main_model['widgets'] = strip + main_model['widgets']
    main_model['canvasWidth'], main_model['canvasHeight'] = CANVAS_W, CANVAS_H
    # WHERE A PAGE WINDOW GOES ON THIS TAB. PAGE_PAD/PAGE_W/PAGE_H above already ARE this rectangle —
    # "what a page gets to be once the canvas has taken its share" — so it is written down for the studio
    # instead of left to be guessed from where the strip, the rail and the trace ended up.
    main_model['pageArea'] = [PAGE_PAD + FRAME_BORDER, VIEW_Y, PAGE_W, PAGE_H]
    # A SURFACE FILLS THE SPACE IT IS GIVEN. These carry the live instruments behind the page windows, and
    # they were fixed-size cards (canvasStatic 0 = "ask the preference") anchored by another preference and
    # ringed with a 1px border — a card floating in the middle of the centre with dead space around it.
    # Reflow, no anchor, no outline: there is no leftover room to align them in and no edge to draw.
    for surf in d['surfaces']['pool']:
        m = surf.get('model')
        if not isinstance(m, dict):
            continue
        m['canvasStatic'] = 3      # reflow: the surface is the view
        m['canvasAnchor'] = 0      # nothing to anchor when it fills
        m['borderWidth']  = 0      # and no card edge to draw round it

    # NOTHING LIVES IN THE TOP BAND. A viewport paints its title over the page's first rows — always: with
    # no caption of its own it falls back to the page's title — so anything up there is simply hidden. The
    # pages this tooling writes leave the band clear by construction; the hand-authored ones predate the
    # rule, so shift each of them down by exactly what it is short, which keeps their layout intact.
    nudged = 0
    for k, v in d['panelLibrary'].items():
        if not isinstance(v, dict) or not v.get('widgets'):
            continue
        top = min(w['y'] for w in v['widgets'])
        if top >= A.BAND:
            continue
        delta = A.BAND - top
        bottom = max(w['y'] + w['h'] for w in v['widgets'])
        if bottom + delta > v.get('canvasHeight', A.CANVAS_H):
            print(f'  ! {k}: needs {delta}px but has none to spare — left alone')
            continue
        for w in v['widgets']:
            w['y'] += delta
        nudged += 1

    # The lamp page, which the Status Lamps dock hosts. Rebuilt every run so a changed lamp list lands.
    status, nid, scols, srows = status_page(nid)
    d['panelLibrary'][STATUS_PAGE] = status

    pages = 0
    for k, v in d['panelLibrary'].items():
        # NOT THE LAMP PAGE. It is sized to its own grid, and the dock opens to that height — forced to
        # the page canvas it would claim 700px of a dock that needs a hundred.
        if k == STATUS_PAGE:
            continue
        # A PAGE IS THE SIZE OF WHAT IS ON IT. This used to stamp every page to the full sheet, which is
        # what a page was given back when one was mirrored into a fixed viewport — and it is a lie now that
        # a page opens in a window sized to what it declares: a 4x8 idle table claimed 1280x700 and its
        # window swamped the tab's own instruments. author.Page.to_json measures the widgets.
        #
        # AND THE WIDTH IS NO LONGER CLAMPED. Holding every page to the sheet was the last thing cutting
        # Target Lambda off: 21 columns at two decimals is 1378px, and a page pinned to 1280 showed
        # twenty of them with a scroll bar over a table that had nowhere to grow. A window opens at what
        # its page declares and the MDI clamps THAT to the screen, so a page wider than the monitor
        # opens as wide as it can and scrolls — which is a map you can reach all of, instead of one
        # whose last column does not exist. The height stays clamped: the sheet's height is the tab's,
        # and a page taller than it pushes its own readouts off the bottom rather than scrolling to them.
        if isinstance(v, dict) and v.get('canvasHeight', 0) > PAGE_H:
            v['canvasHeight'] = PAGE_H
            pages += 1

    was, now = stamp_built_for(d)
    if was != now:
        print(f'built-for stamp: {was} -> {now}')

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'strip: {len(strip)} widgets ({removed} replaced); {moved} viewport(s) dropped '
          f'h={PAGE_H}; {pages} page canvas(es) resized; {capped} caption(s) cleared; '
          f'{nudged} page(s) nudged clear of the title bar')
    print(f'status lamps: {len(STATUS_LAMPS)} in {scols}x{srows} -> {STATUS_PAGE} '
          f'({status["canvasWidth"]:.0f}x{status["canvasHeight"]:.0f})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
