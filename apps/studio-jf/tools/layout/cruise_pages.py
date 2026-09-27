"""Cruise control: the conditions that drive it, the window it may work in, and what it is doing now.

The page follows the order a person actually asks about it. WHAT PRESSES IT — eight conditions, each
one an expression, because a stalk button usually means several things and the same position can appear
in as many of them as it has meanings. WHAT STOPS IT — the brake, clutch and handbrake inputs, which are
deliberately NOT expressions, and the speed window. HOW HARD IT PULLS — three gain curves, on their own
pages. And then the strip that answers the only question anyone asks of a cruise system that did
nothing: which state is it in, and which of the eighteen reasons is holding it off.

That last part is why this is not a generic feature page: feature_page() emits its live strip only when
it has no extra panel, and cruise needs both the strip and the links to its curves.

FIVE NODES, NOT FIVE VIEWS. These were one page with a view chooser at the top, which nothing else in
this document does: the only two other subtabs() callers are the Diagnostics WORKSPACE and knock's
cylinder picker, and the second of those chooses which instance of ONE thing to show rather than hiding
a different set of settings behind a combo. A tree of six hundred pages is how everything else here is
found, so cruise is found that way too.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import fuel_tree as FT
from author import C_DIM, C_RED, C_AMBER, C_GREEN, C_BLUE

C    = 'cruise_control.'
NODE = 'Configuration/Vehicle Functions/Cruise Control'
ON   = '[#cruise_control.enabled] == 1'


# The gain curves, in the order they are read: what it does about the error now, about an error that
# will not go away, and about an error that is CHANGING.
CURVES = [
    ('Proportional Gain', 'cruise_p_gain',
     'Throttle percent per kph of error, applied the moment the error appears. It decides how hard the '
     'system answers a hill arriving. Too much and the car surges: it overshoots, backs off, repeats.'),
    ('Integral Gain', 'cruise_i_gain',
     'Throttle percent per kph of error per second, accumulating while the error refuses to go away. '
     'This is what removes the last of a steady error so the car settles ON the set speed rather than a '
     'little under it up a grade. It is highest at zero error on purpose — on target the loop is purely '
     'integral, which is what makes it sit still instead of hunting.'),
    ('Derivative Gain', 'cruise_d_gain',
     'Throttle percent per kph-per-second of error CHANGE — the brake on an overshoot. It answers how '
     'fast the gap is opening or closing rather than how big it is, which is what lets the other two be '
     'set hard enough to actually hold a speed.'),
]

# One lamp per state. Same title lit or unlit, so the strip always names every state and only the colour
# moves — a lamp that disappears when off tells you nothing about what it would have said.
STATES = [('OFF', 0, C_DIM), ('DISABLED', 1, C_DIM), ('READY', 2, C_BLUE),
          ('CRUISING', 3, C_GREEN), ('FAULT', 4, C_RED)]

# The reasons, in bit order, exactly as CruiseControl::Inhibit declares them. A refusal has to name
# itself or "I pressed Set and nothing happened" has no answer.
REASONS = [(0, 'Brake'), (1, 'Clutch'), (2, 'Handbrake'), (3, 'Below Min'), (4, 'Above Max'),
           (5, 'No Speed'), (6, 'Speed Stale'), (7, 'Speed Jump'), (8, 'Wheel Diff'),
           (9, 'Switch Fault'), (10, 'Bad Condition'), (11, 'Runaway'), (12, 'RPM Low'),
           (13, 'RPM High'), (14, 'Gear'), (15, 'Pedal Fault'),
           (17, 'Unmonitored'), (18, 'No Brake')]


def _para(p, panel, y, w, text):
    """One paragraph in a panel, returning the y the NEXT one starts at. wrapped() measures itself with
    the app's real font, so chaining beats hand-picked offsets that drift when a word changes."""
    wid = p.wrapped(10, y, w, text, into=panel)
    return y + wid['h'] + 10


def _readouts(p, rows):
    """The handful of numbers you read WHILE editing this page, along the bottom at the width a number
    actually needs — the shape fuel_tree.table_page uses, so a child page here looks like one there."""
    from ruler import ruler
    ws = [max(A.Page.chan_value_px(ch, 20) + 24, int(ruler().width(lbl, 15)) + 12)
          for lbl, ch, _f in rows]          # a state channel reads in WORDS — see chan_value_px
    x, y = 10, A.CANVAS_H - 66
    for (lbl, ch, fmt), cw in zip(rows, ws):
        p.readout(None, x, y, lbl, ch, fmt, w=cw)
        x += cw + 12


LIVE = [('Cruise State', 'cruise_state', '%.0f'), ('Set Speed', 'cruise_target', '%.1f'),
        ('Vehicle Speed', 'vehicle_spd', '%.1f'), ('Speed Error', 'cruise_error_kph', '%.1f'),
        ('Cruise Demand', 'cruise_demand', '%.1f')]


def page_cruise():
    """The module: what presses it, and what it is doing right now."""
    p = A.Page(title='Cruise Control')
    y0 = p.head('Holds a road speed by asking the throttle for it — as a floor UNDER the pedal, so '
                'pressing harder always wins. What each stalk button MEANS is a condition you write, '
                'so one button can carry several meanings.', enable=C + 'enabled')

    btn_rows = [('Enable When', 'enable_expr'), ('Disable When', 'disable_expr'),
                ('Enable/Disable When', 'enable_disable_expr'), ('Set When', 'set_expr'),
                ('Resume When', 'resume_expr'), ('Cancel When', 'cancel_expr'),
                ('Speed Up When', 'bump_up_expr'), ('Speed Down When', 'bump_down_expr')]
    h = A.panel_h(len(btn_rows) + 2, A.ROW, top=12, bottom=6)
    btn = p.panel(10, y0, 620, h, 'What presses it')
    y = 12
    for label, f in btn_rows:
        y = p.field(btn, 10, y, label, C + f, 'expression', 380, lbl_w=150, enable=ON)
    y = p.field(btn, 10, y, 'Long Press Time', C + 'long_press_ms', 'configedit', 110, 'ms',
                lbl_w=150, enable=ON)
    p.field(btn, 10, y, 'State At Power-On', C + 'power_on_state', 'enum', 190, lbl_w=150, enable=ON)

    note = p.panel(640, y0, 620, h, 'How a press is read')
    ny = _para(p, note, 12, 600,
               'A tap is acted on when the button is RELEASED, and a press held past the Long Press '
               'Time is a hold instead — which is what stops one press from being both. Only Speed Up '
               'and Speed Down have a hold meaning: tapped they move the set speed one increment, held '
               'they ramp it continuously.')
    ny = _para(p, note, ny, 600,
               'Each condition is an expression over any channel — `cruise_sw == 2` for a stalk '
               'position, a plain switch, a CAN flag, or a rule with more in it. A button that means '
               'several things simply appears in several of them: put the same position in Set When '
               'and Speed Down When and you have described a Set/Coast button exactly.')
    _para(p, note, ny, 600,
          'A condition whose inputs cannot be trusted reads FALSE, so a stalk that has lost its '
          'calibration cannot engage cruise. That is also why the brake and clutch are NOT conditions '
          '— see the Limits page.')

    y1 = y0 + h + 10
    links = p.panel(10, y1, 620, A.panel_h(5, 28, top=12, bottom=6), 'The rest of it')
    y = 12
    for label in ('Limits', 'Tuning') + tuple(c[0] for c in CURVES):
        y = p.switch(links, 10, y, label, '', link=f'{NODE}/{label}', w=570, pitch=28)

    live = p.panel(640, y1, 620, A.panel_h(5, 28, top=12, bottom=6), 'Right Now')
    p.readout(live,  15, 12, 'Set Speed',     '[$cruise_target]',    '%.1f', 120)
    p.readout(live, 145, 12, 'Vehicle Speed', '[$vehicle_spd]',      '%.1f', 120)
    p.readout(live, 275, 12, 'Speed Error',   '[$cruise_error_kph]', '%.1f', 120)
    p.readout(live, 405, 12, 'Demand',        '[$cruise_demand]',    '%.1f', 120)
    for i, (title, val, colour) in enumerate(STATES):
        p.add(p._new('indicator', 10 + i * 118, 74, 110, 34,
                     {'signalName': f'[$cruise_state] == {val}', 'onTitle': title, 'offTitle': title,
                      'onBg': colour, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    return p


def page_limits():
    """Everything that stops it, and everything that makes it give up."""
    p = A.Page(title='Cruise Limits')
    y0 = p.head('What cancels cruise, the window it may work in at all, and the checks that make it '
                'hand back. Every one of these lights its own reason on the Cruise Control page when '
                'it is the thing holding cruise off.')

    stop_rows = [('Brake Signal', 'brake_sig', 'enum'), ('Clutch Signal', 'clutch_sig', 'enum'),
                 ('Handbrake Signal', 'handbrake_sig', 'enum'),
                 ('Minimum Speed', 'min_speed_kph', 'configedit'),
                 ('Maximum Speed', 'max_speed_kph', 'configedit'),
                 ('Minimum RPM', 'min_rpm', 'configedit'), ('Maximum RPM', 'max_rpm', 'configedit'),
                 ('Require A Known Gear', 'gear_check_enabled', 'checkbox'),
                 # …and the minimum only means something once a known gear is required.
                 ('Minimum Gear', 'min_gear', 'configedit', '[#cruise_control.gear_check_enabled] == 1')]
    h_stop = A.panel_h(len(stop_rows), A.ROW, top=12, bottom=6)
    stop = p.panel(10, y0, 430, h_stop, 'What stops it')
    y = 12
    for row in stop_rows:
        label, f, kind = row[0], row[1], row[2]
        gate = ON if len(row) < 4 else f'{ON} and {row[3]}'
        y = p.field(stop, 10, y, label, C + f, kind, 190 if kind == 'enum' else 110,
                    lbl_w=190, enable=gate)

    # The runaway TIME is only consulted once a runaway ERROR has been set — zero switches the whole
    # check off, and a timer for a check that is not running is a setting that does nothing.
    safe_rows = [('Max Control System Error', 'max_error_kph'),
                 ('Max Error Time', 'max_error_ms', '[#cruise_control.max_error_kph] != 0'),
                 ('Max Wheel Speed Difference', 'max_wheel_diff_kph'),
                 ('Speed Signal Timeout', 'spd_max_age_ms'),
                 ('Max Plausible Acceleration', 'max_accel_kph_s'),
                 ('Switch Fault Time', 'sw_fault_ms')]
    safe = p.panel(450, y0, 440, A.panel_h(len(safe_rows), A.ROW, top=12, bottom=6),
                   'When it gives up')
    y = 12
    for row in safe_rows:
        label, f = row[0], row[1]
        gate = ON if len(row) < 3 else f'{ON} and {row[2]}'
        y = p.field(safe, 10, y, label, C + f, 'configedit', 110, lbl_w=220, enable=gate)

    why = p.panel(900, y0, 360, h_stop, 'Why these are signals')
    ny = _para(p, why, 12, 340,
               'The brake, clutch and handbrake are signals rather than conditions, on purpose. An '
               'expression fails to FALSE when its inputs cannot be trusted, which is right for Set — '
               'a dead stalk must not engage cruise — and exactly wrong for a brake switch whose wire '
               'has fallen off, where false reads as "not braking". An INVALID signal here reads as '
               'PRESSED.')
    _para(p, why, ny, 340,
          "That fail-safe only works if something is watching, and a sensor's own fault checks ship "
          'switched OFF. Cruise will not arm while an assigned brake, clutch or handbrake input has '
          'no checks enabled on its sensor page, and says so as Unmonitored.')

    y1 = y0 + h_stop + 10
    inh = p.panel(10, y1, 1250, A.panel_h(2, 30, top=12, bottom=6), 'Why it will not engage')
    for i, (bit, title) in enumerate(REASONS):
        col, row = i % 10, i // 10
        p.add(p._new('indicator', 10 + col * 122, 12 + row * 30, 116, 26,
                     {'signalName': f'bit([$cruise_inhibit], {bit})', 'onTitle': title,
                      'offTitle': title, 'onBg': C_AMBER, 'onFg': '#000000', 'offBg': '#2a2a2e',
                      'offFg': '#6e7178', 'fontName': A.FONT_SMALL}), into=inh)
    _readouts(p, LIVE)
    return p


def page_tuning():
    """How hard it pulls, and how fast it is allowed to change its mind."""
    p = A.Page(title='Cruise Tuning')
    y0 = p.head('How the set speed moves when you ask it to, and how quickly the throttle floor may '
                'follow. The three gain curves that decide how hard it answers an error are pages of '
                'their own.')

    tune_rows = [('Speed +/- Increment', 'speed_increment_kph'), ('Accelerate Rate', 'accel_rate_kph_s'),
                 ('Coast Rate', 'coast_rate_kph_s'), ('Maximum Throttle Demand', 'max_demand_pct'),
                 ('Max Throttle Ramp Rate', 'max_ramp_pct_s'), ('Cancel Decay Time', 'cancel_decay_s')]
    h = A.panel_h(len(tune_rows), A.ROW, top=12, bottom=6)
    tune = p.panel(10, y0, 430, h, 'How it pulls')
    y = 12
    for label, f in tune_rows:
        y = p.field(tune, 10, y, label, C + f, 'configedit', 110, lbl_w=200, enable=ON)

    crv = p.panel(450, y0, 440, A.panel_h(len(CURVES), 28, top=12, bottom=6), 'The Three Gain Curves')
    y = 12
    for label, _f, _blurb in CURVES:
        y = p.switch(crv, 10, y, label, '', link=f'{NODE}/{label}', w=390, pitch=28)

    gnote = p.panel(900, y0, 360, h, 'How the gains are read')
    ny = _para(p, gnote, 12, 340,
               'All three curves are indexed on the speed error itself — set speed minus road speed — '
               'so a 1 kph drift is answered gently and a 10 kph hill is met hard, out of one set of '
               'curves. They are asymmetric about zero because this module may only ever ADD throttle.')
    _para(p, gnote, ny, 340,
          'The floor starts exactly where the pedal was when you pressed Set, so the hand-over is '
          'seamless; the ramp rate limits everything after that.')
    _readouts(p, LIVE)
    return p


def page_curve(label, field, blurb):
    """One gain curve. The shared table page, so these sit beside idle's three gain surfaces and look
    like them: the grid sized from the meta's own allocation, and the handful of numbers you read WHILE
    editing the cells along the bottom. No second view of the same bytes — the table is the editor."""
    return FT.table_page(label, C + field,
                         [('Speed Error', 'cruise_error_kph', '%.1f'),
                          ('Set Speed', 'cruise_target', '%.1f'),
                          ('Vehicle Speed', 'vehicle_spd', '%.1f'),
                          ('Cruise Demand', 'cruise_demand', '%.1f')],
                         blurb)


def pages():
    """The module page and its five children, keyed by node path."""
    out = {NODE: page_cruise(),
           f'{NODE}/Limits': page_limits(),
           f'{NODE}/Tuning': page_tuning()}
    for label, field, blurb in CURVES:
        out[f'{NODE}/{label}'] = page_curve(label, field, blurb)
    return out
