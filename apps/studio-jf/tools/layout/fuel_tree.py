"""The Fuel branch, at the granularity of the reference software's tree: one node per TABLE.

The sample tree groups by what the table is for (Corrections, Stage 1, Transient Throttle) and gives each
table its own leaf — Dead Time, Flow Rate, Short Pulse Width Adder, Firing Angle BTDC under a stage; every
correction, including the per-cylinder ones, under Corrections. Cylinders appear only for cylinders the
engine has.

That is a better fit than the workflow pages I built first: a page holding four tables is quicker to skim
ONCE, but a tree of tables is quicker to use every day after, because the thing you want is a name you can
see rather than a page you have to remember contains it.
"""
import author as A
from ruler import ruler
import fuel_pages as F
from author import C_DIM

FC = F.FC
ROOT = 'Configuration'
ENG = F.ENG
TT = F.TT


def table_page(title, table, live, note='', extra_fields=None, enable='', enable_path='', learned=False):
    """One table, full width, with anything that belongs to THIS table's intent underneath it.

    NO SIDE PANEL. The canvas already carries the live view — the strip of channels across the top, the
    channel list, the trace — and those are PERMANENT: they sit outside the viewport and stay put while
    pages come and go. A page repeating them in a panel of its own was duplicating what is already on
    screen, and paying for it with a third of the grid's width.

    What a page does carry is the handful of numbers that belong to its own subject: a VE table wants VE,
    lambda and the target under it, because those are what you read while editing those cells. They go
    along the BOTTOM, which is where readout() always said they went.

    `enable_path` is the table's OWN enable flag: a tick in the head row, and the same flag greys the
    table under it. Off is a thing the firmware does — the table is not evaluated at all — rather than
    something a page infers from a table full of zeroes."""
    # THE PAGE IS AS WIDE AS ITS TABLE. A table page is one grid and the numbers under it, so the sheet
    # has no business being narrower than the grid: Target Lambda is 21 columns at two decimals — 1378px
    # — and on a 1280 sheet the last column and a half were simply cut off, with a scroll bar over a
    # table that could have been whole. The window opens at what the page declares and the MDI clamps it
    # to the screen, so a page wider than the monitor scrolls rather than truncating.
    tw, _th = A.table_box(table, fallback=(1260, 0))
    p = A.Page(w=max(A.CANVAS_W, tw + 20), title=title)
    if enable_path and not enable:
        enable = f'[#{enable_path}] == 1'
    top = p.head(note, enable=enable_path) if (note or enable_path) else A.BAND

    # Settings that belong WITH the table (the prime mode sits with the prime table; nothing else knows
    # what it means) go in one row above it rather than in a column beside it.
    if extra_fields:
        # AN EXTRA FIELD OF THIS TABLE'S OWN MODULE SHARES THE TABLE'S FATE. A correction slot switched
        # off is not evaluated at all, so its channel and its Applies To are as unread as its curve;
        # leaving them live is a control taking a value nothing consults. The firmware-side gate audit
        # cannot see that one, because a table's channel selector is named only inside the generated
        # descriptor and never appears in the module's own source.
        #
        # A FIELD FROM ANOTHER MODULE DOES NOT. engine.cranking_rpm sits on the fuel cranking page
        # because that is where you want it, but the state machine reads it whatever fuel is doing
        # (SystemComposer.cpp:94) — greying it because cranking ENRICHMENT is off would be the same
        # fault in the opposite direction, and a worse one: a live setting the page says is dead.
        owner = table.split('.', 1)[0]
        for i, (label, path, kind, unit) in enumerate(extra_fields):
            mine = path.split('.', 1)[0] == owner
            p.field(None, 10 + i * 330, top, label, path, kind, 150 if kind != 'enum' else 200, unit,
                    enable=enable if mine else '')
        top += A.ROW + 8

    # A LEARNED TRIM CARRIES ITS OWN WORKFLOW. Folding it into the map it corrects, and throwing it
    # away, are what the surface is FOR — they belong on its page, beside it, not behind a right-click
    # in a menu of table operations that has nothing to do with either.
    #
    # APPLY ONLY WHERE THERE IS SOMETHING TO APPLY TO. Not every learned surface corrects a map: the
    # bank trim is a pair of scalars and declares no `apply_to`, so an Apply button there would be a
    # control that can never do anything — which is the same lie the table menu was telling before.
    # `learned` is 'apply+reset' where the schema names a base map and 'reset' where it does not.
    acts = [] if not learned else (['apply', 'reset'] if 'apply' in str(learned) else ['reset'])
    if acts:
        for i, a in enumerate(acts):
            p.learned_action(10 + i * 200, top, 190, table, a)
        top += 34

    # THE BOX IS THE GRID, not the page. A table used to be given every pixel the canvas had left, so a
    # 4x8 idle target sat in a box built for a 21x22 ignition map: the grid drew its own size in the top
    # corner and the live readouts under it were a screen away, and the WINDOW opened at the width of the
    # page rather than the width of the table. Sized from the meta's full allocation (author.table_box),
    # so the box is what the table can grow to and everything below it follows the grid.
    tbl_w, tbl_h = A.table_box(table, fallback=(p.w - 20, A.CANVAS_H - top - (66 if live else 8)))
    avail_h = A.CANVAS_H - top - (66 if live else 8)
    tbl_h = min(tbl_h, avail_h)
    # ROOM TO GROW, UP TO THE ROOM THERE IS. table_box answers with what the table can grow to, which is
    # the point — an 8x8 gear correction that holds 16x16 should be boxed for sixteen and sit in the
    # space until somebody uses it, not be crippled at eight for ever on an otherwise empty page.
    #
    # …but a page cannot be bigger than a page. Target Lambda can grow to twenty-nine columns at two
    # decimals, which is 2065 px: boxing for that makes the PAGE 2085 wide, so it scrolls sideways on
    # every screen from the day it ships, for columns nobody has added yet. Capped at the authored
    # canvas, so a table grows into the page and then the grid scrolls — which is what it did before for
    # the maps that were always too wide, and is the only honest answer when the map is wider than the
    # screen.
    # …and never below what it needs TODAY. Capping at the canvas took Target Lambda from the 1378 its
    # twenty-one columns actually need to 1260, so a map that fitted started scrolling for columns
    # nobody had added yet. The floor is the shipped size, the ceiling is the page, and between them the
    # table gets the room to grow that it is allocated for.
    ship_w, _ = A.table_box_shipped(table, fallback=(0, 0))
    tbl_w = max(ship_w, min(tbl_w, A.CANVAS_W - 20))
    # THE BOX TAKES THE ROOM THE WINDOW HAS; THE CELLS ARE THE READER'S BUSINESS. A table page is one map
    # and the numbers under it, so any width or height past what the page was authored for belongs to the
    # map — that is what the anchors say, and they still say it. Below the authored size there is no
    # slack, the canvas stops, and the surface scrolls: the authored layout IS the compact layout.
    #
    # WHAT IT NO LONGER SAYS is how big a cell should be. Stretch was written here onto both axes, which
    # made a four-row correction table on a full-height page into four bands a quarter of the canvas
    # each — but the real fault was writing the property at all. Cell sizing is answerable in
    # Preferences ▸ Editor ▸ Globals, it was answered, and an explicit property on the element beats the
    # answer: the page overrode the reader on all 130 tables the installers build. Left unwritten, a
    # table follows the globals, which is where somebody who wants stretched columns can say so once.
    p.table(10, top, tbl_w, tbl_h, table, enable=enable,
            anchor_x='both', anchor_y='both')

    if live:
        # SIDE BY SIDE, AT THE SIZE A NUMBER IS. These were spread across the full page width — three
        # readouts on a 1280 page meant each one was 412 px wide for four digits and a caption under it,
        # with the reader's eye travelling the whole width to compare a target against what the engine is
        # doing. They are a row of gauges, not a justified line of text. Sizing them honestly also lets
        # the page tell the truth about how wide it needs to be: the width now belongs to the TABLE.
        # THE FIRST NUMBER UNDER A TABLE IS THAT TABLE'S OWN OUTPUT — what it is putting out right now,
        # at the cell the live trace is sitting on.
        #
        # It was missing from every page in the document but one. What the rows carried instead was the
        # AGGREGATE the table feeds: seven of the nine transient tables led with "Accel Corr", which is
        # every transient contribution multiplied together, so Enrich Amount and Disenrich Rate and
        # Coolant Temp Corr all showed the same number and none of them showed its own. Ignition
        # Correction was the exception and read correctly, which is what made the rest visible.
        #
        # Bound to the TABLE'S OWN PATH, not to a channel: Cache::configValue sends a table path to
        # solveTable, which interpolates it against its live axis channels in float, bit-for-bit with
        # the firmware. So it works for every table whether or not the ECU publishes a channel for that
        # particular lookup — and most of them do not. A page that also names a real applied channel
        # (Transient Ign) keeps it: what the table SAYS and what the ECU APPLIED after its clamps are
        # two different questions, and a page that answers both is the better page.
        live = [(title, table, A.table_fmt(table))] + list(live)
        y = top + tbl_h + 8
        # EACH ONE ITS OWN WIDTH, laid out left to right. A fixed pitch put every readout the same distance
        # apart and then let a wide caption ("Long Term Fuel Trim") grow past it into its neighbour — three
        # pixels of overlap, which is two live numbers sharing a column of pixels. Width first, then place.
        # …AND A STATE CHANNEL IS WORDS. The cell was sized for '-8888.8' and nothing else, so a readout
        # bound to an enum channel — which prints its label, because an index means nothing to a reader —
        # lost its last letters ('Closed Loop' needs 99 px where the number needs 84). Each cell is now
        # sized for the widest reading ITS OWN channel can produce.
        ws = [max(A.Page.chan_value_px(ch, 20) + 24, int(ruler().width(lbl, 15)) + 12)
              for lbl, ch, _f in live]
        x = 10
        for (lbl, ch, fmt), cw in zip(live, ws):
            # …and they ride the bottom, because the grid above them grew into the height. Anchored
            # rather than re-laid-out: each keeps the width its own number needs.
            p.readout(None, x, y, lbl, ch, fmt, w=cw, anchor_y='bottom')
            x += cw + 12
    return p


LIVE_RPM_LOAD = [('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')]

# (node name, table, live rows, note, extra fields, the correction's own enable flag)
#
# SPECIFIC GRAVITY IS NOT HERE. It sat in this list, and it is not a correction: nothing multiplies by
# it. It is a fuel PROPERTY — the mass/volume conversion between the fuel model and the injector's
# volumetric flow rating — so it stands on its own under Fuel Tuning, beside the setup it belongs to.
CORRECTIONS = [
    ('Coolant Temp',     FC + 'clt_corr_table',
     [('Warmup Corr', 'fuel_corr_warmup', '%.3f'), ('Coolant', 'clt', '%.0f'), ('Lambda', 'lambda_1', '%.2f')],
     'Warmup enrichment. Holds while the engine is cold, with no time term of its own.', [], FC + 'enable_warmup'),
    ('Post Start',       FC + 'post_start_table',
     [('Post-Start Corr', 'fuel_corr_poststart', '%.3f'), ('Run Time', 'run_time', '%.1f'),
      ('Coolant', 'clt', '%.0f')],
     'Decays out over run time. Tune AFTER warmup, or its fuel ends up baked into that table.', [], FC + 'enable_poststart'),
    ('Air Temp',         FC + 'iat_corr_table',
     [('Air Temp Corr', 'fuel_corr_iat', '%.3f'), ('Air Temp', 'iat', '%.0f'), ('MAP', 'map', '%.0f')], '', [], FC + 'enable_iat'),
    ('Barometric',       FC + 'baro_corr_table',
     [('Baro Corr', 'fuel_corr_baro', '%.3f'), ('Barometric', 'baro_kpa', '%.0f'), ('MAP', 'map', '%.0f')], '', [], FC + 'enable_baro'),
    ('Gear',             FC + 'fuel_gear_table',
     [('Gear Corr', 'fuel_corr_gear', '%.3f')] + LIVE_RPM_LOAD, '', [], FC + 'enable_gear'),
    ('RPM Limiter',      FC + 'rev_limit_fuel_corr_table',
     [('Rev Limit Corr', 'fuel_corr_revlimit', '%.3f'), ('RPM Before Cut', 'rpm_to_limit', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f')], '', [], FC + 'enable_revlimit'),
]
for _i in (1, 2, 3, 4):
    CORRECTIONS.append((f'Generic {_i}', f'{FC}generic{_i}_corr_table',
                        [(f'Generic {_i} Corr', f'fuel_corr_generic{_i}', '%.3f')] + LIVE_RPM_LOAD,
                        'A spare correction: pick its axes and it does whatever you need.', [],
                        f'{FC}enable_generic{_i}'))

def page_corrections_branch():
    """The switchboard for the corrections: what is in the fuel calculation, and what each is doing.

    A correction is a table that multiplies the fuel mass, and there are fourteen of them — so the
    question this page answers is the one the folder could not: which are actually in play, and what is
    each contributing right now. The tick is the firmware's own enable (off = the table is not evaluated
    at all), and the number beside it is that correction's live multiplier, where 1.000 is "no effect".
    """
    p = A.Page(title='Corrections')
    y0 = p.head('Every multiplier that sits on top of the VE table. A correction that is off is not '
                'evaluated at all — which is not the same as a table tuned to no effect, and this is '
                'where you can see which is which.')

    ROWS = [(name, enable, live[0][1], live[0][0]) for name, _t, live, _n, _e, enable in CORRECTIONS]
    # …plus the three that are corrections but have no table page of their own under this folder.
    EXTRA = [('Cranking', FC + 'enable_cranking', 'fuel_corr_cranking', f'{ROOT}/Fuel Tuning/Cranking'),
             ('Overall',  FC + 'enable_overall',  'fuel_corr_overall',  f'{ROOT}/Engine Configuration/Fuel System/Fuel Setup')]

    grid = p.panel(10, y0, 620, A.panel_h(len(ROWS) + len(EXTRA), 30, top=34, bottom=8), 'In the Calculation')
    for cx, cw, text in ((10, 40, 'On'), (56, 300, 'Correction'), (380, 120, 'Now'), (510, 100, 'Signal')):
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
    p.add(p._new('panel', 10, 28, 590, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    y = 34
    for name, enable, chan, _lbl in ROWS:
        _corr_row(p, grid, y, name, enable, chan, f'{ROOT}/Fuel Tuning/Corrections/{name}')
        y += 30
    for name, enable, chan, link in EXTRA:
        _corr_row(p, grid, y, name, enable, chan, link)
        y += 30

    note = ('Each tick is the FIRMWARE\'s enable, not a display filter: off, the table is never '
            'evaluated, its axis channels are never read, and the correction publishes exactly 1.000.\n\n'
            'That is the difference between a correction tuned to do nothing and one that is not in the '
            'calculation — a page showing 1.000 used to mean either.\n\n'
            'The multipliers all multiply: the total is their product, capped by an authority guard, and '
            'the fuel that comes out is the base calculation times that.')
    nh = A.Page.wrapped_h(note, 324)
    side = p.panel(640, y0, 350, A.panel_h(1, nh + 2 * A.ROW + 12, top=10, bottom=8), 'How They Combine')
    p.wrapped(10, 10, 324,
              note,
              into=side)
    y = nh + 18
    for label, node in (('Fuel Setup', f'{ROOT}/Engine Configuration/Fuel System/Fuel Setup'),
                        ('VE Table', f'{ROOT}/Fuel Tuning/VE Table')):
        y = p.switch(side, 10, y, label, '', link=node, w=296, pitch=28)

    live = p.panel(1000, y0, 270, 240, 'Right Now')
    p.readout(live, 15, 14, 'Inj PW', 'inj_pw', A.Page.chan_fmt('inj_pw'), 110)
    p.readout(live, 135, 14, 'Base PW', 'base_pw', A.Page.chan_fmt('base_pw'), 110)
    p.readout(live, 15, 84, 'Fuel Load', 'fuel_load', A.Page.chan_fmt('fuel_load'), 110)
    p.readout(live, 135, 84, 'Engine RPM', 'rpm', A.Page.chan_fmt('rpm'), 110)
    p.readout(live, 15, 154, 'Lambda', 'lambda_1', A.Page.chan_fmt('lambda_1'), 110)
    p.readout(live, 135, 154, 'Target', 'lambda_target', A.Page.chan_fmt('lambda_target'), 110)
    return p


def _corr_row(p, grid, y, name, enable, chan, link):
    """One switchboard row: the enable, the name as the way to its page, and what it is doing now."""
    g = p.group()
    p.add(p._new('checkbox', 12, y + 4, A.CHECK_H, A.CHECK_H, {'signalName': enable}, g), into=grid)
    p.add(p._new('label', 56, y + 6, 300, A.LBL_H,
                 {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL, 'link': link}, g), into=grid)
    p.add(p._new('value', 380, y + 4, 110, A.CTL_H,
                 {'signalName': chan, 'format': '%.3f', 'fontName': A.FONT_LBL, 'align': 'Right',
                  'showUnit': '0', 'borderWidth': '1', 'borderRadius': '3',
                  # 1.000 is "no effect" — worth being able to see at a glance which rows are DOING
                  # something, without reading twelve numbers to three decimals.
                  'ranges': f'#2a2a2e,#8a8f98,,,,{chan} == 1,'}, g), into=grid)
    p.add(p._new('label', 510, y + 6, 100, A.LBL_H,
                 {'labelText': chan.replace('fuel_corr_', ''), 'align': 'Left', 'fontName': A.FONT_SMALL,
                  'fgColor': C_DIM}, g), into=grid)


# One node per table under a stage, the way the sample tree does it.
STAGE_TABLES = [
    ('Dead Time',               'dead_time_table',      [('Dead Time', 'pw_add_deadtime', '%.0f'),
                                                         ('Battery', 'battery', '%.2f'),
                                                         ('Press Diff', 'inj_press_diff', '%.0f')],
     'The injector\'s own data: opening time against battery voltage and fuel pressure.'),
    ('Flow Rate',               'inj_flow_table',       [('Inj PW', 'inj_pw', '%.2f'),
                                                         ('Fuel Press', 'fuel_pressure', '%.0f'),
                                                         ('Battery', 'battery', '%.2f')],
     'Static flow, corrected for the pressure the injector actually sees.'),
    ('Short Pulse Width Adder', 'short_pw_adder_table', [('Inj PW', 'inj_pw', '%.2f'),
                                                         ('Base PW', 'base_pw', '%.0f')],
     'Injectors stop being linear at very short pulses; this is the correction for it.'),
    ('Firing Angle BTDC',       'inj_angle_table',      [('Engine RPM', 'rpm', '%.0f'),
                                                         ('Fuel Load', 'fuel_load', '%.0f'),
                                                         ('Inj PW', 'inj_pw', '%.2f')],
     'Where the pulse sits in the cycle, read as the injector timing method says.'),
    ('Staging Duty',            'staging_duty_table',   [('Inj PW', 'inj_pw', '%.2f'),
                                                         ('Engine RPM', 'rpm', '%.0f')],
     'The duty this stage reaches before the charge overflows to the next one.'),
]

TRANSIENT_TABLES = [
    ('Enrich Rate',     TT + 'tt_enrich_rate_table',      [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Load Rate', 'tt_load_rate', '%.1f'),
                                                           ('Start Load', 'tt_start_load', '%.1f')], ''),
    ('Enrich Amount',   TT + 'tt_enrich_sync_table',      [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Engine RPM', 'rpm', '%.0f'),
                                                           ('MAP', 'map', '%.0f')], ''),
    ('Async Amount',    TT + 'tt_enrich_async_table',     [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Engine RPM', 'rpm', '%.0f')], ''),
    # BOTH decay tables are read inside `if (cfg_->enable_decay && ...)` (TransientThrottle.cpp:183).
    # With decay off the enrichment simply follows the rate table and neither curve is consulted, so a
    # fifth element carries the flag the firmware actually tests.
    ('Enrich Decay',    TT + 'tt_enrich_decay_table',     [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Engine RPM', 'rpm', '%.0f')], '',
     'transient_throttle.enable_decay'),
    ('Disenrich Rate',  TT + 'tt_disenrich_rate_table',   [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Load Rate', 'tt_load_rate', '%.1f')], ''),
    ('Disenrich Amount', TT + 'tt_disenrich_amount_table', [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                            ('Engine RPM', 'rpm', '%.0f')], ''),
    ('Disenrich Decay', TT + 'tt_disenrich_decay_table',  [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Engine RPM', 'rpm', '%.0f')], '',
     'transient_throttle.enable_decay'),
    ('Ignition Correction', TT + 'tt_ign_corr_table',     [('Transient Ign', 'ign_corr_transient', '%.1f'),
                                                           ('Advance', 'advance', '%.1f')], ''),
    ('Coolant Temp Corr', TT + 'tt_clt_corr_table',       [('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                                           ('Coolant', 'clt', '%.0f')], ''),
]
