"""Category pages: the switchboard style, applied above the detail pages.

The Sensors page already worked this way and it is the right shape for a category: a screen of ticks
answering "what is this ECU doing", where the name of each thing is the way into its own page. These
build the same thing for the Configuration root, for Fuel, and for Ignition.

Every link is a NODE PATH — it has to name a node that exists or it goes nowhere, which apply_top.py
checks before writing.
"""
import author as A
from author import C_DIM, C_RED, C_AMBER, C_GREEN, C_BLUE
# ONE table-page routine, not a copy per file. The inlined copies here are what let the Advance page
# drift into a shape no other page had, and carry a two-pixel label/value overlap nobody could see in
# the source because it read as deliberate arithmetic.
from fuel_pages import _table_page

CFG = 'Configuration'


# ---- the root: every function the ECU has, and whether it is on ---------------------------------
# (label, enable field, node path). A blank enable field = a function with no switch of its own.
ROOT_GROUPS = [
    ('Fuel & Air', [
        ('Fuel',                  '',                              f'{CFG}/Fuel Tuning'),
        ('Lambda / O2 Control',   'lambda.enabled',            f'{CFG}/Fuel Tuning/O2 Control'),
        ('Transient Throttle',    'transient_throttle.enabled',    f'{CFG}/Fuel Tuning/Transient Throttle'),
        ('Deceleration Fuel Cut', 'dfco.enabled',                  f'{CFG}/Engine Functions/Deceleration Fuel Cut'),
    ]),
    ('Spark', [
        ('Ignition',              '',                              f'{CFG}/Ignition Tuning'),
        ('Knock Detection',       'knock.enabled',                 f'{CFG}/Ignition Tuning/Knock Control'),
        ('Pre-Ignition Detection','knock.preign_enabled',          f'{CFG}/Ignition Tuning/Knock Control'),
    ]),
    ('Air Path', [
        ('Electronic Throttle',   'electronic_throttle.etb[0].enabled', f'{CFG}/Engine Functions/Electronic throttle'),
        ('Idle Control',          'idle.enabled',                  f'{CFG}/Engine Functions/Idle Control'),
        ('Boost Control',         'boost.enabled',                 f'{CFG}/Engine Functions/Boost Control'),
        ('Cam Control (VVT)',     'vvt_control.enabled',           f'{CFG}/Engine Functions/Cam Control'),
        ('Variable Valve Lift',   'vvl.enabled',                   f'{CFG}/Engine Functions/Variable Valve Lift'),
        ('Cruise Control',        'cruise_control.enabled',        f'{CFG}/Vehicle Functions/Cruise Control'),
    ]),
    ('Limits & Protection', [
        ('Rev Limiter',           'rev_limiter.enabled',           f'{CFG}/Protection/Rev Limiter'),
        ('Lambda Protection',     'lambda_protect.enabled',        f'{CFG}/Protection/Lambda Protection'),
        ('EGT Protection',        'egt_protect.enabled',           f'{CFG}/Protection/EGT Protection'),
        ('Engine Protection',     '',                              f'{CFG}/Protection/Engine Protection'),
    ]),
    ('Motorsport', [
        ('Launch Control',        'launch.enabled',                f'{CFG}/Vehicle Functions/Launch Control'),
        ('Flat Shift',            'flat_shift.enabled',            f'{CFG}/Vehicle Functions/Flat Shift'),
        ('Traction Control',      'traction_control.enabled',      f'{CFG}/Vehicle Functions/Traction Control'),
        ('Anti-Lag',              'anti_lag.enabled',              f'{CFG}/Engine Functions/Anti-Lag'),
        ('Nitrous',               'nitrous.enabled',               f'{CFG}/Engine Functions/Nitrous'),
        ('Water & Methanol',      'wmi.enabled',                   f'{CFG}/Engine Functions/Water & Methanol'),
        ('Pit Speed Limiter',     'pit_limiter.enabled',           f'{CFG}/Vehicle Functions/Pit Speed Limiter'),
        ('Gear Detection',        'gear_detect.enabled',           f'{CFG}/Vehicle Functions/Gear Detection'),
        ('Vehicle Speed',         'vehicle_speed.enabled',         f'{CFG}/Vehicle Functions/Vehicle Speed'),
        ('Torque Model',          'torque_model.enabled',          f'{CFG}/Engine Functions/Torque Model'),
    ]),
    ('Vehicle & System', [
        ('Sensors',               '',                              f'{CFG}/Sensors'),
        ('Alternator Control',    'alternator.enabled',            f'{CFG}/Electrical/Alternator Control'),
        ('Idle Stepper',          'stepper.enabled',               f'{CFG}/Engine Functions/Idle Stepper'),
        ('OBD over CAN',          'can.obd_enabled',               f'{CFG}/CAN Bus/OBD-II'),
        ('CAN1',                  'can.bus[0].enabled',            f'{CFG}/CAN Bus/CAN1'),
        ('CAN2',                  'can.bus[1].enabled',            f'{CFG}/CAN Bus/CAN2'),
        ('Datalogging',           'datalog.enabled',               f'{CFG}/Datalogging'),
        ('Lua Scripting',         'lua.enabled',                   f'{CFG}/Lua Scripting'),
    ]),
]


# The identity fields that were already on this page, by binding. Named here rather than copied off the
# live page: re-installing must produce the SAME page, and a build that reads its own last output grows
# a copy of itself every run (this one did — three runs, three switchboards inside the Vehicle panel).
VEHICLE_FIELDS = [('Engine', 'Engine'), ('Make', 'Make'), ('Model', 'Model'),
                  ('Name', 'Name'), ('Notes', 'Notes'), ('VIN', 'VIN')]


def page_root(_unused=None):
    """The master switchboard."""
    p = A.Page(title='Configuration')
    p.head('Every function this ECU has. Tick what the engine uses; the name is the way in.')

    # A ROW IS A ROW. This walked "field() - 24", and field() returns the next row 30px down — so each
    # one advanced six pixels and all five identity fields were drawn on top of each other, which is what
    # the panel looked like: a pile of labels beside an empty-looking box.
    veh = p.panel(10, A.TOP, 330, A.panel_h(len(VEHICLE_FIELDS) + 1, A.ROW, top=12, bottom=6), 'Vehicle')
    vy = 12
    for label, field in VEHICLE_FIELDS:
        vy = p.field(veh, 10, vy, label, f'vehicle.{field}', 'text', 180, lbl_w=90)
    p.add(p._new('label', 10, vy + 6, 300, A.LBL_H,
                 {'labelText': 'Vehicle Identity', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{CFG}/Engine Configuration/Vehicle Identity'}), into=veh)

    # Six columns of switches, each group a panel. Ordered the way an engine is commissioned: what makes
    # it run, then what protects it, then what it does on a track, then the housekeeping.
    x, y = 350, A.TOP
    for title, rows in ROOT_GROUPS:
        h = A.panel_h(len(rows), 25, top=12, bottom=4)
        panel = p.panel(x, y, 300, h, title)
        ry = 12
        for label, flag, link in rows:
            ry = p.switch(panel, 10, ry, label, flag, link=link)
        y += h + 10
        if y > 560:
            x, y = x + 310, A.TOP
    return p


# ---- Fuel: the branch's own switchboard ---------------------------------------------------------
BLEND_ONLY = '[#fuel_calculator.fuel_model] == 3'   # the air model that reads the Alpha-N VE map

FUEL_ROWS = [
    ('Fuel Setup',         '',                                   f'{CFG}/Engine Configuration/Fuel System/Fuel Setup'),
    ('VE Table',           '',                                   f'{CFG}/Fuel Tuning/VE Table'),
    ('Target Lambda',      '',                                   f'{CFG}/Fuel Tuning/Target Lambda'),
    # The Blend air model's low-RPM map is opt-in through the AIR MODEL, not a flag of its own: a second
    # switch beside the selector would be a second way to say the same thing. The row greys until Blend is
    # chosen. (Predicted MAP is MAP Prediction's table and is reached from the MAP Prediction row.)
    ('Alpha-N VE Table',   '',                                   f'{CFG}/Fuel Tuning/Alpha-N VE Table'),
    ('Prime Pulse',        'fuel_calculator.prime_enable',        f'{CFG}/Fuel Tuning/Fuel Prime Pulse'),
    ('Cranking',           'fuel_calculator.enable_cranking',     f'{CFG}/Fuel Tuning/Cranking'),
    ('Warmup (Coolant)',   '',                                   f'{CFG}/Fuel Tuning/Corrections/Coolant Temp'),
    ('Post Start',         '',                                   f'{CFG}/Fuel Tuning/Corrections/Post Start'),
    ('Corrections',        '',                                   f'{CFG}/Fuel Tuning/Corrections/Air Temp'),
    ('Cylinder Trims',     '',                                   f'{CFG}/Fuel Tuning/Corrections/Cylinder 1'),
    ('Injector Stage 1',   '',                                   f'{CFG}/Fuel Tuning/Stage 1/Setup'),
    ('MAP Prediction',     'fuel_calculator.map_predict_enabled', f'{CFG}/Fuel Tuning/MAP Prediction'),
    ('Wall Film',          'fuel_calculator.wallfilm_enabled',    f'{CFG}/Fuel Tuning/MAP Prediction'),
    ('Transient Fuel',     'transient_throttle.enabled',          f'{CFG}/Fuel Tuning/Transient Throttle'),
    # A ROW IS A NAME, and the indent already says it belongs to the one above. These read as lowercase
    # sentence fragments — "…decay" — in a column where every other row is a proper noun you can go to,
    # and an ellipsis that continues a label two rows up is not a name at all. Title Case like the rest,
    # named for the feature rather than for the tail of a sentence.
    ('   Async Pulses',    'transient_throttle.enable_async',     f'{CFG}/Fuel Tuning/Transient Throttle/Async Amount'),
    ('   Enrich Decay',    'transient_throttle.enable_decay',     f'{CFG}/Fuel Tuning/Transient Throttle/Enrich Decay'),
    ('   Disenrichment',   'transient_throttle.enable_disenrich', f'{CFG}/Fuel Tuning/Transient Throttle/Disenrich Rate'),
    # O2 CORRECTION BELONGS HERE LIKE EVERYTHING ELSE. The page existed, the tree node existed and the
    # correction chain on this very page already reported STFT and LTFT — but the switchboard listed no
    # row, so the one fuel feature you could see working was the one you could not switch on without
    # knowing which child node to open. Long-term trim is a sub-row because it is gated by closed loop
    # (its whole group carries enableCondition "[#lambda.enabled] == 1"), exactly like the transient
    # sub-options above.
    ('O2 Control',         'lambda.enabled',                     f'{CFG}/Fuel Tuning/O2 Control'),
    ('   Long Term Fuel Trim', 'lambda.ltft_enabled',            f'{CFG}/Fuel Tuning/Long Term Fuel Trim'),

]


def page_fuel_top():
    p = A.Page(title='Fuel')
    p.head('What the fuel side is made of, and what it is currently doing.')

    sw = p.panel(10, A.TOP, 330, A.panel_h(len(FUEL_ROWS), 25, top=12, bottom=8), 'Fuel Functions')
    y = 12
    for label, flag, link in FUEL_ROWS:
        y = p.switch(sw, 10, y, label, flag, link=link,
                     enable=BLEND_ONLY if label == 'Alpha-N VE Table' else '')

    model_y = A.TOP + A.panel_h(len(FUEL_ROWS), 25, top=12, bottom=8) + 10
    model = p.panel(10, model_y, 330, A.CANVAS_H - model_y - 14, 'Air Model')
    y = p.field(model, 10, 10, 'Air Model', 'fuel_calculator.fuel_model', 'enum', 240)
    p.field(model, 10, y, 'Overall Fuel Trim', 'fuel_calculator.overall_corr_pct', 'configedit', 110, '%')

    live = p.panel(350, A.TOP, 920, 560, 'Live')
    cols = [('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'), ('MAP', 'map', '%.0f'),
            ('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f'), ('VE', 've', '%.1f'),
            # NOT "Duty": base_pw is the pulse width BEFORE the injector characterisation, in
            # microseconds. It was labelled Duty and read as one — 3000 of them.
            ('Inj PW  (ms)', 'inj_pw', '%.2f'), ('Base PW  (us)', 'base_pw', '%.0f')]
    for i, (lbl, ch, fmt) in enumerate(cols):
        p.readout(live, 14 + (i % 4) * 225, 14 + (i // 4) * 74, lbl, ch, fmt, w=210)

    # The correction chain, in the order the firmware multiplies it — the answer to "why is it rich here".
    p.add(p._new('label', 14, 168, 500, 20,
                 {'labelText': 'Fuel correction chain  (x1.00 = doing nothing)', 'align': 'Left',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=live)
    chain = [('Warmup', 'fuel_corr_warmup'), ('Post-Start', 'fuel_corr_poststart'), ('Cranking', 'fuel_corr_cranking'),
             ('Air Temp', 'fuel_corr_iat'), ('MAP', 'fuel_corr_map'), ('Baro', 'fuel_corr_baro'),
             ('Fuel Comp', 'fuel_corr_fuelcomp'), ('Accel', 'fuel_corr_accel'), ('Gear', 'fuel_corr_gear'),
             ('STFT', 'fuel_corr_stft'), ('LTFT', 'fuel_corr_ltft'), ('Rev Limit', 'fuel_corr_revlimit'),
             ('EGT', 'fuel_corr_egt'), ('Protection', 'fuel_corr_protection'), ('Overall', 'fuel_corr_overall')]
    for i, (lbl, ch) in enumerate(chain):
        p.readout(live, 14 + (i % 5) * 180, 194 + (i // 5) * 74, lbl, ch, '%.3f', w=170)
    p.graph(14, 420, 890, 108, '[$lambda_1]', into=live)
    return p


# ---- Ignition -----------------------------------------------------------------------------------
IG = 'ignition.'

IGN_ROWS = [
    ('Advance Table',      '',                            f'{CFG}/Ignition Tuning/Advance Table'),
    ('Cranking Advance',   IG + 'cranking_ign_enable',    f'{CFG}/Ignition Tuning/Cranking Advance'),
    ('Dwell Time',         '',                            f'{CFG}/Ignition Tuning/Dwell Time'),
    ('Advance Limits',     '',                            f'{CFG}/Ignition Tuning/Advance Limits'),
    ('Fixed Timing',       IG + 'fixed_timing_enable',    f'{CFG}/Ignition Tuning/Advance Limits'),
    ('Corrections',        '',                            f'{CFG}/Ignition Tuning/Corrections/Coolant'),
    ('Cylinder Trims',     '',                            f'{CFG}/Ignition Tuning/Cylinder Trims/Cylinder 1'),
    ('Knock Detection',    'knock.enabled',               f'{CFG}/Ignition Tuning/Knock Control'),
    ('Pre-Ignition',       'knock.preign_enabled',        f'{CFG}/Ignition Tuning/Knock Control'),
    ('Trailing Split',     '',                            f'{CFG}/Ignition Tuning/Trailing Split'),
]


def page_ign_top():
    p = A.Page(title='Ignition')
    p.head('Spark: the advance map, what corrects it, and what pulls it back.')

    # SIZED BY THE BOARD, not by a number typed once. A tenth row (cranking advance) would have run off
    # a panel fixed at 260, silently — the checker measures boxes, and the box was the wrong size.
    sw_h = A.panel_h(len(IGN_ROWS), 25, top=12, bottom=8)   # the widget's own formula, title bar included
    sw = p.panel(10, A.TOP, 330, sw_h, 'Ignition Functions')
    y = 12
    for label, flag, link in IGN_ROWS:
        y = p.switch(sw, 10, y, label, flag, link=link)

    base = p.panel(10, A.TOP + sw_h + 10, 330, A.CANVAS_H - (A.TOP + sw_h + 10) - 14, 'Basics')
    y = p.field(base, 10, 10, 'Overall Trim', IG + 'overall_adv_trim', 'configedit', 110, 'deg')
    y = p.field(base, 10, y, 'Max Advance', IG + 'max_adv_deg', 'configedit', 110, 'deg')
    p.field(base, 10, y, 'Min Advance', IG + 'min_adv_deg', 'configedit', 110, 'deg')

    live = p.panel(350, A.TOP, 920, 560, 'Live')
    cols = [('Engine RPM', 'rpm', '%.0f'), ('Advance', 'advance', '%.1f'), ('Dwell', 'dwell', '%.0f'),
            ('Fuel Load', 'fuel_load', '%.0f'), ('Knock Level', 'knock_level', '%.1f'),
            ('Knock Retard', 'knock_retard', '%.1f'), ('Coolant', 'clt', '%.0f'), ('Air Temp', 'iat', '%.0f')]
    for i, (lbl, ch, fmt) in enumerate(cols):
        p.readout(live, 14 + (i % 4) * 225, 14 + (i // 4) * 74, lbl, ch, fmt, w=210)
    p.add(p._new('label', 14, 168, 520, 20,
                 {'labelText': 'What is moving the timing right now  (degrees, + advances)', 'align': 'Left',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=live)
    chain = [('Knock', 'knock_retard'), ('Transient', 'ign_corr_transient'), ('Idle', 'idle_ign_corr'),
             ('Trim', 'ign_advance_trim'), ('Protection', 'prot_ign_retard'), ('Total Retard', 'spark_retard_total')]
    for i, (lbl, ch) in enumerate(chain):
        p.readout(live, 14 + (i % 3) * 300, 194 + (i // 3) * 74, lbl, ch, '%.2f', w=280)
    p.graph(14, 350, 890, 178, '[$advance]', into=live)
    return p


def page_ign_advance():
    # THE SAME SHAPE AS EVERY OTHER TABLE PAGE — table left, live readouts in the side panel. This had
    # its own inlined copy of a top strip whose label and value overlapped each other by two pixels,
    # which is the visible half of the problem; the invisible half was that it did not look like any
    # other tuning page. The retards are what belong beside this table: what was commanded, and
    # everything that took some of it away.
    return _table_page('Advance Table', IG + 'ign_table', None, [
        ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'), ('MAP', 'map', '%.0f'),
        ('Advance', 'advance', '%.1f'), ('Knock Level', 'knock_level', '%.1f'),
        ('Knock Retard', 'knock_retard', '%.1f'), ('Total Retard', 'spark_retard_total', '%.1f'),
        ('Coolant', 'clt', '%.0f')])


def page_ign_dwell():
    """The dwell table gets the page. A coil charges to a CURRENT and the current follows the voltage
    across it, so this curve is the setting — the live battery and dwell sit above it, because what you
    are checking while you edit is whether the number the ECU is using matches the coil's datasheet at
    the voltage it is seeing right now."""
    p = A.Page(title='Dwell Time')
    # Full width, like every other table page: the canvas already carries the live view permanently
    # outside the viewport, so a panel repeating it here would cost a quarter of the grid for nothing.
    # The curve stays because dwell IS a curve and its shape is most of reading it; the four numbers
    # that belong to THIS page sit under both.
    top = p.head('Longer at low volts: a coil reaches its rated current in a time that depends on the '
                 'voltage driving it.')
    strip_y = A.CANVAS_H - 66
    tbl_h = int((strip_y - 8 - top) * 0.58)
    p.table(10, top, 1260, tbl_h, IG + 'dwell_table')
    p.curve(10, top + tbl_h + 8, 1260, strip_y - 16 - (top + tbl_h), IG + 'dwell_table')
    cell = 1260 // 4
    for i, (lbl, ch, fmt) in enumerate((('Battery', 'battery', '%.2f'), ('Dwell', 'dwell', '%.0f'),
                                        ('Engine RPM', 'rpm', '%.0f'), ('Advance', 'advance', '%.1f'))):
        p.readout(None, 10 + i * cell, strip_y, lbl, ch, fmt, w=cell - 8)
    return p


def page_ign_limits():
    p = A.Page(title='Advance Limits')
    p.head('The ceiling and floor on commanded advance, and the fixed-timing override used to '
           'set the crank sensor against a timing light.')

    lim = p.panel(10, A.TOP, 330, 260, 'Advance Limits')
    y = p.field(lim, 10, 10, 'Max Advance', IG + 'max_adv_deg', 'configedit', 110, 'deg')
    y = p.field(lim, 10, y, 'Min Advance', IG + 'min_adv_deg', 'configedit', 110, 'deg')
    p.field(lim, 10, y, 'Overall Trim', IG + 'overall_adv_trim', 'configedit', 110, 'deg')

    fix = p.panel(350, A.TOP, 330, 200, 'Fixed Timing  (timing-light mode)')
    y = p.check(fix, 10, 16, 'Fixed timing enabled', IG + 'fixed_timing_enable')
    p.field(fix, 10, y + 6, 'Fixed Advance', IG + 'fixed_timing_deg', 'configedit', 110, 'deg',
            enable='[#ignition.fixed_timing_enable] == 1')

    # WHAT THE TIMING IS LOOKED UP ON is the table's own axis channel, not a setting here. This panel held
    # ignition.map_src / clt_src, which nothing in the firmware read: two controls that looked like the
    # load source and changed nothing. The load axis defaults to Fuel Load, so it follows the Air Model.
    src = p.panel(690, A.TOP, 240, 200, 'Load Axis')
    p.wrapped(10, 10, 215,
              'The Advance Table is looked up on RPM and Fuel Load, which follows the Air Model on Fuel '
              'Setup. To use another channel, right-click the table and choose Table Axis Setup.',
              into=src)

    live = p.panel(10, A.TOP + 270, 1260, 290, 'Live')
    for i, (lbl, ch, fmt) in enumerate((('Engine RPM', 'rpm', '%.0f'), ('Advance', 'advance', '%.1f'),
                                        ('Dwell', 'dwell', '%.0f'), ('Battery', 'battery', '%.2f'),
                                        ('Knock Retard', 'knock_retard', '%.1f'),
                                        ('Total Retard', 'spark_retard_total', '%.1f'))):
        p.readout(live, 14 + i * 205, 16, lbl, ch, fmt, w=190)
    p.graph(14, 100, 1230, 158, '[$advance]', into=live)
    return p


# (name, table, extra readouts, ENABLE, live contribution channel) — the enable is what makes a
# correction opt-in rather than "tuned flat", and the channel is what it is contributing right now.
IGN_CORRECTIONS = [
    ('Coolant',        IG + 'clt_ign_corr_table',       [('Coolant', 'clt', '%.0f')],
     IG + 'enable_clt',       'ign_corr_clt'),
    ('Air Temp',       IG + 'iat_ign_corr_table',       [('Air Temp', 'iat', '%.0f')],
     IG + 'enable_iat',       'ign_corr_iat'),
    ('Fuel Comp',      IG + 'fuelcomp_ign_corr_table',  [('Ethanol', 'ethanol', '%.0f')],
     IG + 'enable_fuelcomp',  'ign_corr_fuelcomp'),
    ('Rev Limiter',    IG + 'rpmlimit_ign_corr_table',  [('RPM Before Cut', 'rpm_to_limit', '%.0f')],
     IG + 'enable_revlimit',  'ign_corr_revlimit'),
    ('Gear',           IG + 'gear_ign_corr_table',      [('Engine RPM', 'rpm', '%.0f')],
     IG + 'enable_gear',      'ign_corr_gear'),
    ('Post-Start',     IG + 'post_start_ign_corr_table',[('Post-Start Corr', 'fuel_corr_poststart', '%.3f')],
     IG + 'enable_poststart', 'ign_corr_poststart'),
    ('Generic 1',      IG + 'igngen1_ign_corr_table',   [], IG + 'enable_generic1', 'ign_corr_generic1'),
    ('Generic 2',      IG + 'igngen2_ign_corr_table',   [], IG + 'enable_generic2', 'ign_corr_generic2'),
    ('Generic 3',      IG + 'igngen3_ign_corr_table',   [], IG + 'enable_generic3', 'ign_corr_generic3'),
    ('Generic 4',      IG + 'igngen4_ign_corr_table',   [], IG + 'enable_generic4', 'ign_corr_generic4'),
]


def page_ign_correction(entry):
    title, table, extra, enable, chan = entry
    p = A.Page(title=title)
    on = f'[#{enable}] == 1'
    p.table(10, A.BAND, 1000, A.CANVAS_H - A.BAND - 8, table, enable=on)
    panel = p.panel(1020, A.BAND, 250, A.CANVAS_H - A.BAND - 8, title)
    # The switch first, and the table greys with it: off means NOT EVALUATED, which is a different
    # statement from a table of zeros and the page should not blur the two.
    g = p.group()
    p.add(p._new('checkbox', 12, 14, A.CHECK_H, A.CHECK_H, {'signalName': enable}, g), into=panel)
    p.add(p._new('label', 12 + A.CHECK_H + A.CHECK_GAP, 11, 190, A.LBL_H,
                 {'labelText': 'Correction enabled', 'align': 'Left', 'fontName': A.FONT_LBL}, g),
          into=panel)
    y = 46
    for lbl, ch, fmt in ([('This Correction', chan, '%.1f'), ('Advance', 'advance', '%.1f'),
                          ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')] + list(extra)):
        p.readout(panel, 12, y, lbl, ch, fmt, w=210)
        y += 62
    p.note(12, y + 8, 'Degrees added to the advance table. 0 = no change. Off, it is not read at all.',
           w=210, into=panel)
    return p


def page_ign_cyl_trim(n):
    p = A.Page(title=f'Cylinder {n}')
    p.table(10, A.BAND, 1000, A.CANVAS_H - A.BAND - 8, f'{IG}cyl{n}_ign_corr_table')
    panel = p.panel(1020, A.BAND, 250, A.CANVAS_H - A.BAND - 8, f'Cylinder {n}')
    y = 14
    for lbl, ch, fmt in (('Advance', 'advance', '%.1f'), ('Knock Retard', 'knock_retard', '%.1f'),
                         (f'Knock Cyl {n}', 'knock_level', '%.1f'), ('Engine RPM', 'rpm', '%.0f')):
        p.readout(panel, 12, y, lbl, ch, fmt, w=210)
        y += 62
    p.note(12, y + 8, 'Per-cylinder timing trim, in degrees. Sequential ignition only.', w=210, into=panel)
    return p


def page_ign_trailing():
    p = A.Page(title='Trailing Split')
    p.head('Rotary engines: how far the trailing plug fires after the leading one.')
    p.table(10, A.TOP, 1000, A.CANVAS_H - A.TOP - 8, IG + 'trail_split_table')
    panel = p.panel(1020, A.TOP, 250, A.CANVAS_H - A.TOP - 8, 'Live')
    y = 14
    for lbl, ch, fmt in (('Advance', 'advance', '%.1f'), ('Engine RPM', 'rpm', '%.0f'),
                         ('Fuel Load', 'fuel_load', '%.0f')):
        p.readout(panel, 12, y, lbl, ch, fmt, w=210)
        y += 62
    return p
# ---- category pages: the same switchboard, one level down ----------------------------------------
# Each category page lists what IT contains, so the tree and the page agree: Engine Functions is the
# things the engine does while running, Protection is the limits, Vehicle Functions is about the car.
CATEGORY_PAGES = [
    ('Engine Functions', f'{CFG}/Engine Functions', [
        ('Air Path', [
            ('Electronic Throttle', 'electronic_throttle.etb[0].enabled', f'{CFG}/Engine Functions/Electronic throttle'),
            # The FUNCTION's own switch, not one of its inputs. This row wrote sensors.sensor[app_1]
            # .enabled — so the switchboard turned a SENSOR on and off while the pedal page's switch
            # turned the MODULE on and off, and the two disagreed about what "Accelerator Pedal" meant.
            ('Accelerator Pedal',   'app.enabled',                        f'{CFG}/Engine Functions/Accelerator Pedal'),
            ('Idle Control',        'idle.enabled',                       f'{CFG}/Engine Functions/Idle Control'),
            ('Idle Stepper',        'stepper.enabled',                    f'{CFG}/Engine Functions/Idle Stepper'),
            ('Boost Control',       'boost.enabled',                      f'{CFG}/Engine Functions/Boost Control'),
        ]),
        ('Valves', [
            ('Cam Control',         'vvt_control.enabled',                f'{CFG}/Engine Functions/Cam Control'),
            ('Variable Valve Lift', 'vvl.enabled',                        f'{CFG}/Engine Functions/Variable Valve Lift'),
        ]),
        ('Fuelling Behaviour', [
            ('Deceleration Fuel Cut', 'dfco.enabled',                     f'{CFG}/Engine Functions/Deceleration Fuel Cut'),
            ('Torque Model',        'torque_model.enabled',               f'{CFG}/Engine Functions/Torque Model'),
        ]),
        ('Power Adders', [
            ('Anti-Lag',            'anti_lag.enabled',                   f'{CFG}/Engine Functions/Anti-Lag'),
            ('Nitrous',             'nitrous.enabled',                    f'{CFG}/Engine Functions/Nitrous'),
            ('Water & Methanol',    'wmi.enabled',                        f'{CFG}/Engine Functions/Water & Methanol'),
        ]),
    ], 'What the engine does while it runs. Tick what this engine has.'),

    ('Protection', f'{CFG}/Protection', [
        ('Limits', [
            ('Rev Limiter',         'rev_limiter.enabled',                f'{CFG}/Protection/Rev Limiter'),
            ('Engine Protection',   '',                                   f'{CFG}/Protection/Engine Protection'),
        ]),
        ('Mixture & Heat', [
            ('Lambda Protection',   'lambda_protect.enabled',             f'{CFG}/Protection/Lambda Protection'),
            ('EGT Protection',      'egt_protect.enabled',                f'{CFG}/Protection/EGT Protection'),
        ]),
    ], 'The limits. These are what stand between a mistake and a hole in a piston.'),

    ('Vehicle Functions', f'{CFG}/Vehicle Functions', [
        ('Launch & Shift', [
            ('Launch Control',      'launch.enabled',                     f'{CFG}/Vehicle Functions/Launch Control'),
            ('Flat Shift',          'flat_shift.enabled',                 f'{CFG}/Vehicle Functions/Flat Shift'),
            ('Gear Detection',      'gear_detect.enabled',                f'{CFG}/Vehicle Functions/Gear Detection'),
        ]),
        ('Road Speed', [
            ('Vehicle Speed',       'vehicle_speed.enabled',              f'{CFG}/Vehicle Functions/Vehicle Speed'),
        ]),
        ('Driving', [
            ('Traction Control',    'traction_control.enabled',           f'{CFG}/Vehicle Functions/Traction Control'),
            ('Cruise Control',      'cruise_control.enabled',             f'{CFG}/Vehicle Functions/Cruise Control'),
            ('Pit Speed Limiter',   'pit_limiter.enabled',                f'{CFG}/Vehicle Functions/Pit Speed Limiter'),
        ]),
    ], 'Things about the car rather than the engine.'),

    ('Electrical', f'{CFG}/Electrical', [
        ('Charging & Outputs', [
            ('Alternator Control',  'alternator.enabled',                 f'{CFG}/Electrical/Alternator Control'),
            ('Outputs',             '',                                   f'{CFG}/Electrical/Outputs'),
        ]),
    ], 'What the ECU drives, besides the injectors and coils.'),
]


def page_index(title, rows, blurb):
    """A branch node's own page: what is under it, as links.

    A node that groups other nodes had nothing behind it — click Corrections and the viewport stayed on
    whatever was there before, which reads as a broken link rather than as "this one is only a heading".
    A list of its children is the least a branch can say, and it is genuinely useful: the tree shows one
    level at a time, this shows the whole branch at once.
    """
    p = A.Page(title=title)
    y = p.head(blurb)
    per = 18                                   # a column of eighteen before it wraps
    for c in range((len(rows) + per - 1) // per):
        chunk = rows[c * per:(c + 1) * per]
        panel = p.panel(10 + c * 330, y, 320, A.panel_h(len(chunk), 25, top=12, bottom=4), title)
        ry = 12
        for label, link in chunk:
            ry = p.switch(panel, 10, ry, label, '', link=link)
    return p


def page_category(title, groups, blurb):
    """A switchboard: one panel per group, flowed rather than placed.

    The panels used to be walked into columns by hand — down until y passed 520, then across 330 — which
    gives the same two columns whatever the window is: two on a laptop that has room for three, and two
    on a 4K screen with half of it empty. They are a FLOW (A.flow, layoutChildRect mode 7): authored size
    kept, wrapped when the next one will not fit, so the row count is a result of the width. Narrow means
    more rows, not smaller panels — and when the rows run past the bottom the surface scrolls.
    """
    p = A.Page(title=title)
    y0 = p.head(blurb)
    box = p.flow(10, y0, A.CANVAS_W - 20, A.CANVAS_H - y0 - 8, gap=10)
    p.anchor(box, x='both', y='both')
    for gt, rows in groups:
        h = A.panel_h(len(rows), 25, top=12, bottom=4)
        # x/y are what the flow hands out; the SIZE is what it keeps. The gutter belongs to the flow
        # (layoutGap) — carrying it inside the child made every panel 10px wider than the panel, so the
        # titled boxes met edge to edge and a row read as one box with lines drawn through it.
        panel = p.panel(0, 0, 320, h, gt, into=box)
        ry = 12
        for label, flag, link in rows:
            ry = p.switch(panel, 10, ry, label, flag, link=link)
    return p


# ---- where the number came from ------------------------------------------------------------------
# A tuner watching a value move needs the TERM that moved it, not the total. Both engines publish
# every term of their arithmetic as its own channel, so these two pages are ledgers of the firmware's
# own sum: base at the top, each correction under it by name, what pulls it back, and the commanded
# figure at the bottom. Nothing here is computed by the studio — a row that disagrees with the result
# is the firmware disagreeing with itself, which is exactly what a page like this is for.

def _ledger(p, panel, x, y, w, rows, fmt='%.2f'):
    """Label left, value right, one row per term — the shape you read down, not across."""
    for lbl, ch in rows:
        g = p.group()
        # THE VALUE CELL IS AS WIDE AS THE VALUE. 88 px was the width of a number, and a state channel
        # reads in words — "Advance Map" is 101 px at this font and came out "Advance Mac". The label
        # gives up what the reading needs, since a caption has somewhere to go and a clipped word does
        # not say which table is in use.
        # +20, not +10: a readout insets its text 6px from each edge of its own box (ValueWidget's
        # insetRect) before it draws, so a cell the width of the words shrinks them to fit.
        vw = max(88, A.Page.chan_value_px(ch, 17) + 20)
        p.add(p._new('label', x, y, w - vw - 8, 20,
                     {'labelText': lbl, 'align': 'Left', 'fontName': A.FONT_SMALL}, g), into=panel)
        p.add(p._new('value', x + w - vw - 4, y - 2, vw, 22,
                     {'signalName': f'[${ch}]', 'format': fmt, 'fontName': '|17|0|0',
                      'align': 'Right'}, g), into=panel)
        y += 24
    return y


def page_ign_breakdown():
    """The commanded advance, decomposed. Every row is a published channel, in the order Ignition sums
    them: base map, the fast corrections, the nine slow ones IgnitionTrim computes, then every retard."""
    p = A.Page(title='Timing Breakdown')
    y0 = p.head('Read it down: the base map, everything that added to it, everything that pulled it '
                'back, and what was finally commanded. Every row is the firmware\'s own figure.')

    # WHICH base, then what it said. A conditional base table replaces the map rather than correcting it,
    # so naming the map here unconditionally would be wrong for the whole time the engine was cranking.
    # Three possible bases now: the map, the cranking table, and the launch map — all three ABSOLUTE,
    # which is why naming the map here unconditionally would be wrong.
    base = p.panel(10, y0, 400, 98, 'Base')
    _ledger(p, base, 12, 16, 376, [('Base Table In Use', 'ign_base_kind')], '%.0f')
    _ledger(p, base, 12, 40, 376, [('Its Figure', 'ign_base_adv')], '%.1f')

    ADDED = [
        ('Coolant',            'ign_corr_clt'),
        ('Air Temp',           'ign_corr_iat'),
        ('Fuel Composition',   'ign_corr_fuelcomp'),
        ('Gear',               'ign_corr_gear'),
        ('Post-Start',         'ign_corr_poststart'),
        ('Generic 1',          'ign_corr_generic1'),
        ('Generic 2',          'ign_corr_generic2'),
        ('Generic 3',          'ign_corr_generic3'),
        ('Generic 4',          'ign_corr_generic4'),
        ('Rev Limiter',        'ign_corr_revlimit'),
        ('Transient Throttle', 'ign_corr_transient'),
        ('Idle Stabiliser',    'idle_ign_corr'),
    ]
    # SIZED, NOT GUESSED. The panel was 356 tall for content that needed more, so the summed row fell off
    # the bottom of it — the one row that ties the twelve above to the total. Every height here follows
    # from the rows and the measured wrap of the note between them.
    NOTE = ('The first nine are the slow set, summed as Advance Trim below. The overall trim is a fixed '
            'scalar rather than a channel, so it does not appear as a row.')
    nh = A.Page.wrapped_h(NOTE, 376)
    corr_h = A.PANEL_TITLE + 4 + 16 + len(ADDED) * 24 + 10 + nh + 10 + 24 + 12
    add = p.panel(10, y0 + 108, 400, corr_h, 'Corrections  (added)')
    y = _ledger(p, add, 12, 16, 376, ADDED, '%.1f')
    p.wrapped(12, y + 6, 376,
              NOTE,
              into=add)
    _ledger(p, add, 12, y + 16 + nh, 376, [('Advance Trim  (those nine, summed)', 'ign_advance_trim')], '%.1f')

    cut = p.panel(420, y0, 400, 200, 'Retards  (subtracted)')
    _ledger(p, cut, 12, 16, 376, [
        ('Knock',        'knock_retard'),
        ('Protection',   'prot_ign_retard'),
        ('Traction',     'traction_retard'),
        ('Nitrous',      'nitrous_retard'),
        ('Anti-Lag',     'antilag_retard'),
        ('Total Retard', 'spark_retard_total'),
    ], '%.1f')

    CLAMP = ('If this sits exactly on the Max or Min advance limit, the clamp is what is tuning the '
             'engine — not the rows to the left of it.')
    ch = A.Page.wrapped_h(CLAMP, 360)
    res = p.panel(420, y0 + 210, 400, A.PANEL_TITLE + 4 + 82 + ch, 'Commanded')
    p.readout(res, 20, 16, 'Advance', '[$advance]', '%.1f', 170)
    p.readout(res, 205, 16, 'Engine RPM', '[$rpm]', '%.0f', 170)
    p.wrapped(20, 82, 360,
              CLAMP,
              into=res)

    ZERO = 'A correction reading zero is a table of zeros, not a broken input.'
    zh = A.Page.wrapped_h(ZERO, 410)
    ctx = p.panel(830, y0, 440, _grid_h(4, zh), 'What They Are Reading')
    cols = [('Coolant', 'clt', '%.0f'), ('Air Temp', 'iat', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'),
            ('MAP', 'map', '%.0f'), ('Ethanol', 'ethanol', '%.0f'), ('Gear', 'gear', '%.0f'),
            ('Knock Level', 'knock_level', '%.1f'), ('Run Time', 'run_time', '%.0f')]
    for i, (lbl, ch2, fmt) in enumerate(cols):
        p.readout(ctx, 16 + (i % 2) * 215, 16 + (i // 2) * 74, lbl, f'[${ch2}]', fmt, w=200)
    p.wrapped(16, 16 + 4 * 74, 410,
              ZERO,
              into=ctx)

    # UNDER THE PANEL, MEASURED FROM IT. This used to start at a constant (y0 + 304 + ch) that was right
    # when the Commanded panel was 14 px shorter; the panel grew and the graph stayed, so it ran up
    # through the readouts above it. A panel knows where it ends.
    gy = res['y'] + res['h'] + 10
    p.graph(420, gy, 400, A.CANVAS_H - gy - 14, 'advance')
    return p


# A LEDGER PANEL'S HEIGHT, counting the title bar. These were written as "16 + rows*24 + 12", which is
# the padding and the rows and nothing for the bar the panel draws over them — so every ledger clipped
# its last row by 10px, on both breakdown pages, invisibly. A panel gives its children
# h - titleBar - 4, and this is that arithmetic run the other way.
def _ledger_h(rows, row_h=24, top=16, bottom=12):
    return A.PANEL_TITLE + 4 + top + rows * row_h + bottom


# The same for a readout grid: rows of 74, plus a wrapped note under them.
def _grid_h(rows, note_h, row_h=74, top=16, bottom=14):
    return A.PANEL_TITLE + 4 + top + rows * row_h + note_h + bottom


def page_fuel_breakdown():
    """The injector pulse, decomposed — the fuel twin of the timing ledger. The corrections are
    MULTIPLIERS here (1.000 = no change), which is why they read to three places."""
    p = A.Page(title='Fuel Breakdown')
    y0 = p.head('Read it down: the charge the air model worked out, the corrections that scale it '
                '(1.000 changes nothing), then the pulse that was commanded.')

    base = p.panel(10, y0, 400, _ledger_h(5), 'The Charge')
    _ledger(p, base, 12, 16, 376, [
        ('Volumetric Efficiency  (%)', 've'),
        ('Charge Air Mass  (mg)',      'air_mass'),
        ('Target Lambda',              'lambda_target'),
        ('Fuel Load',                  'fuel_load'),
        ('Charge Temperature  (C)',    'charge_temp'),
    ], '%.2f')

    MULT = [
        ('Warmup',        'fuel_corr_warmup'),
        ('Air Temp',      'fuel_corr_iat'),
        ('Barometric',    'fuel_corr_baro'),
        ('Fuel Comp',     'fuel_corr_fuelcomp'),
        ('Gear',          'fuel_corr_gear'),
        ('MAP',           'fuel_corr_map'),
        ('Cranking',      'fuel_corr_cranking'),
        ('Post-Start',    'fuel_corr_poststart'),
        ('Accel Enrich',  'fuel_corr_accel'),
        ('Rev Limit',     'fuel_corr_revlimit'),
        ('Generic 1',     'fuel_corr_generic1'),
        ('Generic 2',     'fuel_corr_generic2'),
        ('Generic 3',     'fuel_corr_generic3'),
        ('Generic 4',     'fuel_corr_generic4'),
        ('Overall',       'fuel_corr_overall'),
        ('Protection',    'fuel_corr_protection'),
        ('EGT Protect',   'fuel_corr_egt'),
    ]
    corr = p.panel(10, y0 + _ledger_h(5) + 6, 400, _ledger_h(len(MULT)), 'Corrections  (multiply)')
    _ledger(p, corr, 12, 16, 376, MULT, '%.3f')

    trim = p.panel(420, y0, 400, _ledger_h(3), 'Closed Loop  (multiply)')
    _ledger(p, trim, 12, 16, 376, [
        ('Short Term  (STFT)', 'fuel_corr_stft'),
        ('Long Term  (LTFT)',  'fuel_corr_ltft'),
        ('Lambda',             'lambda_1'),
    ], '%.3f')

    adds = p.panel(420, y0 + _ledger_h(3) + 6, 400, _ledger_h(3), 'Then, In Time')
    _ledger(p, adds, 12, 16, 376, [
        ('Dead Time  (us)',     'pw_add_deadtime'),
        ('Injector dP  (kPa)',  'inj_press_diff'),
        ('Prime Pulse  (us)',   'prime_pw'),
    ], '%.1f')

    PW = ('Injector PW (ms) and Base PW (µs) are the same pulse, dead time included, '
          'before any bank or cylinder trim.')
    ph = A.Page.wrapped_h(PW, 360)
    res_y = y0 + 2 * (_ledger_h(3) + 6)
    res = p.panel(420, res_y, 400, A.PANEL_TITLE + 4 + 82 + ph, 'Commanded')
    p.readout(res, 20, 16, 'Injector PW', '[$inj_pw]', '%.2f', 170)
    p.readout(res, 205, 16, 'Base PW', '[$base_pw]', '%.2f', 170)
    p.wrapped(20, 82, 360,
              PW,
              into=res)

    ONE = 'A correction at 1.000 is doing nothing — an empty table, not a fault.'
    oh = A.Page.wrapped_h(ONE, 410)
    ctx = p.panel(830, y0, 440, _grid_h(4, oh), 'What They Are Reading')
    cols = [('Coolant', 'clt', '%.0f'), ('Air Temp', 'iat', '%.0f'), ('MAP', 'map', '%.0f'),
            ('Baro', 'baro_kpa', '%.0f'), ('Ethanol', 'ethanol', '%.0f'), ('Throttle', 'tps', '%.0f'),
            ('Engine RPM', 'rpm', '%.0f'), ('Run Time', 'run_time', '%.0f')]
    for i, (lbl, ch2, fmt) in enumerate(cols):
        p.readout(ctx, 16 + (i % 2) * 215, 16 + (i // 2) * 74, lbl, f'[${ch2}]', fmt, w=200)
    p.wrapped(16, 16 + 4 * 74, 410,
              ONE,
              into=ctx)

    gy = res_y + 82 + ph + 24
    p.graph(420, gy, 850, A.CANVAS_H - gy - 14, 'inj_pw')
    return p


def page_ign_corrections_branch():
    """The ignition corrections switchboard — the twin of the fuel one.

    It matters more here than a branch index ever did, because the tree now lists only the corrections
    that are ON: this is where a correction that was switched off is switched back on, so it has to show
    every one of them whatever their state, with what each is contributing right now beside it.
    """
    p = A.Page(title='Corrections')
    y0 = p.head('Everything that moves the advance away from the base table, one table each. They ADD '
                'to it, so a correction left at zero does nothing — and one switched OFF is not read at '
                'all, which is not the same thing. The tree lists the ones that are on.')

    grid = p.panel(10, y0, 620, A.panel_h(len(IGN_CORRECTIONS), 30, top=34, bottom=8), 'In the Sum')
    for cx, cw, text in ((10, 40, 'On'), (56, 300, 'Correction'), (380, 120, 'Now'), (510, 100, 'Channel')):
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
    p.add(p._new('panel', 10, 28, 590, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    y = 34
    for name, _table, _extra, enable, chan in IGN_CORRECTIONS:
        g = p.group()
        p.add(p._new('checkbox', 12, y + 4, A.CHECK_H, A.CHECK_H, {'signalName': enable}, g), into=grid)
        p.add(p._new('label', 56, y + 6, 300, A.LBL_H,
                     {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL,
                      'link': f'{CFG}/Ignition Tuning/Corrections/{name}'}, g), into=grid)
        # Zero is "no effect" here, where the fuel side's neutral is 1.000 — same idea, so a row doing
        # nothing recedes and the ones actually moving the timing stand out.
        p.add(p._new('value', 380, y + 4, 110, A.CTL_H,
                     {'signalName': f'[${chan}]', 'format': '%.1f', 'fontName': A.FONT_LBL,
                      'align': 'Right', 'showUnit': '0', 'borderWidth': '1', 'borderRadius': '3',
                      'ranges': f'#2a2a2e,#8a8f98,,,,{chan} == 0,'}, g), into=grid)
        p.add(p._new('label', 510, y + 6, 100, A.LBL_H,
                     {'labelText': chan.replace('ign_corr_', ''), 'align': 'Left',
                      'fontName': A.FONT_SMALL, 'fgColor': C_DIM}, g), into=grid)
        y += 30

    tot = p.panel(640, y0, 300, A.panel_h(3, 30, top=12, bottom=8), 'What They Add Up To')
    _ledger(p, tot, 12, 16, 276, [('Advance Trim  (the slow nine)', 'ign_advance_trim'),
                                  ('Base Advance', 'ign_base_adv'),
                                  ('Commanded Advance', 'advance')], '%.1f')
    p.note(640, y0 + A.panel_h(3, 30, top=12, bottom=8) + 8,
           'The rev limiter correction is computed per engine cycle rather than with the slow nine, so '
           'it is not part of Advance Trim — it is added straight onto the advance. Timing Breakdown '
           'shows the whole chain in order.', w=300)
    return p
