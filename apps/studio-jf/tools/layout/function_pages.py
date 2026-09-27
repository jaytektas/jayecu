"""Engine-function pages that were a list of settings and are now the job they belong to.

Each one follows the shape the throttle pages settled on: what it reads, what it may do, how it is wired,
and what it is doing right now — with the traps written where the setting is, not in a manual.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from author import C_DIM, C_GREEN, C_AMBER, C_RED, C_BLUE

CFG = 'Configuration'
FUN = f'{CFG}/Engine Functions'
# Beside Outputs, not inside the throttle. A stepper's driver is the same hardware an electronic
# throttle uses, and this page's links used to take a stepper user into the THROTTLE branch to set it up.
HB  = 'Configuration/Electrical/Half Bridges'

HBRIDGE, STEPDIR = 0, 1          # stepper.driver_mode


def page_stepper():
    """A stepper idle valve: the driver it uses, the travel it has, and — the part that was missing — how
    the two H-bridges get told to drive it.

    The page it replaces listed the ten settings and said nothing about the wiring, which is the half that
    is easy to get wrong and silent when it is: in H-Bridge mode the module publishes four signals and the
    bridges have to be pointed at them by hand. A valve that never moves because bridge[0].demand_sig was
    left unset looks exactly like a valve that is stuck.
    """
    s = 'stepper'
    p = A.Page(title='Idle Stepper')
    y0 = p.head('A stepper idle valve instead of a PWM one. It follows a 0-100% demand — idle duty, '
                'normally — and steps a motor there, slowly enough that the valve does not miss steps.',
                enable=f'{s}.enabled')
    on = f'[#{s}.enabled] == 1'
    hb = f'{on} && [#{s}.driver_mode] == {HBRIDGE}'
    sd = f'{on} && [#{s}.driver_mode] == {STEPDIR}'

    drv = p.panel(10, y0, 400, A.panel_h(3, A.ROW, top=12, bottom=6), 'Driver', enable=on)
    y = 12
    y = p.field(drv, 10, y, 'Driver Mode', f'{s}.driver_mode', 'enum', 230, lbl_w=150)
    y = p.field(drv, 10, y, 'Demand Signal', f'{s}.input_sig', 'enum', 200, lbl_w=150)
    p.check(drv, 10, y + 2, 'Rotation Invert', f'{s}.dir_invert')
    h1 = A.panel_h(3, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h1 + 6,
           'H-BRIDGE drives the two coils itself and uses BOTH bridges — so it cannot share a car with '
           'dual drive-by-wire, which claims them too. STEP/DIRECTION emits pulses for an external driver '
           'IC (A4988, DRV8825) on plain GPIO outputs and leaves the bridges alone.', w=390)

    y1 = y0 + h1 + 116
    trv = p.panel(10, y1, 400, A.panel_h(4, A.ROW, top=12, bottom=6), 'Travel & Rate', enable=on)
    y = 12
    y = p.field(trv, 10, y, 'Range', f'{s}.range_steps', 'configedit', 100, 'steps', lbl_w=150)
    y = p.field(trv, 10, y, 'Step Period', f'{s}.step_period_ms', 'configedit', 100, 'ms', lbl_w=150)
    y = p.field(trv, 10, y, 'Microsteps', f'{s}.microstep', 'configedit', 100, '', lbl_w=150,
                enable=hb)
    p.field(trv, 10, y, 'Max Steps / Update', f'{s}.max_step_per_update', 'configedit', 100, '', lbl_w=150,
            enable=hb)
    p.note(10, y1 + A.panel_h(4, A.ROW, top=12, bottom=6) + 6,
           'Range is the whole travel: 0-100% of demand maps onto it. Step period is the minimum time per '
           'FULL step — a GM-style IAC wants 5-10 ms, and going below what the valve can follow makes it '
           'miss steps silently, because a stepper has no feedback to notice. Microsteps and the per-'
           'service ceiling are H-Bridge only; an external driver owns its own microstepping.', w=390)

    cur = p.panel(420, y0, 380, A.panel_h(2, A.ROW, top=12, bottom=6), 'Coil Current', enable=hb)
    y = 12
    y = p.field(cur, 10, y, 'Move Current', f'{s}.move_current_pct', 'configedit', 100, '%', lbl_w=140)
    p.field(cur, 10, y, 'Hold Current', f'{s}.hold_current_pct', 'configedit', 100, '%', lbl_w=140)
    h2 = A.panel_h(2, A.ROW, top=12, bottom=6)
    p.note(420, y0 + h2 + 6,
           'Moving needs more current than holding: too little and the motor stalls and loses position '
           'without saying so. Holding is a static load with no back-EMF to help, so full current there is '
           'heat in the motor and the driver for no more holding torque — and too little lets the valve '
           'creep under spring or airflow load.', w=370)

    # ---- the wiring, per mode ------------------------------------------------------------------
    wy = y0 + h2 + 116
    wire = p.panel(420, wy, 380, 286, 'Wiring · H-Bridge', cond=f'[#{s}.driver_mode] == {HBRIDGE}',
                   enable=on)
    p.wrapped(10, 10, 360,
              'The module publishes two signed coil demands and their enables. Point the '
                               'bridges at them by hand — nothing does it for you, and a bridge left '
                               'unassigned drives nothing while the valve looks stuck:',
              into=wire)
    rows = (('Half Bridge A', 'demand = step_demand_a', 'enable = step_en_a', 0),
            ('Half Bridge B', 'demand = step_demand_b', 'enable = step_en_b', 1))
    ry = 108
    for name, dem, en, i in rows:
        p.add(p._new('label', 10, ry, 150, A.LBL_H,
                     {'labelText': name, 'align': 'Left', 'fontName': A.FONT_LBL,
                      'link': f'{HB}/{name}'}), into=wire)
        p.add(p._new('label', 165, ry, 205, A.LBL_H,
                     {'labelText': dem, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}),
              into=wire)
        p.add(p._new('label', 165, ry + 20, 205, A.LBL_H,
                     {'labelText': en, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}),
              into=wire)
        ry += 48
    p.wrapped(10, ry + 4, 360,
              'Both bridges must be set to UNIPOLAR: the coil demands are signed.',
              into=wire, colour=C_AMBER)

    wire2 = p.panel(420, wy, 380, 286, 'Wiring · Step / Direction', cond=f'[#{s}.driver_mode] == {STEPDIR}',
                    enable=on)
    p.wrapped(10, 10, 360,
              'The module publishes three levels for an external driver IC. Give each one '
                               'an output slot on the Outputs page:',
              into=wire2)
    for n, (sig, what) in enumerate((('step_pulse', 'one pulse per step → STEP'),
                                     ('step_dir', 'held level → DIR'),
                                     ('step_enable', 'held level → ENABLE'))):
        p.add(p._new('label', 10, 88 + n * 26, 130, A.LBL_H,
                     {'labelText': sig, 'align': 'Left', 'fontName': A.FONT_LBL}), into=wire2)
        p.add(p._new('label', 145, 88 + n * 26, 225, A.LBL_H,
                     {'labelText': what, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}),
              into=wire2)
    p.add(p._new('label', 10, 172, 360, A.LBL_H,
                 {'labelText': 'Outputs', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{CFG}/Electrical/Outputs'}), into=wire2)
    p.wrapped(10, 198, 360,
              'Leaves both H-bridges free, so this is the mode to use alongside a '
                               'drive-by-wire throttle.',
              into=wire2)

    # ---- live ----------------------------------------------------------------------------------
    live = p.panel(810, y0, 460, 320, 'Right Now')
    p.readout(live, 15, 14, 'Demand', '[$idle_duty]', '%.1f', 130)
    p.readout(live, 150, 14, 'Engine RPM', '[$rpm]', '%.0f', 130)
    p.readout(live, 285, 14, 'Idle Target', '[$idle_target_rpm]', '%.0f', 130)
    p.readout(live, 15, 76, 'Coil A', '[$step_demand_a]', '%.1f', 130)
    p.readout(live, 150, 76, 'Coil B', '[$step_demand_b]', '%.1f', 130)
    p.add(p._new('indicator', 285, 82, 130, 40,
                 {'signalName': '[$step_en_a]', 'onTitle': 'COILS ON', 'offTitle': 'COILS OFF',
                  'onBg': C_GREEN, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_SMALL}), into=live)
    p.traces(15, 132, 430, 110, [('[$idle_duty]', 0, 100), ('[$step_demand_a]', -100, 100),
                                 ('[$step_demand_b]', -100, 100)], into=live)
    p.wrapped(15, 248, 430,
              'The two coils should look like a sine and a cosine of each other while '
                               'the valve moves, and go quiet at the hold current when it arrives.',
              into=live)

    nxt = p.panel(810, y0 + 330, 460, A.panel_h(3, 28, top=12, bottom=6), 'Its Parts')
    y = 12
    for label, node in (('Idle Control  (what sets the demand)', f'{FUN}/Idle Control'),
                        ('Half Bridge A', f'{HB}/Half Bridge A'),
                        ('Half Bridge B', f'{HB}/Half Bridge B')):
        y = p.switch(nxt, 10, y, label, '', link=node, w=400, pitch=28)

    p.note(810, y0 + 330 + A.panel_h(3, 28, top=12, bottom=6) + 8,
           'On enable the valve is homed: driven closed past its full travel into its stop, which '
           'becomes 0. An engine stop leaves it where it is. From then on its position is the steps '
           'the ECU has issued.', w=450)
    return p


def page_misfire():
    """Misfire detection: how a slow segment becomes a fault, and what the ECU does about it.

    The setting nobody can guess is the threshold, so the page says what it is measured against rather
    than leaving 8% looking like a fact. The segment corrections are learned, not typed, and they are
    RAM persisted to SD rather than tune data — which is why they get their own panel that says so
    instead of sitting among the settings as if they were.
    """
    mf = 'misfire'
    p = A.Page(title='Misfire Detection')
    y0 = p.head('A cylinder that does not fire makes its crank segment take longer than its neighbours. '
                'This is the arithmetic that turns that into a fault code — and, if you let it, into a '
                'fuel cut.', enable=f'{mf}.enabled')
    on = f'[#{mf}.enabled] == 1'

    det = p.panel(10, y0, 400, A.panel_h(3, A.ROW, top=12, bottom=6), 'Detecting', enable=on)
    y = 12
    y = p.field(det, 10, y, 'Threshold', f'{mf}.threshold_pct', 'configedit', 100, '%', lbl_w=150)
    y = p.field(det, 10, y, 'Min RPM', f'{mf}.min_rpm', 'configedit', 100, 'RPM', lbl_w=150)
    p.field(det, 10, y, 'Max RPM', f'{mf}.max_rpm', 'configedit', 100, 'RPM', lbl_w=150)
    h1 = A.panel_h(3, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h1 + 6,
           'Threshold is how much longer than the running mean a segment must be to count. The default is '
           'a starting point, not a measurement: a real engine\'s own torsional variation sets the floor, '
           'and it is found by watching roughness with the engine healthy. Outside the RPM band nothing '
           'is counted — cranking and the top of the range are both too noisy to judge.', w=390)

    y1 = y0 + h1 + 122
    rep = p.panel(10, y1, 400, A.panel_h(3, A.ROW, top=12, bottom=6), 'Reporting', enable=on)
    y = 12
    y = p.field(rep, 10, y, 'Window', f'{mf}.window_cycles', 'configedit', 100, 'cycles', lbl_w=150)
    y = p.field(rep, 10, y, 'Events Before DTC', f'{mf}.events_to_dtc', 'configedit', 100, '', lbl_w=150)
    p.field(rep, 10, y, 'Cylinders for P0300', f'{mf}.multi_cyl_for_p0300', 'configedit', 100, '',
            lbl_w=150)
    p.note(10, y1 + A.panel_h(3, A.ROW, top=12, bottom=6) + 6,
           'One bad cycle is not a fault: a cylinder needs that many events inside the window before its '
           'own P030x is raised, and when several cylinders are over threshold at once the code becomes '
           'P0300 — random/multiple — instead of a list of individual ones. The OBD monitors this mirrors '
           'use 200- and 1000-revolution windows for the same job.', w=390)

    # ---- the learned half ----------------------------------------------------------------------
    lrn = p.panel(420, y0, 420, A.panel_h(2, A.ROW, top=12, bottom=6) + 30, 'Segment Correction',
                  enable=on)
    y = 12
    y = p.check(lrn, 10, y, 'Learn on overrun', f'{mf}.learn_enabled')
    p.field(lrn, 10, y + 4, 'Learn Rate', f'{mf}.learn_rate', 'configedit', 100, '', lbl_w=150)
    learn_h = A.panel_h(2, A.ROW, top=12, bottom=6) + 30
    # ONE COPY OF THE WORDS. This note was written out twice — once to draw and once to measure, with
    # the measuring copy asking at the wrong pitch — so the panel below it was placed to a height the
    # note does not have and sat on its last line.
    LEARN_NOTE = ('A trigger wheel is never perfect, and a narrow tooth makes one cylinder\'s segment '
                  'permanently long — which reads as a cylinder that always misfires. So the correction '
                  'is LEARNED during overrun fuel cut, when nothing is firing and every segment should '
                  'be identical. The twelve learned trims live in RAM and are persisted to SD: they are '
                  'a property of this engine\'s wheel, not part of the tune, and they do not travel '
                  'with it.')
    ln = p.note(420, y0 + learn_h + 6, LEARN_NOTE, w=410)

    act = p.panel(420, ln['y'] + ln['h'] + 10,
                  420, A.panel_h(1, A.ROW, top=12, bottom=6) + 96, 'Acting On It', enable=on)
    p.check(act, 10, 12, 'Cut fuel to a dead cylinder', f'{mf}.cut_fuel')
    p.wrapped(10, 46, 400,
              'Raw fuel through a dead cylinder goes into the catalyst and destroys it, so cutting '
                      'the injector protects the exhaust. It is off by default because it is a decision '
                      'about the engine rather than a diagnostic one: a cylinder cut on a false positive '
                      'is a cylinder you have switched off.',
              into=act)

    # WHAT IT HAS LEARNED, per cylinder. Read-only on purpose: these are measured during overrun, not
    # typed, and the panel above says why. Without them the page described a learned correction that
    # could not be looked at — twelve numbers that decide whether a healthy cylinder reads as a misfire.
    seg = p.panel(420, y0 + 444, 420, 190, 'Learned Segment Trim  (per cylinder)', enable=on)
    for i in range(12):
        col, row = i // 6, i % 6
        # 8, not 10: six rows of 25 with a 30px readout on the last one ended 1px past the
        # content area. Two pixels off the top costs nothing and keeps the panel its own size.
        cx, cy = 10 + col * 200, 8 + row * 25
        vis = f'[#engine.cylinder_count] >= {i + 1}'
        g = p.group()
        p.add(p._new('label', cx, cy + 4, 60, A.LBL_H,
                     {'labelText': f'Cyl {i + 1}', 'align': 'Left', 'fontName': A.FONT_SMALL,
                      'fgColor': C_DIM, 'condition': vis}, g), into=seg)
        p.add(p._new('value', cx + 62, cy, 110, A.CTL_H,
                     {'signalName': f'{mf}.misfire_seg_{i + 1}', 'format': '%.2f', 'align': 'Right',
                      'showUnit': '0', 'fontName': A.FONT_LBL, 'borderWidth': '1', 'borderRadius': '3',
                      'condition': vis}, g), into=seg)

    # ---- live ----------------------------------------------------------------------------------
    live = p.panel(850, y0, 420, 366, 'Right Now')
    p.readout(live, 15, 14, 'Roughness', '[$misfire_rough]', '%.1f', 120)
    p.readout(live, 145, 14, 'Count', '[$misfire_count]', '%.0f', 120)
    p.readout(live, 275, 14, 'Engine RPM', '[$rpm]', '%.0f', 120)
    p.add(p._new('indicator', 15, 80, 180, 40,
                 {'signalName': '[$misfire_count] > 0', 'onTitle': 'MISFIRE SEEN', 'offTitle': 'CLEAN',
                  'onBg': C_RED, 'onFg': '#000000', 'offBg': '#1e3a24', 'offFg': '#8a8f98',
                  'fontName': A.FONT_SMALL}), into=live)
    p.add(p._new('indicator', 205, 80, 190, 40,
                 {'signalName': '[$misfire_cut_mask] > 0', 'onTitle': 'CYLINDER CUT',
                  'offTitle': 'NONE CUT', 'onBg': C_AMBER, 'onFg': '#000000', 'offBg': '#2a2a2e',
                  'offFg': '#6e7178', 'fontName': A.FONT_SMALL}), into=live)
    p.traces(15, 130, 390, 120, [('misfire_rough', -100, 100), ('rpm', 0, 8000)], into=live)
    p.wrapped(15, 256, 390,
              'Roughness is the segment variation the threshold is compared against. Watch it on '
                      'a healthy engine first: whatever it reaches there is the floor, and a threshold '
                      'under it counts the engine itself as a misfire.',
              into=live)

    nxt = p.panel(850, y0 + 376, 420, A.panel_h(3, 28, top=12, bottom=6), 'Related')
    y = 12
    for label, node in (('Trigger Diagnostics  (segment quality)',
                         f'{CFG}/Engine Configuration/Trigger System/Diagnostics'),
                        ('Cylinders & Firing  (which cylinder is which)',
                         f'{CFG}/Engine Configuration/Cylinders & Firing'),
                        ('Diagnostic Trouble Codes', f'{CFG}/Protection')):
        y = p.switch(nxt, 10, y, label, '', link=node, w=370, pitch=28)
    return p


# ---- engine protection ---------------------------------------------------------------------------

PROT = f'{CFG}/Protection/Engine Protection'
LEVELS = 3
MONITORS = 8


def page_protection():
    """The engine's own limits: what it watches, and what it does about it.

    Five widgets for a module with three protection LEVELS of ten settings each and eight programmable
    monitors — none of which were on the page at all. They are two grids, so they are two pages: this one
    is the thresholds the ECU checks by itself, and what it is doing right now.
    """
    e = 'engine_protection'
    p = A.Page(title='Engine Protection')
    y0 = p.head('The limits the ECU enforces on its own, whatever the tune asks for. Everything here acts '
                'BEFORE damage, which is why the warning threshold matters as much as the cut.')

    th = p.panel(10, y0, 420, A.panel_h(4, A.ROW, top=12, bottom=6), 'Built-in Thresholds')
    y = 12
    y = p.field(th, 10, y, 'Coolant Warning', f'{e}.clt_warn_c', 'configedit', 100, 'C', lbl_w=170)
    y = p.field(th, 10, y, 'Coolant Cut', f'{e}.clt_cut_c', 'configedit', 100, 'C', lbl_w=170)
    y = p.field(th, 10, y, 'Air Temp Cut', f'{e}.iat_cut_c', 'configedit', 100, 'C', lbl_w=170)
    p.field(th, 10, y, 'Overboost Cut', f'{e}.map_cut_kpa', 'configedit', 100, 'kPa', lbl_w=170)
    h1 = A.panel_h(4, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h1 + 6,
           'Warning comes first and is the one worth setting carefully: it is the level at which the '
           'engine is still fine and something should be done about it. The cut is the last resort, and '
           'an engine that reaches it has already been at the warning level for a while.', w=410)

    y1 = y0 + h1 + 108
    sig = p.panel(10, y1, 420, A.panel_h(2, A.ROW, top=12, bottom=6), 'Trusting The Signals')
    y = 12
    y = p.field(sig, 10, y, 'Sensor Stale Timeout', f'{e}.sensor_timeout_ms', 'configedit', 100, 'ms',
                lbl_w=170)
    p.field(sig, 10, y, 'Trigger Error Limit', f'{e}.trigger_error_pct_limit', 'configedit', 100, '%',
            lbl_w=170)
    p.note(10, y1 + A.panel_h(2, A.ROW, top=12, bottom=6) + 6,
           'A protection that reads a stale sensor is worse than none: it either cuts on a number that '
           'stopped being true, or misses the one it was watching for. Past the timeout the reading is '
           'treated as absent. The trigger-error limit is the same idea for position — 0 disables it.',
           w=410)

    # ---- where the detail lives ----------------------------------------------------------------
    nxt = p.panel(440, y0, 400, A.panel_h(4, 28, top=12, bottom=6), 'The Two Grids')
    y = 12
    for label, node in ((f'Protection Levels  ({LEVELS} levels)', f'{PROT}/Protection Levels'),
                        (f'Threshold Monitors  ({MONITORS} slots)', f'{PROT}/Threshold Monitors'),
                        ('Lambda Protection', f'{CFG}/Protection/Lambda Protection'),
                        ('Misfire Detection', f'{CFG}/Protection/Misfire Detection')):
        y = p.switch(nxt, 10, y, label, '', link=node, w=350, pitch=28)
    p.note(440, y0 + A.panel_h(4, 28, top=12, bottom=6) + 6,
           'A LEVEL is a package of responses — enrich, retard, trim boost, drop the rev limit — '
           'raised by a DTC condition and held until it auto-resets. A MONITOR is a condition you write, over '
           'any bus signal, which is how you protect against something the built-in '
           'checks do not cover: oil pressure, a fuel-pressure differential, a gearbox temperature.',
           w=390)

    # ---- live ----------------------------------------------------------------------------------
    live = p.panel(850, y0, 420, 400, 'Right Now')
    p.readout(live, 15, 14, 'Ign Retard', '[$prot_ign_retard]', '%.1f', 120)
    p.readout(live, 145, 14, 'Soft Cut', '[$soft_cut_pct]', '%.0f', 120)
    p.readout(live, 275, 14, 'Rev Limit', '[$prot_rev_limit]', '%.0f', 120)
    lamps = (('PROTECTING', '[$prot_status] > 0', C_AMBER),
             ('FUEL CUT', '[$fuel_cut]', C_RED),
             ('IGN CUT', '[$ign_cut]', C_RED))
    for n, (title, expr, colour) in enumerate(lamps):
        p.add(p._new('indicator', 15 + n * 132, 80, 124, 40,
                     {'signalName': expr, 'onTitle': title, 'offTitle': title, 'onBg': colour,
                      'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    p.readout(live, 15, 130, 'Coolant', '[$clt]', '%.1f', 120)
    p.readout(live, 145, 130, 'Air Temp', '[$iat]', '%.1f', 120)
    p.readout(live, 275, 130, 'MAP', '[$map]', '%.0f', 120)
    p.traces(15, 196, 390, 100, [('clt', 0, 130), ('map', 0, 300), ('prot_ign_retard', 0, 60)], into=live)
    p.wrapped(15, 302, 390,
              'Retard and the reduced rev limit are what protection looks like before a cut: if '
                      'they are on, something is already over a threshold.',
              into=live)
    return p


def page_protection_levels():
    """Three levels, ten responses each — as a grid, because that is what it is.

    Rows are the response, columns are the level, so the question the page exists to answer ("what does
    level 2 do that level 1 does not?") is read across a row instead of held in your head across three
    forms.
    """
    e = 'engine_protection.protection_levels'
    p = A.Page(title='Protection Levels')
    y0 = p.head('Each level is a package of responses, raised by a DTC condition and held until it auto-'
                'resets. Read across a row to see what the next level adds.')

    ROWS = [('Enabled', 'enabled', 'checkbox', ''),
            ('DTC Condition', 'dtc_condition', 'enum', ''),
            ('Enrichment', 'enrich_pct', 'configedit', '%'),
            ('Ignition Retard', 'ign_retard_deg', 'configedit', 'deg'),
            ('Boost Correction', 'boost_corr_pct', 'configedit', '%'),
            ('Rev Limit Type', 'rev_limit_type', 'enum', ''),
            ('Rev Limit', 'rev_limit_rpm', 'configedit', 'rpm'),
            ('Auto Reset', 'auto_reset_s', 'configedit', 's')]

    grid = p.panel(10, y0, 900, A.panel_h(len(ROWS) + 1, A.ROW, top=34, bottom=8), 'Levels')
    for n in range(LEVELS):
        p.add(p._new('label', 250 + n * 210, 8, 200, 18,
                     {'labelText': f'Level {n + 1}', 'align': 'Left', 'fontName': '|15|1|0',
                      'fgColor': C_DIM}), into=grid)
    p.add(p._new('panel', 10, 28, 870, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    ry = 34
    for label, field, kind, unit in ROWS:
        p.add(p._new('label', 10, ry + 4, 230, A.LBL_H,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_LBL}), into=grid)
        for n in range(LEVELS):
            on = f'[#{e}[{n}].enabled] == 1' if field != 'enabled' else ''
            x = 250 + n * 210
            path = f'{e}[{n}].{field}'
            if kind == 'checkbox':
                cp = {'signalName': path}
                p.add(p._new('checkbox', x, ry + 3, A.CHECK_H, A.CHECK_H, cp), into=grid)
            else:
                props = {'signalName': path}
                if on: props['enableCondition'] = on
                ctl = A.Page.enum_control(path) if kind == 'enum' else kind
                p.add(p._new(ctl, x, ry, 170, A.CTL_H, props), into=grid)
        ry += A.ROW

    note = p.panel(920, y0, 350, 336, 'How A Level Works')
    p.wrapped(10, 10, 330,
              'A level is raised when its DTC condition is met — CURRENT means the fault is '
                      'happening now, STORED means it happened and has not been cleared. While it holds, '
                      'every response in its column is applied at once: the mixture is enriched, timing '
                      'is pulled, boost is trimmed and the rev limit drops.\n\n'
                      'The levels are meant to escalate: level 1 is what you can drive home on, level 3 '
                      'is what stops the engine being damaged. Auto Reset is how long after the condition '
                      'clears before the level lets go — 0 holds it until the fault is cleared by hand.'
                      '\n\nA level with no responses set does nothing at all, whatever raises it.',
              into=note)

    live = p.panel(920, y0 + 346, 350, A.CANVAS_H - (y0 + 346) - 8, 'Right Now')
    p.readout(live, 15, 14, 'Status Bits', '[$prot_status]', '%.0f', 100)
    p.readout(live, 125, 14, 'Retard', '[$prot_ign_retard]', '%.1f', 100)
    p.readout(live, 235, 14, 'Rev Limit', '[$prot_rev_limit]', '%.0f', 100)
    p.add(p._new('label', 15, 76, 320, A.LBL_H,
                 {'labelText': 'Engine Protection', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': PROT}), into=live)
    return p


def page_threshold_monitors():
    """Eight slots, each an EXPRESSION — the escape hatch, with the whole VM behind it."""
    e = 'engine_protection.threshold_monitors'
    p = A.Page(title='Threshold Monitors')
    y0 = p.head('Eight slots, written condition first — a slot with no condition watches nothing, so its '
                'action stays inert until one is typed. Each watches a CONDITION you write — any bus channel, any config setting, '
                'combined however the question needs — and acts while it holds. This is how you protect '
                'against something the built-in checks do not cover.')

    grid = p.panel(10, y0, 900, A.panel_h(MONITORS + 1, 46, top=34, bottom=8), 'Monitors')
    for cx, cw, text in ((10, 40, '#'), (56, 560, 'Condition'), (626, 200, 'Action'), (836, 50, 'Trip')):
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
    p.add(p._new('panel', 10, 28, 870, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    ry = 34
    for i in range(MONITORS):
        # A slot is entered CONDITION FIRST: the action is what to do about a question, so it stays
        # inert until the question exists. The gate reads the expression field itself — Cache reports a
        # program's PRESENCE as 1/0 — which is the right way round; gating the condition on the action
        # (as this first did) left every row disabled, since a fresh slot's action is Off.
        written = f'[#{e}[{i}].condition] > 0'
        p.add(p._new('label', 10, ry + 8, 40, A.LBL_H,
                     {'labelText': str(i + 1), 'align': 'Left', 'fontName': A.FONT_LBL}), into=grid)
        p.add(p._new('expression', 56, ry + 4, 560, 30,
                     {'signalName': f'{e}[{i}].condition'}), into=grid)
        p.add(p._new(A.Page.enum_control(f'{e}[{i}].action'), 626, ry + 6, 200, A.CTL_H,
                     {'signalName': f'{e}[{i}].action', 'enableCondition': written}), into=grid)
        # WHICH slot tripped, from the firmware's own bitmask — the missing half of "record only
        # first": watching a flag you cannot see is not proving anything.
        p.add(p._new('indicator', 836, ry + 6, 44, 26,
                     {'signalName': f'bit([$monitor_flags], {i})', 'onTitle': '!', 'offTitle': '',
                      'onBg': C_RED, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=grid)
        ry += 46

    slot_text = ('Write what SHOULD NOT be true. "oil_pressure < 100 and rpm > 2000" is a pressure limit '
                 'that only applies while the engine is turning — which the low/high pair this replaces '
                 'could not say at all. Channels, config settings, arithmetic between two channels, bits '
                 'of a status word, and age(signal) for "this one went quiet" are all fair game.\n\n'
                 'RECORD ONLY is worth using first: it sets the slot\'s flag and nothing else, so the '
                 'condition can be proven right before it is given authority to stop the engine. The Trip '
                 'lamp beside each row is that flag.\n\n'
                 'There is no debounce. The VM is stateless by design — no timers, no latches — so a '
                 'condition true for one frame cuts for one frame, and a noisy signal wants margin in the '
                 'condition rather than a tighter limit.\n\n'
                 'A condition that cannot be evaluated is DISARMED and raises a config fault: a '
                 'protection whose question is broken must not be allowed to answer "cut".')
    slot_h = A.Page.wrapped_h(slot_text, 330)
    note = p.panel(920, y0, 350, A.panel_h(1, slot_h + A.ROW + 10, top=10, bottom=8), 'Writing One')
    p.wrapped(10, 10, 330,
              slot_text,
              into=note)
    p.add(p._new('label', 10, slot_h + 18, 330, A.LBL_H,
                 {'labelText': 'Engine Protection', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': PROT}), into=note)
    return p
