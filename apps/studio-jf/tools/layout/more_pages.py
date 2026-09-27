"""The branches that had tree nodes and no pages: O2 trim, cam control, knock, torque, and every branch
node that was only a folder.

Two shapes, both already established here:

  BRANCH PAGE   a switchboard of what the branch contains — the same shape as the category pages, so a
                folder in the tree is a page that says what is in it rather than a dead end.
  TABLE PAGE    one table, what it is currently producing, and a note (fuel_tree.table_page).

The table branches are matched by LABEL: the tree's child names came from the schema's table labels, so
"Lambda LTFT Bank 1" in the tree is the table whose label is "Lambda LTFT Bank 1". Nothing is hard-coded
twice, and a table renamed in the schema stops matching loudly rather than quietly pointing at the wrong
one.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import fuel_tree as FT
from author import C_DIM

ROOT = 'Configuration'

CFG = 'Configuration'

# What a branch page SAYS. "What X contains" is a placeholder, and a page whose only words are its own
# title has told the reader nothing they did not already know from clicking it. These say what the branch
# is FOR — which is the sentence someone new to the tune needs and someone experienced skips.
BRANCH_BLURB = {
    f'{CFG}/Fuel Tuning':
        'Everything that decides how much fuel goes in: the air model and the VE table it reads, the '
        'target mixture, the corrections that trim it, and the injectors that deliver it.',
    f'{CFG}/Fuel Tuning/Corrections':
        'Multipliers on the base fuel mass, each for one thing the VE table cannot know — how cold it is, '
        'how long it has been running, what the fuel is, which gear it is in. They multiply, so two at '
        '10% is 21%, not 20%.',
    f'{CFG}/Ignition Tuning/Corrections':
        'Degrees added to or taken from the advance table, each for one condition it cannot see. They '
        'SUM, and the total is what the limits then clamp.',
    f'{CFG}/Ignition Tuning/Cylinder Trims':
        'Per-cylinder timing, for an engine where one cylinder runs hotter or knocks first. Sequential '
        'ignition only — a wasted-spark coil fires two cylinders and cannot tell them apart.',
    f'{CFG}/Electrical/Half Bridges':
        'Two bidirectional drivers — the only ones the ECU has. Each realises whichever bus signal it is '
        'pointed at: a throttle servo, a PWM or BAC valve, a DC motor; or, as a pair, one bipolar '
        'stepper. They share a carrier frequency.',
}
for _i in (1, 2, 3, 4):
    BRANCH_BLURB[f'{CFG}/Fuel Tuning/Stage {_i}'] = (
        f'Injection stage {_i}: the injector data — dead time, flow rate and the short-pulse correction — '
        f'and where in the cycle this stage fires. A stage is a set of injectors driven together; the '
        f'staging duty decides when the next one starts helping.')


# (branch path, module, live rows shown beside every table in it, the note each table page carries)
TABLE_BRANCHES = [
    (f'{CFG}/Fuel Tuning/O2 Control', 'lambda',
     [('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f'),
      ('STFT', 'stft_pct', '%.1f'), ('LTFT', 'ltft_pct', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'How the fast trim behaves at each operating point: the delay says how long ago the gas being read '
     'left the cylinder, and the gains how hard the trim reacts. Raise a gain until the mixture starts '
     'to hunt, then back it off by a third.'),

    (f'{CFG}/Vehicle Functions/Traction Control', 'traction_control',
     [('Slip', 'traction_slip', '%.1f'), ('Slip Error', 'traction_slip_err', '%.1f'),
      ('Throttle Cap', 'traction_cap', '%.0f'), ('Retard', 'traction_retard', '%.1f'),
      ('Cut', 'traction_cut_pct', '%.0f'), ('Vehicle Speed', 'vehicle_spd', '%.0f')],
     'The target says how much slip is WANTED at this speed — more off the line, where slip is how a car '
     'accelerates, and less at speed, where it is how a car leaves the road. The retard and cut curves '
     'are read on how far over that target the slip actually is.'),

    (f'{CFG}/Vehicle Functions/Launch Control', 'launch',
     [('Launch Active', 'launch_active', '%.0f'), ('End RPM', 'launch_end_rpm', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'),
      ('Cut', 'launch_cut_pct', '%.0f'), ('Launch Advance', 'launch_ign_adv', '%.1f')],
     'The End RPM is the limit itself — the RPM at which every cylinder is cut — and its two axes ship '
     'switched off, so it is one number until you say otherwise. The ignition map is the ACTUAL advance '
     'while launch is active, not a correction on the main map: away from the launch cell it should read '
     'like the normal calibration, because the engine runs through it on the way there and back.'),

    (f'{CFG}/Engine Functions/Cam Control', 'vvt_control',
     [('Intake Angle', 'vvt_angle_1', '%.1f'), ('Intake Duty', 'vvt_duty_1', '%.1f'),
      ('Exhaust Angle', 'vvt_angle_2', '%.1f'), ('Exhaust Duty', 'vvt_duty_2', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'Cam position is a closed loop like any other: the target says where the cam should be, the gains '
     'decide how hard it is pushed there, and the base duty is the holding current it starts from.'),

    (f'{CFG}/Engine Functions/Torque Model', 'torque_model',
     [('Engine Torque', 'engine_torque_nm', '%.0f'), ('Advance', 'advance', '%.1f'),
      ('Lambda', 'lambda_1', '%.2f'), ('Lambda Torque', 'lambda_torque_ratio', '%.2f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'An estimate of the torque the engine makes, and what taking timing or mixture away costs it. '
     'It is for logging and the dash: nothing in the firmware acts on it yet.'),

    (f'{CFG}/Ignition Tuning/Knock Control', 'knock',
     [('Knock Level', 'knock_level', '%.1f'), ('Knock Retard', 'knock_retard', '%.1f'),
      ('Knock Count', 'knock_count', '%.0f'), ('Advance', 'advance', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'The threshold is how far over the noise floor counts as knock, and the retard is what it costs. '
     'Both are read at the operating point, because an engine is noisier at 7000 rpm than at idle and a '
     'single number for both is either deaf or hysterical.'),
]


def tables_of(meta, mod):
    """{label: path} for every real table in a module — axes excluded, since an axis is not a page."""
    out = {}
    for name, e in meta['config'].get(mod, {}).items():
        if isinstance(e, dict) and e.get('type') == 'table' and not name.endswith(('_axis', '_yaxis')):
            label = e.get('label') or name
            out[label] = f'{mod}.{name}'
    return out


def table_pages(meta, tree_children):
    """A page for every child node of a table branch whose name matches a table label.

    `tree_children` is {branch path: [child names]}, read from the document — the tree is the list of what
    should exist, so a node that is there gets a page and a table that has no node is left alone.
    """
    pages, missing = {}, []
    for branch, mod, live, note in TABLE_BRANCHES:
        by_label = tables_of(meta, mod)
        for name in tree_children.get(branch, []):
            if name in by_label:
                pages[f'{branch}/{name}'] = FT.table_page(name, by_label[name], live, note)
            else:
                missing.append(f'{branch}/{name}')
    return pages, missing


def branch_page(title, rows, blurb, enable=''):
    """A folder that is a page: what this branch contains, each name a way in.

    The same shape as the category switchboards, because it answers the same question — what is in here —
    and a tree node that opens onto nothing is the one thing a navigation tree must not do.
    """
    p = A.Page(title=title)
    p.head(blurb, enable=enable)
    COL_W, ROW_H, COLS = 300, 25, 4
    per = max(1, -(-len(rows) // COLS))
    x, y, n = 10, A.TOP, 0
    panel = None
    for i, (name, link) in enumerate(rows):
        if i % per == 0:
            h = A.panel_h(min(per, len(rows) - i), ROW_H, top=12, bottom=4)
            panel = p.panel(x, A.TOP, COL_W, h, title if i == 0 else '')
            x += COL_W + 10
            y = 12
        y = p.switch(panel, 10, y, name, '', link=link, w=COL_W - 40)
    return p


# ---- the engine-configuration pages ------------------------------------------------------------
# No module switch on any of these: an engine has a cylinder count whether you like it or not. They are
# the first pages of a new installation, which is why they say what the number is FOR rather than just
# naming it — every one of them is read by something further down the tree.

def page_vehicle_identity():
    p = A.Page(title='Vehicle Identity')
    p.head('Whose car this is. Nothing here changes how the engine runs — it is what tells one saved tune '
           'from another six months later.')
    box = p.panel(10, A.TOP, 620, A.panel_h(6, A.ROW, top=12, bottom=6), 'Vehicle')
    y = 12
    for label, field in (('Name', 'Name'), ('Make', 'Make'), ('Model', 'Model'),
                         ('Engine', 'Engine'), ('VIN', 'VIN'), ('Notes', 'Notes')):
        y = p.field(box, 10, y, label, f'vehicle.{field}', 'text', 340)
    return p


def page_cylinders_firing(cyl_count=12):
    """The engine's shape, and the firing order that follows from it."""
    p = A.Page(title='Cylinders & Firing')
    p.head('What the engine IS: how many cylinders, how big, which cycle, and the order they fire in. '
           'Everything downstream — the scheduler, the injection stages, the per-cylinder trims — is '
           'derived from these.')
    eng = p.panel(10, A.TOP, 460, A.panel_h(6, A.ROW, top=12, bottom=6), 'Engine')
    y = 12
    y = p.field(eng, 10, y, 'Cylinders', 'engine.cylinder_count', 'configedit', 90)
    y = p.field(eng, 10, y, 'Displacement', 'engine.displacement', 'configedit', 110, 'cc')
    y = p.field(eng, 10, y, 'Engine Cycle', 'engine.cycle_type', 'enum', 160)
    y = p.field(eng, 10, y, 'Odd-Fire Engine', 'engine.odd_fire', 'checkbox')
    y = p.field(eng, 10, y, 'Cranking Threshold', 'engine.cranking_rpm', 'configedit', 90, 'RPM')
    p.field(eng, 10, y, 'Injection Stages', 'engine.num_inj_stages', 'configedit', 90)

    out = p.panel(480, A.TOP, 460, A.panel_h(2, A.ROW, top=12, bottom=6), 'Outputs')
    y = 12
    y = p.field(out, 10, y, 'Ignition Mode', 'engine.ign_mode', 'enum', 200)
    p.field(out, 10, y, 'Injector Timing', 'engine.injector_timing_method', 'enum', 200)

    # The per-cylinder grid: firing order and TDC, one row per cylinder, only as many as the engine has.
    fire = p.panel(10, A.TOP + 250, 930, A.CANVAS_H - A.TOP - 258, 'Firing Order')
    p.wrapped(10, 8, 900,
              'One row per cylinder, in FIRING ORDER: which bank it is on and where its '
                               'TDC falls in the cycle. Rows past the cylinder count are ignored.',
              into=fire)
    ry = 48
    for i in range(cyl_count):
        on = f'[#engine.cylinder_count] > {i}'
        g = p.group()
        col = i % 3
        cx = 10 + col * 300
        cy = ry + (i // 3) * A.ROW
        p.add(p._new('label', cx, cy + 4, 70, A.LBL_H,
                     {'labelText': f'Cyl {i + 1}', 'align': 'Left', 'fontName': A.FONT_LBL,
                      'enableCondition': on}, g), into=fire)
        p.add(p._new('configedit', cx + 76, cy, 60, A.CTL_H,
                     {'signalName': f'engine.cyl[{i}].bank', 'enableCondition': on}, g), into=fire)
        p.add(p._new('configedit', cx + 142, cy, 80, A.CTL_H,
                     {'signalName': f'engine.cyl[{i}].tdc_angle', 'enableCondition': on}, g), into=fire)
        p.add(p._new('label', cx + 228, cy + 4, 60, A.LBL_H,
                     {'labelText': 'deg', 'align': 'Left', 'fontName': A.FONT_LBL, 'fgColor': C_DIM,
                      'enableCondition': on}, g), into=fire)
    return p


def page_ignition_system():
    """Where the spark comes from and what shapes it — the wiring-level half of ignition."""
    p = A.Page(title='Ignition System')
    p.head('How the spark is made: the limits on commanded advance, the fixed-timing override used to set '
           'the crank sensor, and the signals the dwell table reads.')
    lim = p.panel(10, A.TOP, 440, A.panel_h(3, A.ROW, top=12, bottom=6), 'Advance Limits')
    y = 12
    y = p.field(lim, 10, y, 'Max Advance', 'ignition.max_adv_deg', 'configedit', 90, 'deg')
    y = p.field(lim, 10, y, 'Min Advance', 'ignition.min_adv_deg', 'configedit', 90, 'deg')
    p.field(lim, 10, y, 'Overall Trim', 'ignition.overall_adv_trim', 'configedit', 90, 'deg')

    fix = p.panel(460, A.TOP, 440, A.panel_h(3, A.ROW, top=12, bottom=6), 'Fixed Timing')
    y = p.check(fix, 10, 16, 'Fixed timing enabled', 'ignition.fixed_timing_enable')
    p.field(fix, 10, y + 4, 'Fixed Advance', 'ignition.fixed_timing_deg', 'configedit', 90, 'deg',
            enable='[#ignition.fixed_timing_enable] == 1')
    p.note(10, A.TOP + 150, 'Fixed timing holds one advance whatever the map says, so a timing light can '
                            'be used to check that the crank sensor and the ECU agree about TDC. It is a '
                            'setup tool: leave it off to drive.', w=890)
    return p


# THE TEMPLATES LIVE IN THE SCHEMA, not here. They were a list in this file, which is the wrong place
# for them twice over: the studio's wizard cannot read a python list, and a template that drifts from
# the page offering it installs something other than what it says. They are `output_templates` in
# definition/ecu.schema.yaml now and arrive in the meta — so the page carries a BUTTON, and the wizard
# reads the personalities from the connected firmware's own definition.


# The per-pin page's first panel title, replaced per page by apply_outputs with the pin and its connector.
PIN_TITLE = 'This Pin'
# The pin's own name, substituted per page by apply_outputs — the pinwire row resolves the connector
# terminal, its shell colour and the wire colour from it.
PIN_RES = '%PIN%'


def page_output_setup(count=42):
    """ONE PHYSICAL OUTPUT in full: what the pin does, and — for a generic output — what drives it.

    outputs.output[i] IS pin i (IGN1-12, LS1-22, HS1-8), so this page is a pin, and it is where that pin's
    job is SET and SEEN: a coil and the cylinder it fires, an injector with its cylinder and stage, or a
    generic output with its conditions, value and shaping. The studio fills the coil and injector rows in
    when the cylinder count, ignition mode or injection stages change; this is where one is changed by
    hand.

    A TEMPLATE, one page per pin, exactly as the sensors are. The body is written with `[*]` bindings and
    the page carries the element (`outputs.output[7]`), so forty-two identical pages cost one body. The
    panel titled PIN_TITLE is renamed per page by apply_outputs to the pin and its connector terminal.
    """
    SEL = 'outputs.output[*]'
    NOTE_KIND = ('PWM or a plain level is the Kind. The carrier is on the Frequency page.')
    # The middle column's two panels are stacked, so the lower one has to know how tall the upper one
    # came out — added up here rather than guessed at each use, which is how they came to overlap.
    # Nine rows, the divider, and the expression row that is taller than a row.
    h_cand = A.panel_h(10, A.ROW, top=12, bottom=6) + 16
    NOTE_CAND = 'Primaries request, limits clamp down, an override replaces the lot.'
    h_note_cand = A.Page.note_h(NOTE_CAND, 420)
    p = A.Page(title='Output')
    # NO SWITCH IN THE HEAD: what this pin does is its Function, the first field on the page.
    y0 = p.head('What this pin does: a coil, an injector, or a generic output. A generic output\'s '
                'conditions, value and shaping are the rest of the page.')

    # WHERE THIS PIN COMES OUT, drawn rather than spelled: the connector as a badge in its own shell
    # colour and the wire as a bar in its insulation colours. It was a sentence in the panel title —
    # "IGN2 - CN2-25, wire W" — which asks the reader to translate CN2 into a connector they can
    # point at and W into a colour, when both are already known and both ARE colours.
    PIN_ROW = 18
    pick = p.panel(10, y0, 400, A.panel_h(2, A.ROW, top=12, bottom=6) + 44 + PIN_ROW, PIN_TITLE)
    y = 12
    p.add(p._new('pinwire', 10, y, 380, 20, {'resource': PIN_RES}), into=pick)
    y += PIN_ROW
    y = p.field(pick, 10, y, 'Function', f'{SEL}.function', 'enum', 190)
    y = p.field(pick, 10, y, 'Name', f'{SEL}.name', 'text', 190,
                enable=f'[#{SEL}.function] == 3')
    # THE WIZARD. Pick what the slot IS, answer the two or three numbers that vary, and it writes the
    # conditions, the timings and the fail direction — then shows exactly what it wrote, on this page,
    # editable. A dialog rather than a dropdown because a personality is not a set of numbers: it is
    # two compiled conditions, four parameters whose meaning changes with the template, and a pin
    # somebody has to wire.
    p.add(p._new('wizard', 10, y + 6, 200, 30,
                 {'signalName': SEL, 'labelText': 'Set up this output\u2026'}), into=pick)
    p.add(p._new('label', 218, y + 10, 172, A.LBL_H,
                 {'labelText': 'fuel pump, fan, tacho\u2026', 'align': 'Left',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=pick)
    h_pick = A.panel_h(2, A.ROW, top=12, bottom=6) + 44 + PIN_ROW

    # A COIL OR AN INJECTOR: the one cylinder it serves. Stage is an injector's, Plug a rotary coil's.
    # Polarity belongs to every function — a coil, an injector and a generic output are all driven at it.
    COIL, INJ = f'[#{SEL}.function] == 1', f'[#{SEL}.function] == 2'
    h_fire = A.panel_h(2, A.ROW, top=12, bottom=6)
    fire = p.panel(10, y0 + h_pick + 8, 400, h_fire, 'Coil or Injector')
    y = 12
    y = p.field(fire, 10, y, 'Cylinder', f'{SEL}.cylinder', 'enum', 150, enable=f'{COIL} or {INJ}')
    g = p.group()
    p.add(p._new('label', 10, y + 6, 60, A.LBL_H,
                 {'labelText': 'Stage', 'align': 'Left', 'fontName': A.FONT_LBL, 'enableCondition': INJ},
                 g), into=fire)
    p.add(p._new(A.Page.enum_control(f'{SEL}.inj_stage'), 70, y + 2, 110, A.CTL_H,
                 {'signalName': f'{SEL}.inj_stage', 'enableCondition': INJ}, g), into=fire)
    ROT = f'{COIL} and [#engine.cycle_type] == 2'
    g = p.group()
    p.add(p._new('label', 200, y + 6, 50, A.LBL_H,
                 {'labelText': 'Plug', 'align': 'Left', 'fontName': A.FONT_LBL, 'enableCondition': ROT},
                 g), into=fire)
    p.add(p._new(A.Page.enum_control(f'{SEL}.ign_plug'), 250, y + 2, 140, A.CTL_H,
                 {'signalName': f'{SEL}.ign_plug', 'enableCondition': ROT}, g), into=fire)

    # EVERYTHING BELOW IS A GENERIC OUTPUT'S, and greys for a coil or an injector.
    on = f'[#{SEL}.function] == 3'
    h_drive = A.panel_h(3, A.ROW, top=12, bottom=6)
    drive = p.panel(10, y0 + h_pick + h_fire + 16, 400, h_drive, 'Driving')
    y = 12
    y = p.field(drive, 10, y, 'Active High', f'{SEL}.active_high', 'checkbox', enable=f'[#{SEL}.function] != 0')
    y = p.field(drive, 10, y, 'Kind', f'{SEL}.kind', 'enum', 170, enable=on)
    p.field(drive, 10, y, 'PWM Frequency', f'{SEL}.pwm_freq_hz', 'configedit', 110, 'Hz',
            enable=f'{on} and [#{SEL}.kind] == 0')
    # The carrier lives on its own page: fixed, table or expression is a question with three answers and
    # a map behind one of them, and the row it would take here does not exist.
    nk = p.wrapped(10, y0 + h_pick + h_fire + h_drive + 20, 390, NOTE_KIND)

    # ---- WHEN it is on ---------------------------------------------------------------------------
    # The two conditions sit in the middle column, under the value they gate. Their TIMINGS go in the
    # left column under the hardware, because that is what they are about — a relay's minimum cycle
    # time, a starter motor's bounded crank. Two different questions, two halves of the page.
    # Placed under the note rather than 52px under the panel: the literal was a guess at the note's
    # height, and the note is two lines at this width.
    time = p.panel(10, nk['y'] + nk['h'] + 8, 400, A.panel_h(5, A.ROW, top=12, bottom=6), 'Timing')
    y = 12
    y = p.field(time, 10, y, 'Minimum On Time', f'{SEL}.min_on_ms', 'configedit', 100, 'ms', enable=on)
    y = p.field(time, 10, y, 'Minimum Off Time', f'{SEL}.min_off_ms', 'configedit', 100, 'ms', enable=on)
    y = p.field(time, 10, y, 'Maximum On Time', f'{SEL}.max_on_ms', 'configedit', 100, 'ms', enable=on)
    y = p.field(time, 10, y, 'Re-Arm Delay', f'{SEL}.rearm_ms', 'configedit', 100, 'ms', enable=on)
    p.field(time, 10, y, 'If Unanswerable', f'{SEL}.on_invalid', 'enum', 150, enable=on)

    # Under the value it gates, and it follows whatever the panel above it actually came out at.
    y_when = y0 + h_cand + 4 + h_note_cand + 8
    h_when = A.panel_h(2, 34, top=12, bottom=6)
    when = p.panel(420, y_when, 430, h_when, 'When  (the condition)')
    for n, (lbl, f) in enumerate((('Turn on when', 'on_expr'), ('Turn off when', 'off_expr'))):
        p.add(p._new('label', 10, 18 + n * 34, 110, A.LBL_H,
                     {'labelText': lbl, 'align': 'Left', 'fontName': A.FONT_LBL}), into=when)
        p.add(p._new('expression', 124, 14 + n * 34, 296, 30,
                     {'signalName': f'{SEL}.{f}', 'enableCondition': on}), into=when)
    # THE NUMBERS THE CONDITIONS READ. A template's conditions reference these rather than baking
    # numbers in, which is what lets the wizard ask for a prime time without the slot ceasing to be
    # recognisable as a fuel pump. What each one MEANS is the template's business — the wizard labels
    # them — but they are shown here because somebody who edits the conditions by hand still needs to
    # set them, and a number a page hides is a number nobody can change.
    pnl = p.panel(420, y_when + h_when + 8, 430, A.panel_h(2, A.ROW, top=12, bottom=6) + 22,
                  'Template Numbers  (what the conditions read)')  # 124 tall
    for n, f in enumerate(('param_a', 'param_b', 'param_c', 'param_d')):
        col, row = n // 2, n % 2
        p.field(pnl, 10 + col * 205, 12 + row * A.ROW, chr(ord('A') + n), f'{SEL}.{f}',
                'configedit', 100, enable=on, lbl_w=60)
    p.add(p._new('label', 10, 12 + 2 * A.ROW + 2, 400, A.LBL_H,
                 {'labelText': 'What each means is on the wizard that set it.', 'align': 'Left',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=pnl)

    # ---- how much ---------------------------------------------------------------------------------
    # THREE SOURCES, one panel. A slot driven by a module arbitrates its candidates; a fan, a PWM pump
    # or a steering pump wants a MAP against channels it picks; a relay wants a number. Each row is
    # greyed unless it is the source in force, so the page shows which of the three is answering.
    TAB = f'[#{SEL}.value_source] == 1'
    FIX = f'[#{SEL}.value_source] == 2'
    CAN = f'[#{SEL}.value_source] == 0'
    EXP = f'[#{SEL}.value_source] == 3'
    cand = p.panel(420, y0, 430, h_cand, 'How Much  (the value)')
    y = 12
    y = p.field(cand, 10, y, 'Value From', f'{SEL}.value_source', 'enum', 170, enable=on)
    y = p.field(cand, 10, y, 'Fixed Value', f'{SEL}.fixed_x10', 'configedit', 100, '%',
                enable=f'{on} and {FIX}')
    # WHICH TABLE — not a table of this slot's own. Eight generic maps exist for exactly this, and any
    # other table in the tune can be named too; it is read at its own axes.
    y = p.field(cand, 10, y, 'Duty Table', f'{SEL}.duty_table_sel', 'enum', 250,
                enable=f'{on} and {TAB}')
    # A DUTY THAT IS ARITHMETIC. The same editor the conditions use, read for its number rather than
    # its truth — "clt * 2 - 100" is a fan ramp without a surface anybody had to draw.
    p.add(p._new('label', 10, y + 6, 110, A.LBL_H,
                 {'labelText': 'Duty Expression', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'enableCondition': f'{on} and {EXP}'}), into=cand)
    p.add(p._new('expression', 124, y + 2, 296, 30,
                 {'signalName': f'{SEL}.duty_expr', 'enableCondition': f'{on} and {EXP}'}), into=cand)
    y += 34
    y = p.field(cand, 10, y, 'Candidates In Use', f'{SEL}.n_cand', 'configedit', 90,
                enable=f'{on} and {CAN}')
    y = p.field(cand, 10, y, 'Primary Policy', f'{SEL}.primary_policy', 'enum', 170,
                enable=f'{on} and {CAN}')
    p.add(p._new('panel', 10, y + 4, 410, 1, {'bgColor': C_DIM, 'padding': '0'}), into=cand)
    y += 12
    for i in range(4):
        used = f'{on} and {CAN} and [#{SEL}.n_cand] > {i}'
        g = p.group()
        p.add(p._new('label', 10, y + 4, 40, A.LBL_H,
                     {'labelText': str(i + 1), 'align': 'Left', 'fontName': A.FONT_LBL,
                      'enableCondition': used}, g), into=cand)
        p.add(p._new(A.Page.enum_control(f'{SEL}.cand[{i}].sig'), 54, y + 2, 220, A.CTL_H,
                     {'signalName': f'{SEL}.cand[{i}].sig', 'enableCondition': used}, g), into=cand)
        p.add(p._new(A.Page.enum_control(f'{SEL}.cand[{i}].role'), 284, y + 2, 130, A.CTL_H,
                     {'signalName': f'{SEL}.cand[{i}].role', 'enableCondition': used}, g), into=cand)
        y += A.ROW
    p.note(420, y0 + h_cand + 4, NOTE_CAND, w=420)

    # ---- how the number is shaped ----------------------------------------------------------------
    shape = p.panel(860, y0, 410, A.panel_h(5, A.ROW, top=12, bottom=6), 'Shaping')
    y = 12
    y = p.field(shape, 10, y, 'Scale (x1000)', f'{SEL}.scale_x1000', 'configedit', 100, enable=on)
    y = p.field(shape, 10, y, 'Offset (x10)', f'{SEL}.offset_x10', 'configedit', 100, enable=on)
    y = p.field(shape, 10, y, 'Clamp Low (x10)', f'{SEL}.clamp_lo_x10', 'configedit', 100, enable=on)
    y = p.field(shape, 10, y, 'Clamp High (x10)', f'{SEL}.clamp_hi_x10', 'configedit', 100, enable=on)
    p.field(shape, 10, y, 'Failsafe (x10)', f'{SEL}.failsafe_x10', 'configedit', 100, enable=on)
    h_shape = A.panel_h(5, A.ROW, top=12, bottom=6)
    SHAPE_NOTE = (
           'Scale and offset are a straight line from the engineering value to the actuator command, so '
           'two known points define them. The clamps are the ACTUATOR\'s limits rather than a control '
           'decision, and the failsafe is what is emitted when no candidate is valid — arbitration '
           'always emits something.')
    # MEASURED, so the panels under it start where it actually ends. The 96px that used to be reserved
    # here was a guess with 88px of it spare, and closing that gap by eye put the next panel on top of
    # this note — which is the same mistake in the other direction.
    p.note(860, y0 + h_shape + 6, SHAPE_NOTE, w=400)
    h_shape_note = A.Page.note_h(SHAPE_NOTE, 400) + 12

    # WHAT THIS SLOT IS DOING. One value widget per output, stacked in the same place, shown by the
    # chooser — the arbitrated command is a channel per slot and cannot be indexed by an expression.
    # +8, not +96: the gap under Shaping was 88px of nothing, and the Bench Test panel below now needs
    # it. Sitting the live readout directly under Shaping puts "what this pin is doing" next to the
    # button that makes it do something, which is the pairing that matters while testing.
    live = p.panel(860, y0 + h_shape + 6 + h_shape_note, 410, 92, 'Right Now')
    # ONE readout, template-bound. This was forty-two stacked values each conditioned on the picker
    # matching its index — the picker's own shape, and it went with it. "$*" is the element's primary
    # channel, which for an output slot is the level or duty it is driving.
    p.add(p._new('value', 10, 12, 180, 40,
                 {'signalName': '[$*]', 'format': '%.0f', 'fontName': '|22|0|0',
                  'align': 'Center', 'borderWidth': '1', 'borderRadius': '3'}), into=live)
    p.add(p._new('label', 200, 14, 200, A.LBL_H,
                 {'labelText': 'Commanded output', 'align': 'Left', 'fontName': A.FONT_SMALL,
                  'fgColor': C_DIM}), into=live)
    p.add(p._new('label', 200, 38, 200, A.LBL_H,
                 {'labelText': 'Outputs  (every pin)', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{ROOT}/Electrical/Outputs'}), into=live)

    # ---- BENCH TEST ----------------------------------------------------------
    # ON THE PIN'S OWN PAGE, beside what it is doing right now, because that is where this pin's job is
    # already set and seen. A separate dialog listing "Spark #1..12 / Injector #1..12" would be a second
    # surface saying what this page already says, kept in step by hand — and it would name cylinders
    # while the hardware is pins.
    #
    # %ROW% is replaced with this page's output index by apply_outputs, which is the one thing a shared
    # template body cannot carry: every other binding here resolves through the page's element scope,
    # but a CLI command takes a NUMBER.
    TEST_NOTE = ('Engine stopped only; a start cancels it, and the ECU ends the test itself. What '
                 'happens comes from the Function above \u2014 a coil dwells then releases, an injector '
                 'pulses, anything else is held on. On Time is used as given: mind the dwell on a coil.')
    # AS TALL AS WHAT IS IN IT — three rows, the button row, then the note measured at the width it is
    # drawn at. The CAN Setup panel beside it was hand-tuned twice and outgrew both; there is a
    # Page.wrapped_h() for exactly this, so this one is measured from the start.
    TEST_NOTE_W = 390
    test_h = 12 + 3 * A.ROW + 8 + 30 + 6 + A.Page.wrapped_h(TEST_NOTE, TEST_NOTE_W) + A.PANEL_TITLE + 8
    test = p.panel(860, y0 + h_shape + 6 + h_shape_note + 92 + 8, 410, test_h, 'Bench Test')
    ty = 12
    ty = p.field(test, 10, ty, 'Count',    'pc.test_count',  'configedit', 90)
    ty = p.field(test, 10, ty, 'On Time',  'pc.test_on_ms',  'configedit', 90)
    ty = p.field(test, 10, ty, 'Off Time', 'pc.test_off_ms', 'configedit', 90)
    # The three are HOST variables shared by every output's page: set the shape of the test once and it
    # applies to whichever pin you press next.
    p.add(p._new('command', 10, ty + 8, 120, 30,
                 {'labelText': 'Test', 'command': 'test %ROW%',
                  'arg0': '[#pc.test_count]', 'arg1': '[#pc.test_on_ms]', 'arg2': '[#pc.test_off_ms]'}),
          into=test)
    p.add(p._new('command', 138, ty + 8, 90, 30,
                 {'labelText': 'Stop', 'command': 'test %ROW% 0 0 0'}), into=test)
    p.add(p._new('command', 236, ty + 8, 110, 30,
                 {'labelText': 'Stop All', 'command': 'test 255 0 0 0'}), into=test)
    # WHAT THE ECU WILL ACTUALLY DO, said here rather than discovered. The numbers above are a request:
    # an ignition output is not a switch, and a coil held on saturates and takes its driver with it, so
    # the firmware clamps the on-time per function and the console reply says when it did.
    p.wrapped(10, ty + 44, TEST_NOTE_W, TEST_NOTE, into=test, colour=C_DIM)

    return p


def page_freq_map():
    """The selected slot's CARRIER: one number, however it is arrived at.

    A PWM output is two numbers — how much (the duty, on the output's own page) and how fast it
    is chopped.
    The second has three answers: a fixed frequency, an expression, or a table it names. Most slots
    answer it once with a number and never come back; the ones that sweep a carrier (a buzzer's pitch,
    a valve walked across its range, a solenoid that follows engine speed) are why the other two exist.

    The node appears only for a slot whose Kind is PWM: a plain level has no carrier at all.
    """
    SEL = 'outputs.output[*]'
    p = A.Page(title='Frequency')
    y0 = p.head('How fast this output is chopped. A fixed frequency, an expression, or a table it '
                'names — the duty is on the output\'s own page; this is only the carrier.')

    # EVERY GATE CARRIES THE PIN'S OWN FUNCTION, as the setup page's do. A carrier setting means nothing
    # on a pin that is not a generic output, and a row that stays live then is a row edited to no effect.
    on = f'[#{SEL}.function] == 3'
    h_side = A.panel_h(3, A.ROW, top=12, bottom=6) + 34
    side = p.panel(10, y0, 470, h_side, 'Carrier')
    y = 12
    y = p.field(side, 10, y, 'Frequency From', f'{SEL}.freq_source', 'enum', 190)
    y = p.field(side, 10, y, 'Fixed Frequency', f'{SEL}.pwm_freq_hz', 'configedit', 100, 'Hz',
                enable=f'{on} and [#{SEL}.freq_source] == 0')
    y = p.field(side, 10, y, 'Frequency Table', f'{SEL}.freq_table_sel', 'enum', 250,
                enable=f'{on} and [#{SEL}.freq_source] == 1')
    # THE SAME EDITOR THE CONDITIONS USE, read for a frequency rather than a truth — "rpm / 4" is a
    # carrier that follows engine speed without a map anybody had to fill in.
    p.add(p._new('label', 10, y + 6, 110, A.LBL_H,
                 {'labelText': 'Expression', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'enableCondition': f'{on} and [#{SEL}.freq_source] == 2'}), into=side)
    p.add(p._new('expression', 124, y + 2, 336, 30,
                 {'signalName': f'{SEL}.freq_expr', 'enableCondition': f'{on} and [#{SEL}.freq_source] == 2'}),
          into=side)

    NOTE = ('Answers are hertz, clamped to what the timer can make. The pulse engine is software-timed, '
            'so a carrier much past two kilohertz costs more in edges than it buys in resolution. An '
            'unanswerable expression or table falls back to the fixed frequency rather than stopping '
            'the output — a duty with no carrier is not an output at all.')
    h_note = A.Page.note_h(NOTE, 470)
    p.note(10, y0 + h_side + 8, NOTE, w=470)

    live = p.panel(10, y0 + h_side + h_note + 16, 470, 132, 'Right Now')
    for n, (lbl, ch) in enumerate((('Engine RPM', 'rpm'), ('Coolant', 'clt'))):
        p.readout(live, 15 + n * 180, 12, lbl, ch, A.Page.chan_fmt(ch), 170)
    p.add(p._new('label', 15, 76, 340, A.LBL_H,
                 {'labelText': 'Outputs  (the duty)', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{ROOT}/Electrical/Outputs'}), into=live)

    tables = p.panel(500, y0, 470, A.panel_h(2, A.ROW, top=12, bottom=6) + 46, 'The Tables')
    p.wrapped(10, 12, 450,
              'A carrier table is a table like any other — the generic pool is where '
                               'one with no other job lives. Edit the cells there; name it here.',
              into=tables)
    p.add(p._new('label', 10, 62, 300, A.LBL_H,
                 {'labelText': 'Generic Tables', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{ROOT}/Generic Tables'}), into=tables)
    p.add(p._new('label', 10, 90, 300, A.LBL_H,
                 {'labelText': 'Outputs  (all slots)', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{ROOT}/Electrical/Outputs'}), into=tables)
    return p


def page_generic_table(i):
    """One of the eight tables that belong to nobody.

    Every other table here is owned by the module that reads it and says what its cells mean. These
    say nothing: their meaning arrives when an output's duty, an output's carrier or an expression
    points at one. So the page is the map, the two channels its axes read, and the NAME — which is the
    only thing that tells one of eight identically-shaped maps from another in a picker.
    """
    T = f'generic_tables.table_{i}'
    p = A.Page(title=f'Generic Table {i}')
    y0 = p.head('A table with no opinion about what it is for. Point an output\'s duty or carrier at '
                'it, or read it from an expression; its axes name the two channels it is read against.')

    p.table(10, y0, 880, A.CANVAS_H - y0 - 16, T)

    h_side = A.panel_h(4, A.ROW, top=12, bottom=6)
    side = p.panel(900, y0, 370, h_side, 'This Table')
    y = 12
    y = p.field(side, 10, y, 'Name', f'generic_tables.name_{i}', 'text', 190)
    y = p.field(side, 10, y, 'X Channel', f'{T}_x_src', 'enum', 190)
    # A DISABLED AXIS READS NO CHANNEL. TableEval collapses an optional axis whose enable is off to
    # index 0 and never fetches its coordinate (TableEval.h:131-137), so picking a Y channel while the
    # Y axis is unused chooses what nothing looks at. Every other optional axis in the ECU already
    # greys its selector this way; these eight were the ones nobody had.
    y = p.field(side, 10, y, 'Y Channel', f'{T}_y_src', 'enum', 190,
                enable=f'[#{T}_y_en] == 1')
    p.field(side, 10, y, 'Y Axis Used', f'{T}_y_en', 'checkbox')

    NOTE = ('The name is what the pickers show, so call it what it does — "Fan Ramp", "Buzzer Pitch". '
            'Cells mean whatever the thing reading them takes them to mean: percent for a duty, hertz '
            'for a carrier, anything at all for an expression. Turn the Y axis off and it is a curve '
            'against one channel, which is what most of these end up being.')
    h_note = A.Page.note_h(NOTE, 360)
    p.note(900, y0 + h_side + 8, NOTE, w=360)

    live = p.panel(900, y0 + h_side + h_note + 16, 370, 132, 'Right Now')
    # WHAT THIS TABLE IS PUTTING OUT, first. A generic table is whatever the tuner points at it, so no
    # module channel means "this one" — its own path is the only honest source, and Cache::solveTable
    # answers it by interpolating the grid against the two channels its axes name.
    p.readout(live, 15, 12, 'Output', T, A.table_fmt(T), 170)
    for n, (lbl, ch) in enumerate((('Engine RPM', 'rpm'),)):
        p.readout(live, 195 + n * 180, 12, lbl, ch, A.Page.chan_fmt(ch), 170)
    p.add(p._new('label', 15, 76, 340, A.LBL_H,
                 {'labelText': 'Outputs  (what reads a table)', 'align': 'Left',
                  'fontName': A.FONT_LBL,
                  'link': f'{ROOT}/Electrical/Outputs'}), into=live)
    return p


def page_generic_tables():
    """The pool, listed — which of the eight are named, and what each is read against."""
    p = A.Page(title='Generic Tables')
    y0 = p.head('Eight tables that belong to nobody. An output\'s duty, an output\'s carrier or an '
                'expression can point at any of them; until something does, a table is just cells.')
    rows = []
    for i in range(1, 9):
        rows.append((f'Generic Table {i}', f'{ROOT}/Generic Tables/Generic Table {i}'))
    x, y = 10, y0
    for n, (name, path) in enumerate(rows):
        col, row = n // 4, n % 4
        pnl = p.panel(10 + col * 430, y0 + row * 92, 410, 86, name)   # 86: the label at 40+20 needs 60 inside; 84 gave 58
        # THE NAME ITSELF, as the box that edits it. A label does not expand [#…] — it printed the
        # sigil text — and an unnamed table is exactly the one that most needs somewhere to type.
        p.add(p._new('text', 10, 6, 260, 28, {'signalName': f'generic_tables.name_{n + 1}'}), into=pnl)
        p.add(p._new('label', 10, 40, 380, A.LBL_H,
                     {'labelText': 'Open to edit its cells, axes and name.', 'align': 'Left',
                      'fontName': A.FONT_SMALL, 'fgColor': C_DIM, 'link': path}), into=pnl)
    return p


def output_row_count():
    """How many physical outputs THIS BOARD has — the length of the meta's element_labels.

    jaytek_v1 has 42 (IGN12 + LS22 + HS8) and proteus_f7 has 32 (IGN12 + LS16 + HS4). It was a
    hardcoded 42, so the second board would have drawn ten rows for pins it does not own.
    """
    from apply_fuel import load_meta
    labels = (load_meta().get('config', {}).get('outputs', {}).get('output', {}) or {}).get('element_labels') or []
    return len(labels) or 42


def output_row_names(count=None):
    """The pin each output row IS — the meta's element labels (IGN1.., LS1.., HS1..), from the board."""
    from apply_fuel import load_meta
    labels = (load_meta().get('config', {}).get('outputs', {}).get('output', {}) or {}).get('element_labels') or []
    if count is None:
        count = len(labels) or 42
    return [labels[i] if i < len(labels) else f'Output {i + 1}' for i in range(count)]


def page_outputs(count=None):
    """Every physical output, by pin: what it does, which cylinder it serves, and what it is driving now.

    outputs.output[i] IS pin i. Each row is the pin (a link to its own page), its Function and its
    Cylinder — both editable here, and both filtered the same way as on the pin's page: Function to what
    the pin can do (only an IGN pin can be a coil, only an LS pin an injector), Cylinder to what this
    engine and the row's stage can use. The studio still lays coils and injectors out when the cylinder
    count, ignition mode or stages change; this is where the result is scanned and adjusted.
    """
    if count is None:
        count = output_row_count()
    names = output_row_names(count)
    # WIDE ENOUGH FOR WHAT IS ON IT, the way fuel_tree sizes itself to its table — three columns of
    # outputs need 3x330 plus gaps, which is more than the 1280 this was authored at.
    COL_W, ROWS, COLS, PITCH = 330, 14, 3, 36
    cols_used = max(1, min(COLS, -(-count // ROWS)))
    page_w = max(A.CANVAS_W, 10 + cols_used * (COL_W + 5) + 10)
    p = A.Page(w=page_w, title='Outputs')
    # WHERE THE BLURB ENDS, not A.TOP: it wraps to two lines, and the panels started at the constant ran
    # up under its second line.
    y0 = p.head('Every output the board can drive, by pin: what it does and which cylinder it serves. '
                'Coils and injectors are laid out for you when the cylinder count, ignition mode or '
                'injection stages change; change any pin here or on its own page.')
    # THREE COLUMNS OF FOURTEEN, not two of twenty-one. The old shape only fit by shaving every
    # control to 21px on a 27px pitch — 42 full-height rows in two columns is 686px of a 700px page.
    # Nothing about this grid wanted smaller controls; the column count did, and that is the thing
    # to change. At CTL_H the pitch is 36 and fourteen rows is 518px, with room over.
    for c in range(COLS):
        first = c * ROWS
        if first >= count:
            break
        n = min(ROWS, count - first)
        panel = p.panel(10 + c * (COL_W + 5), y0, COL_W,
                        A.panel_h(n, PITCH, top=10, bottom=4), f'{names[first]} - {names[first + n - 1]}')
        y = 10
        for i in range(first, first + n):
            g = p.group()
            row = f'outputs.output[{i}]'
            # THE PIN IS THE WAY IN. Its node is always in the tree, so the link is always followable.
            p.add(p._new('label', 8, y + 5, 48, A.LBL_H,
                         {'labelText': names[i], 'align': 'Left', 'fontName': A.FONT_LBL,
                          'link': f'{ROOT}/Electrical/Outputs/{names[i]}'}, g), into=panel)
            p.add(p._new(A.Page.enum_control(f'{row}.function'), 58, y, 84, A.CTL_H,
                         {'signalName': f'{row}.function'}, g), into=panel)
            # A cylinder means something only for a coil or an injector.
            p.add(p._new(A.Page.enum_control(f'{row}.cylinder'), 146, y, 116, A.CTL_H,
                         {'signalName': f'{row}.cylinder',
                          'enableCondition': f'[#{row}.function] == 1 or [#{row}.function] == 2'}, g),
                  into=panel)
            p.add(p._new('value', 266, y, 56, A.CTL_H,
                         {'signalName': f'[$out_{i + 1}]', 'format': '%.0f', 'fontName': A.FONT_SMALL,
                          'align': 'Right', 'enableCondition': f'[#{row}.function] == 3'}, g),
                  into=panel)
            y += PITCH
    return p
