"""The Engine Configuration branch: what this engine IS, and what the ECU makes of it.

These are the first pages of a new installation and the last ones anybody looks at afterwards, which is
the shape they are built to. Nothing here is optional — an engine has a cylinder count whether you like
it or not — so no page carries a module switch; what they carry instead is the reason each number exists,
because every one of them is read by something further down the tree and a wrong one is not a wrong
number, it is an engine that will not start.

Five pages under one branch, split the way the questions are asked:

  Engine Configuration   what this is, where to go, and whether the ECU agrees right now
    Vehicle Identity     whose car it is (hand-authored elsewhere; the branch links to it)
    Cylinders & Firing   the shape of the engine and the order it fires in
    Ignition System      how the spark is made
    Fuel System          how the fuel is delivered — the stages, not the tune
    Trigger System       where the crank reference comes from
      Diagnostics        whether that reference is any good, live
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from author import C_DIM, C_GREEN, C_AMBER, C_RED, C_BLUE

CFG  = 'Configuration'
ENG  = f'{CFG}/Engine Configuration'
TRIG = f'{ENG}/Trigger System'

# The firing-order presets that were on the branch page: eighteen real engines, each setting the whole
# order AND the cylinder count in one write. Kept verbatim — this is a table of facts about engines, and
# retyping facts is how they stop being facts.
PRESETS = [
    ('I2 360 twin (1-2)',                       [1, 2]),
    ('I3 (1-2-3)',                              [1, 2, 3]),
    ('I4 1-3-4-2 (Honda, Toyota, most)',        [1, 3, 4, 2]),
    ('I4 1-2-4-3 (Ford, Mazda)',                [1, 2, 4, 3]),
    ('I4 1-3-2-4 (Subaru boxer, VW)',           [1, 3, 2, 4]),
    ('I5 1-2-4-5-3 (Audi, Volvo)',              [1, 2, 4, 5, 3]),
    ('I6 1-5-3-6-2-4 (BMW, 2JZ, RB)',           [1, 5, 3, 6, 2, 4]),
    ('I6 1-4-2-6-3-5 (reverse rotation)',       [1, 4, 2, 6, 3, 5]),
    ('V6 1-2-3-4-5-6 (GM 60, Nissan VQ)',       [1, 2, 3, 4, 5, 6]),
    ('V6 1-4-2-5-3-6 (Ford Cologne, Honda J)',  [1, 4, 2, 5, 3, 6]),
    ('V8 1-8-4-3-6-5-7-2 (Chevy SBC/BBC, Mopar)', [1, 8, 4, 3, 6, 5, 7, 2]),
    ('V8 1-8-7-2-6-5-4-3 (GM LS)',              [1, 8, 7, 2, 6, 5, 4, 3]),
    ('V8 1-3-7-2-6-5-4-8 (Ford Windsor, Coyote)', [1, 3, 7, 2, 6, 5, 4, 8]),
    ('V8 1-5-4-2-6-3-7-8 (Ford FE)',            [1, 5, 4, 2, 6, 3, 7, 8]),
    ('V10 1-10-9-4-3-6-5-8-7-2 (Dodge Viper)',  [1, 10, 9, 4, 3, 6, 5, 8, 7, 2]),
    ('V10 1-6-5-10-2-7-3-8-4-9 (BMW S85, Audi, Ford)', [1, 6, 5, 10, 2, 7, 3, 8, 4, 9]),
    ('V12 1-7-5-11-3-9-6-12-2-8-4-10 (Jaguar, BMW)', [1, 7, 5, 11, 3, 9, 6, 12, 2, 8, 4, 10]),
]
ORDINAL = ['1st', '2nd', '3rd', '4th', '5th', '6th', '7th', '8th', '9th', '10th', '11th', '12th']

# THE ECU MARKS ITS OWN HOMEWORK. A firing order has to name each cylinder exactly once, and the firmware
# says so on `firing_order_fault` rather than the studio re-deriving the rule. The cells that make up the
# order paint red while it holds, which is the difference between "you typed something wrong" and "you
# typed something wrong SEVEN ROWS AGO": a compact rule of bg,fg,accent,border,blink,when.
ORDER_FAULT = '#ff453a,,,,0,[$firing_order_fault]'

# …and the order is not editable with the engine turning. Re-timing the scheduler under a running engine
# is not an edit, it is a misfire — so the whole table is live only while stopped, or offline where there
# is no engine to upset.
#
# (The page this replaces wrote the engine-state half as [#engine_state] — the CONFIG sigil — for a
# TELEMETRY channel, so it resolved to nothing and the gate never held. Hence [$…] here.)
ORDER_EDITABLE = '![%connected] || [$engine_state] == 0'


def presets_prop():
    """The selector's `presets` string: "name|path=value,path=value" per line, one line per engine.

    Every preset writes ALL TWELVE slots, not just the ones it uses: going from a V8 to a triple has to
    clear the tail, or the scheduler keeps firing cylinders the engine no longer has.
    """
    lines = []
    for name, order in PRESETS:
        writes = [f'engine.firing_order[{i}].cyl={order[i] if i < len(order) else 0}' for i in range(12)]
        writes.append(f'engine.cylinder_count={len(order)}')
        lines.append(name + '|' + ','.join(writes))
    return '\n'.join(lines)


# ---- the branch page ---------------------------------------------------------------------------

def page_top():
    """What this engine is, where each part of it is set, and whether the ECU agrees right now."""
    p = A.Page(title='Engine Configuration')
    y0 = p.head('What this engine IS. The scheduler, the fuel model and every per-cylinder table read '
                'these numbers — a wrong one here is not a wrong number, it is an engine that will not '
                'start.')

    eng = p.panel(10, y0, 400, A.panel_h(6, A.ROW, top=12, bottom=6), 'The Engine')
    y = 12
    y = p.field(eng, 10, y, 'Cylinders', 'engine.cylinder_count', 'configedit', 80, lbl_w=150)
    y = p.field(eng, 10, y, 'Displacement', 'engine.displacement', 'configedit', 100, 'cc', lbl_w=150)
    y = p.field(eng, 10, y, 'Bore', 'engine.bore_mm', 'configedit', 100, 'mm', lbl_w=150)
    y = p.field(eng, 10, y, 'Engine Cycle', 'engine.cycle_type', 'enum', 190, lbl_w=150)
    y = p.field(eng, 10, y, 'Odd-Fire Engine', 'engine.odd_fire', 'checkbox', lbl_w=150)
    p.field(eng, 10, y, 'Cranking Threshold', 'engine.cranking_rpm', 'configedit', 120, 'RPM', lbl_w=150)

    # THE BRANCH, AS A LIST. The tree shows one level at a time; this shows the whole branch at once, in
    # the order an installation works through it.
    sec = p.panel(420, y0, 420, A.panel_h(6, 28, top=12, bottom=6), 'Sections')
    y = 12
    for label, node in (('Vehicle Identity', f'{ENG}/Vehicle Identity'),
                        ('Cylinders & Firing', f'{ENG}/Cylinders & Firing'),
                        ('Trigger System', TRIG),
                        ('Ignition System', f'{ENG}/Ignition System'),
                        ('Fuel System', f'{ENG}/Fuel System'),
                        ('Trigger Diagnostics  (live)', f'{TRIG}/Diagnostics')):
        y = p.switch(sec, 10, y, label, '', link=node, w=360, pitch=28)

    ident = p.panel(850, y0, 420, A.panel_h(4, A.ROW, top=12, bottom=6), 'This Car')
    y = 12
    for label, field in (('Name', 'Name'), ('Make', 'Make'), ('Model', 'Model'), ('Engine', 'Engine')):
        y = p.field(ident, 10, y, label, f'vehicle.{field}', 'text', 240, lbl_w=90)

    # WHAT THE ECU MAKES OF IT. The settings above are a claim about the engine; these are what the ECU
    # is actually seeing. Reading them together is how a wrong claim shows itself — a crank that syncs
    # and an engine that will not start are two different problems, and this row tells them apart.
    # Under the TALLER of the two panels above it (The Engine grew a row for Bore).
    y1 = y0 + max(A.panel_h(6, 28, top=12, bottom=6), A.panel_h(6, A.ROW, top=12, bottom=6)) + 10
    live = p.panel(10, y1, 1260, 166, 'Right Now')
    p.readout(live, 20, 14, 'Engine RPM', '[$rpm]', '%.0f', 130)
    p.readout(live, 160, 14, 'Crank Angle', '[$crank_angle]', '%.1f', 130)
    p.readout(live, 300, 14, 'Run Time', '[$run_time]', '%.0f', 130)
    p.readout(live, 440, 14, 'Trigger Errors', '[$trigger_error_pct]', '%.0f', 130)
    lamps = (('STOPPED', '[$engine_state] == 0', C_DIM),
             ('CRANKING', '[$engine_state] == 1', C_AMBER),
             ('RUNNING', '[$engine_state] == 2', C_GREEN),
             ('CRANK SYNC', '[$sync_level] >= 1', C_BLUE),
             ('PHASE SYNC', '[$sync_level] == 2', C_GREEN),
             ('TRIG ERROR', '[$trigger_error_pct] > 0', C_RED))
    for i, (title, expr, colour) in enumerate(lamps):
        p.add(p._new('indicator', 600 + i * 108, 14, 100, 44,
                     {'signalName': expr, 'onTitle': title, 'offTitle': title, 'onBg': colour,
                      'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    p.wrapped(600, 64, 640,
              'Cranking but never running is a fuel or spark problem; no crank sync at '
                               'all is a trigger problem, and belongs on the Trigger pages.',
              into=live)
    p.wrapped(20, 74, 560,
              'Crank angle counts up through the whole cycle — 0-720° on a four-stroke, '
                               '0-360° on a two-stroke — so at idle it sweeps the full range several '
                               'times a second. Trigger errors should read 0: anything else is teeth '
                               'being missed or invented.',
              into=live)

    # THE ORDER TO DO IT IN. A new installation is a sequence, and the tree does not say what it is.
    y2 = y1 + 170
    steps = p.panel(10, y2, 1260, A.CANVAS_H - y2 - 8, 'A New Installation, In Order')
    items = [('1', 'Vehicle Identity', 'Name the car, so a saved tune says what it belongs to.',
              f'{ENG}/Vehicle Identity'),
             ('2', 'Cylinders & Firing', 'How many, how big, which cycle, what order.',
              f'{ENG}/Cylinders & Firing'),
             ('3', 'Trigger System', 'Where TDC is, and which teeth say so.', TRIG),
             ('4', 'Ignition & Fuel System', 'How the spark and the injectors are wired and staged.',
              f'{ENG}/Ignition System'),
             ('5', 'Sensors', 'Tick what the car has; calibrate each one.', f'{CFG}/Sensors')]
    for i, (num, name, why, node) in enumerate(items):
        x = 12 + i * 250
        g = p.group()
        p.add(p._new('label', x, 10, 24, A.LBL_H,
                     {'labelText': num, 'align': 'Left', 'fontName': '|18|1|0', 'fgColor': C_BLUE}, g),
              into=steps)
        p.add(p._new('label', x + 26, 12, 210, A.LBL_H,
                     {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL, 'link': node}, g),
              into=steps)
        p.add(p._new('label', x + 26, 38, 214, 44,
                     {'labelText': why, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM,
                      'wrap': '1'}, g), into=steps)
    return p


# ---- cylinders & firing ------------------------------------------------------------------------

def page_cylinders(cyl_count=12):
    """The shape of the engine, and the order it fires in — as ONE table.

    The order lived on the branch page and the per-cylinder bank/TDC on this one, which is the same table
    split across two pages: the order is what makes a cylinder the "3rd to fire", and its bank and TDC are
    what the scheduler does about it. Reading one without the other tells you nothing.
    """
    p = A.Page(title='Cylinders & Firing')
    y0 = p.head('How many cylinders, how big, which cycle — and the order they fire in, with the bank and '
                'TDC angle of each. Everything per-cylinder downstream is indexed by this.')

    eng = p.panel(10, y0, 380, A.panel_h(6, A.ROW, top=12, bottom=6), 'Engine')
    y = 12
    y = p.field(eng, 10, y, 'Cylinders', 'engine.cylinder_count', 'configedit', 80, lbl_w=150)
    y = p.field(eng, 10, y, 'Displacement', 'engine.displacement', 'configedit', 100, 'cc', lbl_w=150)
    y = p.field(eng, 10, y, 'Bore', 'engine.bore_mm', 'configedit', 100, 'mm', lbl_w=150)
    y = p.field(eng, 10, y, 'Engine Cycle', 'engine.cycle_type', 'enum', 190, lbl_w=150)
    y = p.field(eng, 10, y, 'Odd-Fire Engine', 'engine.odd_fire', 'checkbox', lbl_w=150)
    p.field(eng, 10, y, 'Cranking Threshold', 'engine.cranking_rpm', 'configedit', 120, 'RPM', lbl_w=150)

    ph = A.panel_h(6, A.ROW, top=12, bottom=6)
    pre = p.panel(10, y0 + ph + 10, 380, 162, 'Known Engines')
    p.add(p._new('settingselector', 10, 12, 340, A.CTL_H, {'presets': presets_prop()}), into=pre)
    p.wrapped(10, 52, 350,
              'Picks a whole firing order AND the cylinder count in one write. Every '
                               'preset clears the slots it does not use — going from a V8 to a triple '
                               'has to, or the scheduler keeps firing cylinders that are not there.',
              into=pre)

    # THE TABLE. One row per firing position: which cylinder fires there, which bank it is on, and where
    # its TDC falls. Bank and TDC are addressed THROUGH the order — engine.cyl[@firing_order[i].cyl - 1] —
    # so a row stays about the same physical cylinder when the order is changed.
    fire = p.panel(400, y0, 520, A.panel_h(13, A.ROW, top=34, bottom=8), 'Firing Order',
                   enable=ORDER_EDITABLE)
    for cx, cw, text in ((10, 90, 'Fires'), (104, 80, 'Cylinder'), (194, 70, 'Bank'), (272, 110, 'TDC Angle')):
        p.add(p._new('label', cx, 8, cw, 16,
                     {'labelText': text, 'align': 'Left', 'fontName': '|16|1|0', 'fgColor': C_DIM}),
              into=fire)
    p.add(p._new('panel', 10, 28, 480, 1, {'bgColor': C_DIM, 'padding': '0'}), into=fire)
    # Sized for the words at the page's font, not for the words at the font they were written in — the
    # caption clipped to "red = inv" the moment the body text went up two points.
    p.add(p._new('label', 350, 6, 160, 20,
                 {'labelText': 'red = invalid', 'align': 'Right', 'fontName': '|14|0|0',
                  'fgColor': C_RED}), into=fire)
    ry = 34
    for i in range(cyl_count):
        # HIDDEN, not greyed: a row for a cylinder the engine does not have is not a disabled setting,
        # it is not part of this engine. A four-cylinder shows four rows.
        on = f'[#engine.cylinder_count] > {i}'
        at = f'engine.cyl[@engine.firing_order[{i}].cyl - 1]'
        g = p.group()
        p.add(p._new('label', 10, ry + 4, 90, A.LBL_H,
                     {'labelText': f'{ORDINAL[i]} to fire', 'align': 'Left', 'fontName': A.FONT_LBL,
                      'condition': on}, g), into=fire)
        p.add(p._new('configedit', 104, ry, 80, A.CTL_H,
                     {'signalName': f'engine.firing_order[{i}].cyl', 'condition': on,
                      'ranges': ORDER_FAULT}, g), into=fire)
        p.add(p._new('configedit', 194, ry, 70, A.CTL_H,
                     {'signalName': f'{at}.bank', 'condition': on}, g), into=fire)
        p.add(p._new('configedit', 272, ry, 110, A.CTL_H,
                     {'signalName': f'{at}.tdc_angle', 'condition': on}, g), into=fire)
        ry += A.ROW

    live = p.panel(10, y0 + ph + 182, 380, 216, 'Right Now')
    p.readout(live, 15, 14, 'Engine RPM', '[$rpm]', '%.0f', 110)
    p.readout(live, 130, 14, 'Crank Angle', '[$crank_angle]', '%.1f', 120)
    p.add(p._new('indicator', 255, 14, 105, 44,
                 {'signalName': '[$sync_level] >= 1', 'onTitle': 'SYNC', 'offTitle': 'NO SYNC',
                  'onBg': C_GREEN, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_SMALL}), into=live)
    p.add(p._new('indicator', 255, 62, 105, 40,
                 {'signalName': '[$firing_order_fault]', 'onTitle': 'ORDER BAD', 'offTitle': 'ORDER OK',
                  'onBg': C_RED, 'onFg': '#000000', 'offBg': '#1e3a24', 'offFg': '#8a8f98',
                  'fontName': A.FONT_SMALL}), into=live)
    # UNDER the lamps, at the panel's full width. Squeezed into the 230px the lamps left beside them it
    # wrapped to six lines in a box with room for three, so it lost its first and last rows to the clip.
    p.wrapped(15, 108, 350,
              'Crank angle is measured from the trigger reference: with no sync there '
                               'is no reference, and the TDC angles are just numbers. ORDER BAD means '
                               'the ECU cannot use the order beside it — a cylinder listed twice, or '
                               'one missing.',
              into=live)

    why = p.panel(930, y0, 340, 332, 'What These Are For')
    p.wrapped(10, 10, 320,
              'FIRING ORDER is the sequence, not the wiring: the 3rd row is whichever '
                               'cylinder fires third, and the ignition and injection outputs are bound to '
                               'cylinder NUMBER, so changing the order re-times the engine without '
                               'rewiring it.\n\n'
                               'BANK groups cylinders that share an exhaust side — which wideband trims '
                               'them, and which bank a per-bank correction applies to.\n\n'
                               'TDC ANGLE is where that cylinder reaches top dead centre within the '
                               'cycle, measured from the reference the trigger defines. An evenly-fired '
                               'engine is the cycle divided by the cylinder count: 720/6 = 120° apart on '
                               'a six. Odd-fire engines are not, which is what the Odd-Fire switch is '
                               'about.',
              into=why)

    nxt = p.panel(930, y0 + 342, 340, A.panel_h(3, 28, top=12, bottom=6), 'Reads This')
    y = 12
    for label, node in (('Ignition · Cylinder Trims', f'{CFG}/Ignition Tuning/Cylinder Trims'),
                        ('Fuel · Corrections', f'{CFG}/Fuel Tuning/Corrections'),
                        ('Trigger System', TRIG)):
        y = p.switch(nxt, 10, y, label, '', link=node, w=296, pitch=28)
    return p


# ---- ignition system ---------------------------------------------------------------------------

def page_ignition():
    """How the spark is MADE — the hardware half of ignition. What it is worth is Ignition Tuning."""
    p = A.Page(title='Ignition System')
    y0 = p.head('How the spark is made: how many coils and how they are driven, the ceiling and floor on '
                'commanded advance, and the fixed-timing override used to set the crank sensor.')

    # THE GATEWAY BELONGS WITH THE HARDWARE IT GATES, not on a diagnostics page: "do the coils fire at
    # all" is the first question about spark hardware, and the answer is a setting rather than a state.
    h_hw = A.panel_h(3, A.ROW, top=12, bottom=6) + 24
    hw = p.panel(10, y0, 440, h_hw, 'Spark Hardware')  # h_hw + the note below it position Right Now
    y = 12
    y = p.field(hw, 10, y, 'Ignition Mode', 'engine.ign_mode', 'enum', 230, lbl_w=150)
    y = p.field(hw, 10, y, 'Cylinders', 'engine.cylinder_count', 'configedit', 80, lbl_w=150)
    y = p.field(hw, 10, y, 'Ignition Outputs', 'engine.ign_enable', 'enum', 110, lbl_w=150)
    # SHOWN ONLY WHEN IT IS OFF, because that is the only time it needs saying — and it needs saying:
    # this survives a reset, so an engine that cranks and will not start can be explained by a checkbox
    # somebody set last week.
    p.wrapped(10, y, 420,
              'OFF \u2014 no coil will fire, and this survives a reset.',
              into=hw, colour='#d08770', cond='[#engine.ign_enable] == 0')
    HW_NOTE = ('Single coil fires through a distributor; wasted spark fires two cylinders a revolution '
               'apart from one coil; coil-on-plug is one output per cylinder. Changing either lays the '
               'coil outputs out again (IGN1 = cylinder 1 and so on); each IGN pin\'s own page under '
               'Electrical/Outputs shows its cylinder and its polarity, and is where one is changed by '
               'hand.')
    p.note(10, y0 + h_hw + 8, HW_NOTE, w=430)

    lim = p.panel(460, y0, 440, A.panel_h(3, A.ROW, top=12, bottom=6), 'Advance Limits')
    y = 12
    y = p.field(lim, 10, y, 'Max Advance', 'ignition.max_adv_deg', 'configedit', 90, 'deg', lbl_w=150)
    y = p.field(lim, 10, y, 'Min Advance', 'ignition.min_adv_deg', 'configedit', 90, 'deg', lbl_w=150)
    p.field(lim, 10, y, 'Overall Trim', 'ignition.overall_adv_trim', 'configedit', 90, 'deg', lbl_w=150)
    p.note(460, y0 + A.panel_h(3, A.ROW, top=12, bottom=6) + 8,
           'The limits clamp whatever the tables and corrections add up to, so a bad correction cannot '
           'run the engine into detonation. Positive is BTDC — advance. The trim moves every commanded '
           'angle at once and is meant for a quick global change, not for tuning.', w=430)

    fix = p.panel(910, y0, 360, A.panel_h(3, A.ROW, top=12, bottom=6), 'Fixed Timing')
    y = p.check(fix, 10, 16, 'Fixed timing enabled', 'ignition.fixed_timing_enable')
    p.field(fix, 10, y + 6, 'Fixed Advance', 'ignition.fixed_timing_deg', 'configedit', 90, 'deg',
            enable='[#ignition.fixed_timing_enable] == 1', lbl_w=130)
    p.note(910, y0 + A.panel_h(3, A.ROW, top=12, bottom=6) + 8,
           'Holds ONE advance whatever the map says, so a timing light can check that the crank sensor '
           'and the ECU agree about TDC. If the light reads what this box says, the trigger offset is '
           'right. It is a setup tool — leave it off to drive.', w=350)

    # MEASURED, not a hand-added 110. That constant stood for "the Spark Hardware panel, then the note
    # under it" — two things it did not name, so adding a row to the panel drove the note straight
    # through this one.
    y1 = y0 + h_hw + 8 + A.Page.note_h(HW_NOTE, 430) + 12
    live = p.panel(10, y1, 890, 130, 'Right Now')
    p.readout(live, 20, 14, 'Engine RPM', '[$rpm]', '%.0f', 130)
    p.readout(live, 160, 14, 'Advance', '[$advance]', '%.1f', 130)
    p.readout(live, 300, 14, 'Dwell', '[$dwell]', '%.0f', 130)
    p.readout(live, 440, 14, 'Battery', '[$battery]', '%.2f', 130)
    p.wrapped(590, 20, 290,
              'Commanded advance against the limits above: if it sits exactly on Max, '
                               'the limit is what is tuning the engine, not the table.',
              into=live)

    nxt = p.panel(910, y1, 360, A.panel_h(3, 28, top=12, bottom=6), 'Then Tune It')
    y = 12
    for label, node in (('Advance Table', f'{CFG}/Ignition Tuning/Advance Table'),
                        ('Dwell Time', f'{CFG}/Ignition Tuning/Dwell Time'),
                        ('Ignition Tuning', f'{CFG}/Ignition Tuning')):
        y = p.switch(nxt, 10, y, label, '', link=node, w=300, pitch=28)
    return p


# ---- fuel system -------------------------------------------------------------------------------

def page_fuel_system(stages=4):
    """The injection HARDWARE: how many stages, how each is driven, how many outputs it has.

    The page this replaces had all four stage rows bound to stage 1 — four rows that looked independent
    and edited one element — so a second stage could be configured all afternoon without anything
    changing. Each row addresses its own element now, and the row is only live when the stage count says
    that stage exists.
    """
    p = A.Page(title='Fuel System')
    y0 = p.head('How fuel is DELIVERED: how many injection stages this engine has, how each one is driven '
                'and how many outputs it uses. What each stage squirts is Fuel Tuning.')

    # FOUR ROWS NOW, and the warning line under them. Sized from one constant so the note below the
    # panel follows it — the note's y was a second copy of the panel height, and they parted company the
    # moment a row was added.
    h_sys = A.panel_h(4, A.ROW, top=12, bottom=6) + 24
    sysp = p.panel(10, y0, 430, h_sys, 'Injection System')
    y = 12
    y = p.field(sysp, 10, y, 'Injection Stages', 'engine.num_inj_stages', 'configedit', 80, lbl_w=190)
    y = p.field(sysp, 10, y, 'Injector Timing', 'engine.injector_timing_method', 'enum', 190, lbl_w=190)
    # (The page this replaces also carried "Cylinder/Bank Correction Stages", bound to
    # fuel_calculator.cyl_bank_correction_stages — a field this firmware's schema no longer has, so the
    # box edited nothing. It is dropped rather than re-drawn.)
    y = p.field(sysp, 10, y, 'Cylinders', 'engine.cylinder_count', 'configedit', 80, lbl_w=190)
    # The same gateway as the Ignition System page, for the other half of "it cranks and will not start".
    y = p.field(sysp, 10, y, 'Injector Outputs', 'engine.inj_enable', 'enum', 110, lbl_w=190)
    p.wrapped(10, y, 410,
              'OFF \u2014 no injector will open, and this survives a reset.',
              into=sysp, colour='#d08770', cond='[#engine.inj_enable] == 0')
    p.note(10, y0 + h_sys + 8,
           'Injector timing says whether the angle in the tables is the END of the squirt or the START of '
           'it — the same number means two different things, and the fuel lands in the wrong place if it '
           'is set the wrong way. Changing the stages, a stage\'s mode or its outputs lays the injector '
           'outputs out again (LS1 = cylinder 1 and so on); each LS pin\'s own page under '
           'Electrical/Outputs shows its cylinder, stage and polarity.', w=420)

    st = p.panel(450, y0, 560, A.panel_h(stages + 1, A.ROW, top=34, bottom=8), 'Stages')
    for cx, cw, text in ((10, 100, 'Stage'), (114, 90, 'Outputs'), (212, 170, 'Mode'), (390, 120, 'Per Cycle')):
        p.add(p._new('label', cx, 8, cw, 16,
                     {'labelText': text, 'align': 'Left', 'fontName': '|16|1|0', 'fgColor': C_DIM}),
              into=st)
    p.add(p._new('panel', 10, 28, 520, 1, {'bgColor': C_DIM, 'padding': '0'}), into=st)
    ry = 34
    for i in range(stages):
        on = f'[#engine.num_inj_stages] > {i}'
        g = p.group()
        p.add(p._new('label', 10, ry + 4, 100, A.LBL_H,
                     {'labelText': f'Stage {i + 1}', 'align': 'Left', 'fontName': A.FONT_LBL,
                      'link': f'{CFG}/Fuel Tuning/Stage {i + 1}/Setup', 'enableCondition': on}, g), into=st)
        p.add(p._new('configedit', 114, ry, 80, A.CTL_H,
                     {'signalName': f'engine.inj_stage[{i}].num_outputs', 'enableCondition': on}, g), into=st)
        p.add(p._new('combobox', 212, ry, 170, A.CTL_H,
                     {'signalName': f'engine.inj_stage[{i}].mode', 'enableCondition': on}, g), into=st)
        p.add(p._new('configedit', 390, ry, 80, A.CTL_H,
                     {'signalName': f'engine.inj_stage[{i}].injections_per_cycle', 'enableCondition': on},
                     g), into=st)
        ry += A.ROW
    p.note(450, y0 + A.panel_h(stages + 1, A.ROW, top=34, bottom=8) + 8,
           'A stage is a set of injectors that share a tune: sequential fires each on its own cylinder\'s '
           'event, bank groups them by the bank the cylinder is on, batch fires them together. The name '
           'is the way to that stage\'s tuning. A row greys out when the stage count says it does not '
           'exist — the settings are still stored, they are simply not run.', w=550)

    nxt = p.panel(1020, y0, 250, A.panel_h(5, 28, top=12, bottom=6), 'Then Tune It')
    y = 12
    for label, node in (('Fuel Setup', f'{CFG}/Engine Configuration/Fuel System/Fuel Setup'),
                        ('VE Table', f'{CFG}/Fuel Tuning/VE Table'),
                        ('Target Lambda', f'{CFG}/Fuel Tuning/Target Lambda'),
                        ('Prime Pulse', f'{CFG}/Fuel Tuning/Fuel Prime Pulse'),
                        ('Fuel Tuning', f'{CFG}/Fuel Tuning')):
        y = p.switch(nxt, 10, y, label, '', link=node, w=200, pitch=28)

    y1 = y0 + A.panel_h(stages + 1, A.ROW, top=34, bottom=8) + 100
    live = p.panel(10, y1, 1260, A.CANVAS_H - y1 - 8, 'Right Now')
    p.readout(live, 20, 14, 'Injector PW', '[$inj_pw]', '%.2f', 130)
    p.readout(live, 160, 14, 'Fuel Load', '[$fuel_load]', '%.0f', 130)
    p.readout(live, 300, 14, 'Lambda', '[$lambda_1]', '%.2f', 130)
    p.readout(live, 440, 14, 'Target', '[$lambda_target]', '%.2f', 130)
    p.readout(live, 580, 14, 'Engine RPM', '[$rpm]', '%.0f', 130)
    p.wrapped(730, 20, 510,
              'Pulse width against the cycle time is the honest limit on a stage: '
                               'injectors that never close cannot meter fuel, which is what a second '
                               'stage is for.',
              into=live)
    return p


# ---- trigger system ----------------------------------------------------------------------------

def page_trigger():
    """Where the crank reference comes from: the offset, the pickup, the sync band, and the streams."""
    p = A.Page(title='Trigger System')
    y0 = p.head('Where TDC is. Every angle the ECU commands — spark, injection, cam target — is measured '
                'from the reference these settings define, so nothing downstream can be more right than '
                'this page is.')

    ref = p.panel(10, y0, 430, A.panel_h(2, A.ROW, top=12, bottom=6), 'Crank Reference')
    y = 12
    y = p.field(ref, 10, y, 'Trigger Offset BTDC', 'trigger.trigger_offset_btdc', 'configedit', 90, 'deg',
                lbl_w=180)
    p.field(ref, 10, y, 'Pickup Angle', 'trigger.sensor_angle', 'configedit', 90, 'deg', lbl_w=180)
    h1 = A.panel_h(2, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h1 + 8,
           'OFFSET is where the pattern\'s reference edge sits before TDC #1 — set it with a timing light '
           'and Fixed Timing on the Ignition System page. PICKUP is where the sensor is mounted around '
           'the wheel. Positive is BTDC, and the angle axis increases with rotation, so BTDC counts '
           'BACKWARDS from TDC.', w=420)

    band = p.panel(10, y0 + h1 + 96, 430, A.panel_h(2, A.ROW, top=12, bottom=6), 'Phase Sync Band')
    y = 12
    y = p.field(band, 10, y, 'Min RPM for Phase Sync', 'trigger.min_full_sync_rpm_x10', 'configedit', 100,
                'RPM', lbl_w=180)
    p.field(band, 10, y, 'Max RPM for Phase Sync', 'trigger.max_full_sync_rpm_x10', 'configedit', 100,
            'RPM', lbl_w=180)
    p.note(10, y0 + h1 + 96 + A.panel_h(2, A.ROW, top=12, bottom=6) + 8,
           'Phase sync is the CAM telling the ECU which revolution of the cycle it is on. It is only '
           'trusted inside this band: a VR cam sensor can glitch at cranking speed and again at the top '
           'of the range, and a false phase halves the engine\'s fuelling for a cycle.', w=420)

    # THE STREAMS, as a switchboard. Each name is the way into its own page, which is where the thirteen
    # fields of a stream live.
    st = p.panel(450, y0, 380, A.panel_h(6, 28, top=12, bottom=6), 'Streams')
    y = 12
    for i, name in enumerate(STREAM_NAMES):
        y = p.switch(st, 10, y, name, f'trigger.streams[{i}].enabled',
                     link=f'{TRIG}/Trigger Streams/{name}', w=320, pitch=28)
    p.note(450, y0 + A.panel_h(6, 28, top=12, bottom=6) + 8,
           'A stream is one capture input and the pattern it is expected to see. The crank streams give '
           'RPM and position; a cam stream gives the phase, and on a VVT engine its nominal angle is what '
           'cam control measures against.', w=370)

    diag = p.panel(840, y0, 430, 430, 'Trigger Diagram')
    p.add(p._new('triggerdiagram', 10, 10, 400, 380, {}), into=diag)

    y1 = y0 + 440
    live = p.panel(840, y1, 430, A.CANVAS_H - y1 - 8, 'Right Now')
    p.readout(live, 15, 14, 'Sync Level', '[$sync_level]', '%.0f', 100)
    p.readout(live, 120, 14, 'Crank Angle', '[$crank_angle]', '%.1f', 110)
    p.readout(live, 235, 14, 'RPM', '[$rpm]', '%.0f', 100)
    p.add(p._new('label', 15, 70, 400, 20,
                 {'labelText': 'Sync level: 0 none · 1 crank · 2 phase.',
                  'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=live)
    p.add(p._new('label', 15, 92, 200, A.LBL_H,
                 {'labelText': 'Trigger Diagnostics  (live)', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{TRIG}/Diagnostics'}), into=live)

    # ENGINE-SYNCHRONOUS SAMPLING. A subsystem setting rather than a sensor's own, and it belongs to
    # the angle domain: it says over how many degrees the engine-synchronous inputs are averaged. It
    # had no page anywhere, which is how the setting that decides what MAP MEANS went unreachable.
    y_sync = y0 + A.panel_h(6, 28, top=12, bottom=6) + 100
    sync = p.panel(450, y_sync, 380, A.panel_h(1, A.ROW, top=12, bottom=6) + 100, 'Engine-Sync Sampling')
    p.field(sync, 10, 12, 'Averaging Window', 'sensors.engine_sync_window_deg', 'configedit', 100, 'deg',
            lbl_w=180)
    p.wrapped(10, 46, 360,
              'How many degrees of crank the engine-synchronous inputs are averaged '
                               'over. It exists for MAP: sampling manifold pressure at an arbitrary '
                               'moment catches whatever the intake pulse is doing, so the reading '
                               'swings with RPM rather than with load.',
              into=sync)

    y2 = y_sync + A.panel_h(1, A.ROW, top=12, bottom=6) + 110
    tools = p.panel(450, y2, 380, A.panel_h(3, 28, top=12, bottom=6), 'Tools')
    y = 12
    for label, node in (('Trigger Streams', f'{TRIG}/Trigger Streams'),
                        ('Trigger Diagnostics  (live)', f'{TRIG}/Diagnostics'),
                        ('Ignition System  (fixed timing)', f'{ENG}/Ignition System')):
        y = p.switch(tools, 10, y, label, '', link=node, w=320, pitch=28)
    return p


# ------------------------------------------------------------------------------------------------
# THE TRIGGER STREAMS
# ------------------------------------------------------------------------------------------------
# They were six pages built by hand and then copied: Crank Primary sat at its own margins with a
# 390-wide diagram and the other five at different ones; four carried Cam Intake B1's enable label
# because that is the page they were cloned from; two had no title at all. A stream is a stream — the
# same thirteen fields, with only the NAME and the element index changing — so they are generated.
#
# The ORDER is the job, not the struct. Setting a stream up is: say where the wire comes in, say what
# pattern that input carries, then crank the engine and see whether the ECU agrees. So the page reads
# 1 Wiring, 2 Pattern, 3 Proof, left to right, and the pattern half leads with the WHEEL LIBRARY —
# nobody should be typing "36" and "3" and a gap cell index for a wheel the meta already describes.
STREAM_NAMES = ['Crank Primary', 'Crank Secondary', 'Cam Intake B1', 'Cam Exhaust B1',
                'Cam Intake B2', 'Cam Exhaust B2']

# What a cell MEANS depends on the primitive, so the caption over the cells says which — all three are
# written, and the one that applies is the one shown.
CELL_NOTES = [
    (0, 'GAP: each cell is the index of the tooth that ENDS a gap. 36-1 is one cell, 0.'),
    (1, 'SEQUENCE: each cell is one INTER-EVENT ANGLE in 0.1° — the step to the next event.'),
    (2, 'WIDTH reads no cells. The pulse is identified by its LENGTH, in the band below.'),
]


def wheel_presets(rate):
    """The wheel library as a preset list for ONE stream: every wheel that has a stream at this rate.

    `rate` 0 is the crank-rate half of a wheel (Crank Primary/Secondary), 1 the cam-rate half. The same
    wheel therefore appears on a crank page and a cam page, and picking it on each installs that half —
    which is what the library is for. Only the fields the primitive READS are written, so a 36-2 does
    not carry seven meaningless width numbers with it, and the dropdown still recognises the tune it is
    looking at (the selector matches on the pairs an option writes).
    """
    lines = []
    for w in A.Page._meta().get('trigger_wheels', []):
        st = next((x for x in w.get('streams', []) if x.get('rate', 0) == rate), None)
        if st is None:
            continue
        kind = st.get('kind', 'gap')
        pr = {'gap': 0, 'seq': 1, 'width': 2}.get(kind)
        if pr is None:
            continue
        pairs = [f'primitive={pr}']
        if kind == 'gap':
            gaps = st.get('gaps', [])
            pairs += [f'slots={st.get("slots", 0)}', f'gap_ratio={max(1, st.get("ratio", 0) or 1)}',
                      f'cell_len={len(gaps)}']
            pairs += [f'cell[{k}].v={v}' for k, v in enumerate(gaps)]
        elif kind == 'seq':
            cells = st.get('cell', [])
            pairs += [f'cell_len={len(cells)}']
            pairs += [f'cell[{k}].v={v}' for k, v in enumerate(cells)]
        else:
            pairs += [f'width_min={st.get("width_min", 0)}', f'width_max={st.get("width_max", 7200)}',
                      f'width_target={st.get("width_target", 0)}', 'cell_len=0']
        lines.append((w['name'], pairs))
    return lines


def page_stream(i, name):
    """One trigger stream: where it comes in, what pattern it carries, and whether the ECU agrees."""
    e = f'trigger.streams[{i}]'
    is_cam = i >= 2
    p = A.Page(title=name)
    y0 = p.head(f'{name}: one capture input and the pattern it is expected to see. '
                + ('A cam stream gives the PHASE — which revolution of the cycle — and on a VVT engine '
                   'its nominal angle is what cam control measures against.'
                   if is_cam else
                   'The crank streams give RPM and position; every angle the ECU commands is measured '
                   'from them.'),
                enable=f'{e}.enabled')

    # Which fields are READ depends on the primitive, so the ones that are not are greyed rather than
    # hidden — the row keeps its place, and the page does not reshuffle itself as the mode changes.
    GAP, SEQ, WIDTH = f'[#{e}.primitive] == 0', f'[#{e}.primitive] == 1', f'[#{e}.primitive] == 2'
    CELLS = f'[#{e}.primitive] != 2'

    # ---- 1 · Wiring ---------------------------------------------------------------------------
    wire = p.panel(10, y0, 400, A.panel_h(3, A.ROW, top=12, bottom=6), '1 · Wiring')
    y = 12
    y = p.field(wire, 10, y, 'Capture Input', f'{e}.capture_index', 'enum', 150)
    y = p.field(wire, 10, y, 'Capture Edge',  f'{e}.edge',          'enum', 150)
    p.field(wire, 10, y, 'Pattern Repeats / Cycle', f'{e}.repeats', 'configedit', 90)
    h_wire = A.panel_h(3, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h_wire + 6,
           'The input is a pin on the board, by name. REPEATS is how many times this pattern comes '
           'round per engine CYCLE — 0 takes the slot default (2 for a crank stream on a four-stroke, '
           '1 for a cam).', w=390)

    # ---- 2 · Pattern --------------------------------------------------------------------------
    y_pat = y0 + h_wire + 84
    presets = wheel_presets(0 if not is_cam else 1)
    lib_h = 96
    pat_rows = 9
    pat_h = lib_h + A.panel_h(pat_rows, A.ROW, top=0, bottom=6)
    pat = p.panel(10, y_pat, 400, pat_h, '2 · Pattern')
    p.add(p._new('settingselector', 10, 12, 360, A.CTL_H,
                 {'presets': '\n'.join(nm + '|' + ','.join(f'{e}.{q}' for q in pairs)
                                       for nm, pairs in presets)}), into=pat)
    # 380, not 360: at 360 this broke into three lines and ran down over the Primitive row below it. The
    # panel is 400 wide and nothing else is on this row, so the words get the width.
    p.wrapped(10, 50, 380,
              'Pick the WHEEL this engine has and its ' +
                               ('cam' if is_cam else 'crank') + ' half lands here. Pick the same wheel '
                               'on the other stream\'s page for its half.',
              into=pat)
    y = lib_h
    y = p.field(pat, 10, y, 'Primitive',   f'{e}.primitive',  'enum', 150)
    y = p.field(pat, 10, y, 'Base Teeth',  f'{e}.slots',      'configedit', 90, enable=GAP)
    y = p.field(pat, 10, y, 'Gap Ratio (missing + 1)', f'{e}.gap_ratio', 'configedit', 90, enable=GAP)
    y = p.field(pat, 10, y, 'Cell Length', f'{e}.cell_len',   'configedit', 90, enable=CELLS)
    y = p.field(pat, 10, y, 'Match Window', f'{e}.window_pct', 'configedit', 100, '%')
    # THE SAME TOLERANCE AT CRANKING, next to the one it blends into. A starter turns an engine
    # unevenly — it slows into every compression — so the window that is right at 3000 rpm rejects
    # real teeth at 200, and this was the only stream field with nowhere to set it.
    y = p.field(pat, 10, y, 'Match Window (cranking)', f'{e}.window_crank_pct', 'configedit', 100, '%')
    y = p.field(pat, 10, y, 'WIDTH min',    f'{e}.width_min',    'configedit', 100, 'deg', enable=WIDTH)
    y = p.field(pat, 10, y, 'WIDTH max',    f'{e}.width_max',    'configedit', 100, 'deg', enable=WIDTH)
    p.field(pat, 10, y, 'WIDTH target angle', f'{e}.width_target', 'configedit', 100, 'deg', enable=WIDTH)

    # ---- The cells ----------------------------------------------------------------------------
    cells = p.panel(420, y0, 300, 462, 'Pattern Cells', enable=CELLS)
    for n, (cx, first) in enumerate(((10, 0), (150, 16))):
        p.add(p._new('array1d', cx, 46, 130, 388,
                     {'signalName': f'{e}.cell[].v', 'firstIndex': str(first), 'rowCount': '16',
                      'labelText': f'{first}–{first + 15}'}), into=cells)
    for pr, text in CELL_NOTES:
        p.wrapped(10, 10, 270,
                  text,
                  into=cells, cond=f'[#{e}.primitive] == {pr}')
    p.note(420, y0 + 470, 'Cells past Cell Length are ignored, not cleared — shrinking a pattern leaves '
                          'the old values in place.', w=290)

    # ---- 3 · Proof ----------------------------------------------------------------------------
    diag = p.panel(730, y0, 540, 360, '3 · Proof')
    p.add(p._new('triggerdiagram', 10, 10, 510, 316, {}), into=diag)

    y_live = y0 + 370
    live = p.panel(730, y_live, 540, 160, 'Right Now')
    p.readout(live, 15, 14, 'Sync Level', '[$sync_level]', '%.0f', 100)
    p.readout(live, 125, 14, 'RPM', '[$rpm]', '%.0f', 110)
    p.readout(live, 245, 14, 'Crank Angle', '[$crank_angle]', '%.1f', 120)
    p.readout(live, 375, 14, 'Error Rate', '[$trigger_error_pct]', '%.0f', 110)
    # Two rows, and WRAPPED: this is 724px of text and the box is 500, so as one line it was guillotined
    # mid-word ("…the pattern is right when sy") with nothing on screen to say the rest existed. The
    # readouts end at y=64 and the links start at 106, so 30px is exactly two rows of FONT_SMALL.
    p.add(p._new('label', 15, 76, 500, 30,
                 {'labelText': 'Sync level: 0 none · 1 crank · 2 phase. Crank it and watch: the pattern '
                               'is right when sync holds and the error rate stays at 0.',
                  'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM, 'wrap': '1'}), into=live)
    # The way out, in the panel that sends you there: proof first, then wherever the proof points.
    for n, (label, node) in enumerate((('Trigger System  (offset and pickup)', TRIG),
                                       ('Trigger Diagnostics  (live)', f'{TRIG}/Diagnostics'))):
        p.add(p._new('label', 15 + n * 260, 106, 250, A.LBL_H,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_LBL, 'link': node}),
              into=live)

    if is_cam:
        # ONE SUBJECT, ONE PANEL. The nominal angle was here on its own while `phased`, the phaser's
        # authority and the fixed-cam allowance had nowhere to be set at all — and they are the same
        # question: where does this cam sit, and how far is it allowed to be from there. Two columns
        # rather than four rows because there are ~100px left at this y, which is also why the old
        # caption went: the page blurb already says what a nominal angle is for.
        # Flush under Right Now and padded tight: there are ~100px of canvas left at this y and
        # two rows plus a title bar is what fits in them.
        # +8, not +4: the second row's control ended 2px past the content area. The comment above
        # is still true — there is canvas left here, so the panel can have the pixels.
        ph = p.panel(730, y_live + 160, 540, A.PANEL_TITLE + 6 + 2 * A.ROW + 8, 'Cam Phase')
        yy = 8
        p.field(ph, 10, yy, 'Nominal Angle', f'{e}.nominal_angle', 'configedit', 90, 'deg', lbl_w=120)
        p.check(ph, 280, yy, 'Cam Is Phased (VVT)', f'{e}.phased')
        yy += A.ROW
        # Authority bounds a MOVING cam; allowance bounds a fixed one. Exactly one is read, so the
        # other is greyed — the row keeps its place and the panel does not reshuffle.
        # lbl_w 120 and 115, not 150 and 120: these two columns sit 270px apart and a degrees box sizes
        # itself to hold its value AND its unit now, so it is the names that give way. Both still fit.
        p.field(ph, 10, yy, 'Phaser Authority', f'{e}.phase_authority', 'configedit', 90, 'deg',
                lbl_w=120, enable=f'[#{e}.phased] == 1')
        p.field(ph, 280, yy, 'Mech Allowance', f'{e}.phase_allowance', 'configedit', 90, 'deg',
                lbl_w=115, enable=f'[#{e}.phased] == 0')

    return p


def page_streams_branch():
    """The switchboard: all six streams on one screen — on/off, input, primitive — and the way in.

    It was six link labels in a 320-wide box and nothing else, so the one question it should answer at a
    glance ("what is this engine's trigger set up as?") took six visits to six pages.
    """
    p = A.Page(title='Trigger Streams')
    y0 = p.head('Every trigger input this ECU reads. A stream is one capture input and the pattern it is '
                'expected to see; a disabled stream is not bound, not decoded, and holds no pin.')

    grid = p.panel(10, y0, 900, A.panel_h(6, 34, top=34, bottom=8), 'Streams')
    for cx, cw, text in ((10, 40, 'On'), (56, 260, 'Stream'), (326, 200, 'Capture Input'),
                         (536, 180, 'Primitive'), (726, 160, 'Repeats / Cycle')):
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
    p.add(p._new('panel', 10, 28, 870, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    y = 34
    for i, name in enumerate(STREAM_NAMES):
        e = f'trigger.streams[{i}]'
        g = p.group()
        p.add(p._new('checkbox', 12, y + 4, A.CHECK_H, A.CHECK_H, {'signalName': f'{e}.enabled'}, g), into=grid)
        p.add(p._new('label', 56, y + 6, 260, A.LBL_H,
                     {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL,
                      'link': f'{TRIG}/Trigger Streams/{name}'}, g), into=grid)
        p.add(p._new(A.Page.enum_control(f'{e}.capture_index'), 326, y + 4, 190, A.CTL_H,
                     {'signalName': f'{e}.capture_index'}, g), into=grid)
        p.add(p._new(A.Page.enum_control(f'{e}.primitive'), 536, y + 4, 170, A.CTL_H,
                     {'signalName': f'{e}.primitive'}, g), into=grid)
        p.add(p._new('configedit', 726, y + 4, 90, A.CTL_H, {'signalName': f'{e}.repeats'}, g), into=grid)
        y += 34

    note = ('The INDEX is the role: slot 1 is Crank Primary, slot 2 Crank Secondary, and slots 3-6 the '
            'four cams. There is no role field, so two streams cannot claim the same job — an engine '
            'with phasers on both intakes enables Cam Intake B1 and Cam Intake B2 and leaves the '
            'exhaust slots off.\n\n'
            'Each name opens that stream\'s own page, where the wheel library fills the pattern in.')
    nh = A.Page.wrapped_h(note, 324)
    side = p.panel(920, y0, 350, A.panel_h(1, nh + 2 * A.ROW + 12, top=10, bottom=8), 'Roles')
    p.wrapped(10, 10, 324,
              note,
              into=side)
    y = nh + 18
    for label, node in (('Trigger System', TRIG), ('Trigger Diagnostics  (live)', f'{TRIG}/Diagnostics')):
        y = p.switch(side, 10, y, label, '', link=node, w=296, pitch=28)

    live = p.panel(10, y0 + A.panel_h(6, 34, top=34, bottom=8) + 10, 900, 120, 'Right Now')
    p.readout(live, 15, 14, 'Sync Level', '[$sync_level]', '%.0f', 110)
    p.readout(live, 135, 14, 'RPM', '[$rpm]', '%.0f', 110)
    p.readout(live, 255, 14, 'Crank Angle', '[$crank_angle]', '%.1f', 120)
    p.readout(live, 385, 14, 'Error Rate', '[$trigger_error_pct]', '%.0f', 110)
    p.readout(live, 505, 14, 'Last Error Tooth', '[$trigger_last_error_tooth]', '%.0f', 140)
    return p


def page_trigger_diagnostics():
    """Is the reference any good? The live half of the trigger, on its own page.

    Configuration and evidence are two different jobs: the settings are typed once at installation, and
    this is what you watch while cranking a stubborn engine. Splitting them keeps the numbers big enough
    to read from under the bonnet.
    """
    p = A.Page(title='Trigger Diagnostics')
    y0 = p.head('What the trigger is doing right now. Watch this while cranking: a crank that never syncs '
                'and an engine that syncs and will not fire are different problems.')

    st = p.panel(10, y0, 620, 180, 'Sync')
    lamps = (('NO SYNC', '[$sync_level] == 0', C_RED),
             ('CRANK SYNC', '[$sync_level] >= 1', C_BLUE),
             ('PHASE SYNC', '[$sync_level] == 2', C_GREEN))
    for i, (title, expr, colour) in enumerate(lamps):
        p.add(p._new('indicator', 15 + i * 200, 14, 190, 56,
                     {'signalName': expr, 'onTitle': title, 'offTitle': title, 'onBg': colour,
                      'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178'}), into=st)
    p.readout(st, 15, 82, 'Sync Level', '[$sync_level]', '%.0f', 120)
    p.readout(st, 145, 82, 'Crank Angle', '[$crank_angle]', '%.1f', 130)
    p.readout(st, 285, 82, 'Engine RPM', '[$rpm]', '%.0f', 130)
    p.readout(st, 425, 82, 'Engine State', '[$engine_state]', '%.0f', 130)

    err = p.panel(650, y0, 620, 192, 'Errors')
    p.add(p._new('indicator', 15, 14, 190, 56,
                 {'signalName': '[$trigger_error_pct] > 0', 'onTitle': 'TRIGGER ERRORS',
                  'offTitle': 'CLEAN', 'onBg': C_RED, 'onFg': '#000000', 'offBg': '#1e3a24',
                  'offFg': '#8a8f98'}), into=err)
    p.readout(err, 215, 14, 'Error Rate  %', '[$trigger_error_pct]', '%.0f', 130)
    p.readout(err, 355, 14, 'Last Kind', '[$trigger_last_error_kind]', '%.0f', 120)
    p.readout(err, 485, 14, 'Last Tooth', '[$trigger_last_error_tooth]', '%.0f', 120)
    p.wrapped(15, 84, 590,
              'Error rate is the share of teeth in the last pattern revolution that were '
                               'rejected. It should be 0. Anything else is teeth being missed (a gap that '
                               'is not there) or invented (noise read as a tooth) — check the sensor gap, '
                               'the shielding, and that the wheel in Trigger Streams is the wheel on the '
                               'engine.',
              into=err)

    y1 = y0 + 202
    tr = p.panel(10, y1, 1260, A.CANVAS_H - y1 - 8, 'While It Cranks')
    p.traces(10, 10, 1240, 300, [('[$rpm]', 0, 8000), ('[$sync_level]', 0, 2),
                                 ('[$trigger_error_pct]', 0, 100)], into=tr)
    p.wrapped(10, 320, 1240,
              'RPM should come up smoothly on the starter and sync should follow within '
                               'a revolution or two; sync that appears and drops away is a pattern that '
                               'matches sometimes, which is a wheel definition or a noise problem rather '
                               'than a missing signal.',
              into=tr)
    return p
