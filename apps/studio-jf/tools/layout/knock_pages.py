"""Knock control: the pages for hearing detonation, and for everything it learns while listening.

Knock is a listening problem before it is a timing problem — WHERE in the cycle the ECU listens, WHAT it
compares against, and only then what it does about what it heard. The module page follows that order.

The three things a generic settings page could never show live here too: which knock input hears which
cylinder (a per-cylinder assignment, not a scalar), the two tables that decide "how loud is too loud" and
"how much may be pulled", and the LEARNED noise floor — twelve tables of what quiet sounds like at each
operating point, with the sample count beside each so a cell nobody has visited is visible as such.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from author import C_DIM, C_RED, C_AMBER, C_GREEN

K    = 'knock.'
IGN  = 'Configuration/Ignition Tuning'
NODE = f'{IGN}/Knock Control'
ON   = '[#knock.enabled] == 1'
CYLS = 12


def page_knock():
    """The module: where it listens, what counts as knock, what it does, and what it has learned."""
    p = A.Page(title='Knock Control')
    y0 = p.head('Listens in a window around each cylinder\'s combustion, compares what it hears to the '
                'learned noise floor at that operating point, and pulls timing when the difference says '
                'detonation.', enable=K + 'enabled')

    # ---- 1 · Listening ---------------------------------------------------------------------------
    det = p.panel(10, y0, 430, A.panel_h(5, A.ROW, top=12, bottom=6), '1 · Listening')
    y = 12
    y = p.field(det, 10, y, 'Detection Source', K + 'source', 'enum', 190, enable=ON)
    # Read only on the external source path; with the onboard ADC burst it is a selector that selects
    # nothing (tools/gate_audit.py).
    y = p.field(det, 10, y, 'External Intensity', K + 'external_intensity_sig', 'enum', 190,
                enable=f'{ON} and [#knock.source] != 0')
    y = p.field(det, 10, y, 'Knock Frequency', K + 'knock_frequency', 'configedit', 110, 'Hz', enable=ON)
    y = p.field(det, 10, y, 'Window Start', K + 'window_start_btdc', 'configedit', 110, 'deg', enable=ON)
    p.field(det, 10, y, 'Window Duration', K + 'window_duration_deg', 'configedit', 110, 'deg', enable=ON)
    h_det = A.panel_h(5, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h_det + 6,
           'Frequency 0 derives the resonance from the bore. The window is measured from each '
           'cylinder\'s own TDC (+ before, - after, like spark advance), so it follows the firing order '
           'without being told it — open it too wide and valve noise arrives inside it.', w=420)

    # ---- 2 · Response ----------------------------------------------------------------------------
    y_res = y0 + h_det + 92
    res = p.panel(10, y_res, 430, A.panel_h(3, A.ROW, top=10, bottom=4), '2 · Response')
    y = 12
    y = p.field(res, 10, y, 'Retard Step', K + 'retard_step_deg', 'configedit', 110, 'deg', enable=ON)
    y = p.field(res, 10, y, 'Reapply Rate', K + 'retard_reapply_rate', 'configedit', 110, enable=ON)
    p.field(res, 10, y, 'Ignore Below TPS', K + 'suppress_min_tps', 'configedit', 110, '%', enable=ON)
    h_res = A.panel_h(3, A.ROW, top=10, bottom=4)

    tbl = p.panel(10, y_res + h_res + 8, 430, A.panel_h(2, 28, top=10, bottom=4), 'The Two Tables')
    y = 12
    for label, node in (('Knock Threshold  (how loud is too loud)', f'{NODE}/Threshold'),
                        ('Max Retard  (how much may be pulled)', f'{NODE}/Max Retard')):
        y = p.switch(tbl, 10, y, label, '', link=node, w=380, pitch=28)

    # ---- 3 · What it has learned -----------------------------------------------------------------
    learn = p.panel(450, y0, 400, A.panel_h(5, A.ROW, top=12, bottom=6), '3 · Noise Floor  (learned)')
    y = 12
    y = p.field(learn, 10, y, 'Noise Floor Seed', K + 'noise_seed_db', 'configedit', 110, 'dB', enable=ON)
    y = p.field(learn, 10, y, 'Learn Rate', K + 'learn_rate', 'configedit', 110, enable=ON)
    y = p.field(learn, 10, y, 'Fast Samples', K + 'learn_fast_n', 'configedit', 110, enable=ON)
    y = p.field(learn, 10, y, 'Learn Dwell', K + 'learn_dwell_ms', 'configedit', 110, 'ms', enable=ON)
    p.field(learn, 10, y, 'Learn Above RPM', K + 'learn_min_rpm', 'configedit', 110, 'RPM', enable=ON)
    h_learn = A.panel_h(5, A.ROW, top=12, bottom=6)
    p.note(450, y0 + h_learn + 4,
           'The floor is what QUIET sounds like at each RPM and load, learned while nothing is knocking. '
           'Runtime state persisted to the SD card, not tune data.', w=390)

    y_pre = y0 + h_learn + 62
    h_pre = A.panel_h(8, A.ROW, top=10, bottom=4)
    pre = p.panel(450, y_pre, 400, h_pre, 'Pre-Ignition')
    pon = f'{ON} and [#knock.preign_enabled] == 1'
    y = 12
    y = p.field(pre, 10, y, 'Pre-Ignition Detection', K + 'preign_enabled', 'checkbox', enable=ON)
    # THE WINDOW FOLLOWS THE SPARK: with this on, each window opens this far before its own cylinder's
    # spark, wherever the timing is — the setting that makes the rest of this panel able to see anything.
    y = p.field(pre, 10, y, 'Look-Ahead', K + 'preign_lookahead_deg', 'configedit', 110, 'deg', enable=pon)
    y = p.field(pre, 10, y, 'Pre-Window Fraction', K + 'preign_pre_frac', 'configedit', 110, enable=pon)
    y = p.field(pre, 10, y, 'Margin', K + 'preign_margin_db', 'configedit', 110, 'dB', enable=pon)
    y = p.field(pre, 10, y, 'Extreme', K + 'preign_extreme_db', 'configedit', 110, 'dB', enable=pon)
    y = p.field(pre, 10, y, 'Events to Act', K + 'preign_events_to_act', 'configedit', 110, enable=pon)
    p.field(pre, 10, y, 'Cut Fuel', K + 'preign_cut_fuel', 'checkbox', enable=pon)     # one row: the two
    y = p.field(pre, 200, y, 'Cut Spark', K + 'preign_cut_spark', 'checkbox', enable=pon)   # are one choice
    p.field(pre, 10, y, 'Cut Hold', K + 'preign_clear_s', 'configedit', 110, 's', enable=pon)

    # ---- Which input hears which cylinder ---------------------------------------------------------
    # A per-cylinder assignment, which no list of scalars can express: an inline four with two sensors
    # and a V6 with one are both normal, and only this grid can say which is which.
    grid = p.panel(860, y0, 410, A.panel_h(CYLS + 1, 26, top=34, bottom=8), 'Sensor per Cylinder')
    for cx, cw, text in ((10, 90, 'Cylinder'), (110, 150, 'Knock Input'), (270, 120, 'Gain')):
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
    p.add(p._new('panel', 10, 28, 380, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    y = 34
    for i in range(CYLS):
        # Hidden, not greyed, past the engine's cylinder count — a row for a cylinder that does not
        # exist is not a setting you are not allowed to change, it is not a setting.
        vis = f'[#engine.cylinder_count] >= {i + 1}'
        g = p.group()
        p.add(p._new('label', 10, y + 4, 90, A.LBL_H,
                     {'labelText': str(i + 1), 'align': 'Left', 'fontName': A.FONT_LBL,
                      'condition': vis}, g), into=grid)
        p.add(p._new(A.Page.enum_control(f'{K}cyl_sensor[{i}].input'), 110, y + 2, 150, A.CTL_H,
                     {'signalName': f'{K}cyl_sensor[{i}].input', 'condition': vis,
                      'enableCondition': ON}, g), into=grid)
        p.add(p._new('configedit', 270, y + 2, 90, A.CTL_H,
                     {'signalName': f'{K}cyl_sensor[{i}].gain', 'condition': vis,
                      'enableCondition': ON}, g), into=grid)
        y += 26

    p.note(860, y0 + A.panel_h(CYLS + 1, 26, top=34, bottom=8) + 6,
           'Auto takes the input from the cylinder\'s BANK, which is right whenever sensors and banks '
           'line up. Set it by hand for an inline engine with two sensors, or a V with one.', w=400)

    # ---- Sensor health -------------------------------------------------------------------------------
    # OPT-IN, because a quiet engine at idle can sit below any fixed level. One row in the free space
    # under the sensor grid: the switch and the level read as one sentence.
    sh = p.panel(860, 506, 410, A.panel_h(1, A.ROW, top=12, bottom=8), 'Sensor Health')
    p.add(p._new('checkbox', 10, 15, A.CHECK_H, A.CHECK_H,
                 {'signalName': K + 'quiet_check_en', 'enableCondition': ON}), into=sh)
    p.add(p._new('label', 40, 16, 230, A.LBL_H,
                 {'labelText': 'Fault if quieter than', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'enableCondition': ON}), into=sh)
    p.add(p._new('configedit', 280, 12, 110, A.CTL_H,
                 {'signalName': K + 'quiet_db',
                  'enableCondition': f'{ON} and [#knock.quiet_check_en] == 1'}), into=sh)

    # ---- Live ------------------------------------------------------------------------------------
    # Level is what it hears, in dB over the learned floor; retard is what it has taken away, and it
    # comes back at the reapply rate once the noise stops. (Said in the page's own note rather than
    # under the numbers — the strip is as tall as the panels above it leave room for.)
    # UNDER THE LOWEST PANEL above it, not at a fixed 600: the Pre-Ignition panel grew a row (Look-Ahead)
    # and ran into it.
    live = p.panel(10, max(600, y_pre + h_pre + 8), 840, 92, 'Right Now')
    for n, (lbl, ch) in enumerate((('Knock Level', 'knock_level'), ('Knock Retard', 'knock_retard'),
                                   ('Knock Count', 'knock_count'), ('Advance', 'advance'),
                                   ('Engine RPM', 'rpm'), ('Fuel Load', 'fuel_load'))):
        p.readout(live, 15 + n * 138, 8, lbl, ch, A.Page.chan_fmt(ch), 130)

    nf = p.panel(860, 600, 410, 92, 'Learned Floors')
    p.switch(nf, 10, 8, 'Noise Floor  (per cylinder, learned)', '', link=f'{NODE}/Noise Floor', w=356)
    p.add(p._new('label', 10, 40, 356, A.LBL_H,
                 {'labelText': 'What quiet sounds like, and how well learned.',
                  'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=nf)
    return p


def page_table(title, table, blurb, live):
    """One of the two decision tables, with what it is deciding about right now beside it."""
    p = A.Page(title=title)
    p.table(10, A.BAND, 1000, A.CANVAS_H - A.BAND - 8, table, enable=ON)
    panel = p.panel(1020, A.BAND, 250, A.CANVAS_H - A.BAND - 8, title)
    y = 14
    # THIS TABLE'S OWN OUTPUT FIRST — what it is deciding right now, at the cell the trace is on.
    # See fuel_tree.table_page: bound to the table path, which Cache::solveTable interpolates live.
    live = [(title, table)] + list(live)
    for lbl, ch in live:
        fmt = A.table_fmt(ch) if '.' in ch else A.Page.chan_fmt(ch)
        p.readout(panel, 12, y, lbl, ch, fmt, w=210)
        y += 62
    p.note(12, y + 8, blurb, w=210, into=panel)
    p.add(p._new('label', 12, A.CANVAS_H - A.BAND - 60, 210, A.LBL_H,
                 {'labelText': 'Knock Control', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': NODE}), into=panel)
    return p


def page_noise_floor():
    """The learned floor, one cylinder at a time — the level beside the sample count that produced it.

    Twelve pairs of tables is twenty-four pages nobody would open. A chooser makes it one page and one
    question ("which cylinder"), and putting the SAMPLE COUNT beside the level is the whole point: a
    floor cell with no samples is the seed, not something the engine has taught the ECU, and the two
    look identical until you can see both.
    """
    p = A.Page(title='Noise Floor')
    conds = p.subtabs(10, A.BAND, 1260, 40, 'knock_noise_cyl', [f'Cyl {i + 1}' for i in range(CYLS)])
    y0 = A.BAND + 46
    for i, cond in enumerate(conds):
        # One pair per cylinder, stacked in the same place and shown by the chooser — the same shape
        # the Diagnostics tab uses for its views.
        # EACH GRID IS AS WIDE AS ITS OWN NUMBERS. These were 600 and 610 — a round pair that fitted the
        # page rather than the tables: the dB floor prints THREE decimals and needed 634, so it scrolled,
        # while the sample counts print none and needed 401, so 610 was 200px of empty grid beside it.
        lw, _ = A.table_box(f'{K}knock_noise_{i + 1}')
        nw, _ = A.table_box(f'{K}knock_noise_n_{i + 1}')
        lvl = p.panel(10, y0, lw + 20, A.CANVAS_H - y0 - 116,
                      f'Cylinder {i + 1} — Noise Floor  (dB)', cond=cond)
        p.table(10, 10, lw, A.CANVAS_H - y0 - 156, f'{K}knock_noise_{i + 1}', into=lvl)
        cnt = p.panel(lw + 40, y0, nw + 20, A.CANVAS_H - y0 - 116,
                      f'Cylinder {i + 1} — Samples', cond=cond)
        p.table(10, 10, nw, A.CANVAS_H - y0 - 156, f'{K}knock_noise_n_{i + 1}', into=cnt)

    live = p.panel(10, A.CANVAS_H - 106, 1260, 90, 'Right Now')
    for n, (lbl, ch) in enumerate((('Knock Level', 'knock_level'), ('Knock Retard', 'knock_retard'),
                                   ('Engine RPM', 'rpm'), ('Fuel Load', 'fuel_load'))):
        p.readout(live, 15 + n * 150, 8, lbl, ch, A.Page.chan_fmt(ch), 140)
    p.wrapped(620, 12, 620,
              'Learned state, persisted to the SD card — it is not part of the tune and '
                               'a tune file will not carry it. A cell with no samples has never been '
                               'visited: what it holds is the seed.',
              into=live)
    return p


def pages():
    """The whole family, keyed by node path — the module page and its three children."""
    return {
        NODE: page_knock(),
        f'{NODE}/Threshold': page_table(
            'Knock Threshold', K + 'knock_threshold_table',
            'How far above the learned floor a measurement has to be before it counts as knock. Too low '
            'and mechanical noise reads as detonation; too high and real knock goes unheard.',
            [('Knock Level', 'knock_level'), ('Engine RPM', 'rpm'), ('Fuel Load', 'fuel_load')]),
        f'{NODE}/Max Retard': page_table(
            'Max Knock Retard', K + 'knock_max_retard_table',
            'The most timing knock control may pull at each operating point, whatever it hears. It is '
            'the floor under the authority — the engine still runs on what is left.',
            [('Knock Retard', 'knock_retard'), ('Advance', 'advance'), ('Engine RPM', 'rpm')]),
        f'{NODE}/Noise Floor': page_noise_floor(),
    }
