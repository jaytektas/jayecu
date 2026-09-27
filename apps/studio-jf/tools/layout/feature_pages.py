"""A page per feature module: the switch, its settings, and what it is doing right now.

Every one of these is reachable from the search box whether or not the feature is on, so the page must
carry its OWN enable — otherwise a user who finds it has no way to turn the thing on from the place that
describes it. That is the shape the Throttle Body A page already uses: the switch first, at the top left,
and everything it governs greyed while it is off.

Field LABELS and UNITS come from the meta rather than being retyped here: the schema already names them,
and a page that renames a field is a page that disagrees with its own help text.
"""
import json
import author as A
from author import C_DIM

_META = None


def meta():
    global _META
    if _META is None:
        s = open(__import__('paths').META, encoding='utf-8', errors='replace').read()
        _META = json.JSONDecoder().raw_decode(s, 0)[0]
    return _META


def _fld(mod, name):
    e = meta()['config'][mod].get(name, {})
    return e.get('label', name), e.get('units', ''), e.get('kind', '')


# A SETTING THE FIRMWARE HAS STOPPED READING MUST SAY SO. Each entry is a field whose code is behind a
# mode — a switch, an enum, an input that is not assigned — so the row greys instead of taking a value
# nobody consults. Kept as a table rather than a ladder of `if` because tools/gate_audit.py finds these
# by reading the firmware, and what it finds lands here.
FIELD_GATES = {
    # Bank 2 needs a bank 1: it is also the banked/unbanked switch, so setting it while the first
    # source is Disabled asks for two loops when there is not yet one.
    'lambda.o2_src_2':                 '[#lambda.o2_src_1] != 0',
    # Target Voltage divides rich from lean and Warm Voltage is a Nernst cell's readiness test; neither
    # means anything to a wideband, which measures lambda and takes its target from the lambda table.
    'lambda.nb_target_mv':             '[#lambda.o2_src_1] > 0 and [#lambda.o2_src_1] < 3',
    'lambda.nb_warm_mv':               '[#lambda.o2_src_1] > 0 and [#lambda.o2_src_1] < 3',
    # The axle only decides something when the WHEELS are the driven source; with a gearbox pickup the
    # driven speed comes through the diff and the axle is never consulted.
    'traction_control.driven_axle':    '[#traction_control.driven_src] == 0',
    # THE STAGGER ONLY EXISTS WITH TWO CUTS. Launch.cpp reads cut_adder_rpm and cut_lead inside
    # `method == CutBoth`, and the lead has nothing to lead until the adder is non-zero.
    'launch.cut_adder_rpm':            '[#launch.cut_method] == 2',
    'launch.cut_lead':                 '[#launch.cut_method] == 2 and [#launch.cut_adder_rpm] != 0',
    # The soft band and the hard cut's hysteresis are the two halves of one enum: each is read on
    # exactly one branch of cut_type (Launch::cut_channel).
    'launch.cut_range_rpm':            '[#launch.cut_type] == 1',
    'launch.resume_band_rpm':          '[#launch.cut_type] == 0',
    # MODE PICKS WHICH CAMS RUN AT ALL. VvtControl.cpp:67 — intake loops are active in Intake and in
    # Intake+Exhaust, exhaust loops in Exhaust and Intake+Exhaust; an inactive loop resets and publishes
    # zero without reading one of its settings. So half of this page is dead in either single-cam mode.
    'vvt_control.intake_duty_min':              '[#vvt_control.mode] != 1',
    'vvt_control.intake_duty_max':              '[#vvt_control.mode] != 1',
    'vvt_control.intake_direction':             '[#vvt_control.mode] != 1',
    'vvt_control.intake_dead_band':             '[#vvt_control.mode] != 1',
    'vvt_control.intake_overall_corr':          '[#vvt_control.mode] != 1',
    'vvt_control.exhaust_duty_min':             '[#vvt_control.mode] != 0',
    'vvt_control.exhaust_duty_max':             '[#vvt_control.mode] != 0',
    'vvt_control.exhaust_direction':            '[#vvt_control.mode] != 0',
    'vvt_control.exhaust_dead_band':            '[#vvt_control.mode] != 0',
    'vvt_control.exhaust_overall_corr':         '[#vvt_control.mode] != 0',
    # The learned trim's gains and authority are dead while the learning is switched off — and the
    # per-cam gains are dead again in the mode that does not run that cam.
    'vvt_control.intake_ltt_gain':     '[#vvt_control.enable_ltt] == 1 and [#vvt_control.mode] != 1',
    'vvt_control.exhaust_ltt_gain':    '[#vvt_control.enable_ltt] == 1 and [#vvt_control.mode] != 0',
    'vvt_control.ltt_authority_pct':   '[#vvt_control.enable_ltt] == 1',
    # A bottle-pressure minimum with no pressure sensor assigned is a threshold against nothing.
    'nitrous.min_pressure_kpa':        '[#nitrous.pressure_src] >= 0',
    # CanBroker.cpp:66 — reconfigure_obd consumes the bus only `if (enabled)`, so choosing one while
    # OBD is off picks a bus for a responder that is never registered.
    'can.obd_bus':                     '[#can.obd_enabled] == 1',
}


def feature_page(title, mod, groups, live, blurb='', extra=None):
    """`groups`: [(panel title, [field names])] or [(panel title, [field names], gate)].
    `live`: [(label, channel, format)].

    A group's optional third element is its ENABLE EXPRESSION, and `None` means NOT GATED. Every group
    is gated on the module's own switch by default, which is right when the switch really does turn the
    whole feature off — and wrong the moment a module has two mechanisms with two switches. O2 Control
    is the case: `lambda.enabled` gates only the fast loop, and the firmware applies the learned surface
    whatever it says (Lambda.cpp: "Applied whenever enabled, conditions or not"), so gating the
    long-term group on it greyed out the only control for a correction that was still multiplying fuel.

    `extra(page, x, y, on)` is called after the scalar panels, for the one thing this shape cannot do:
    a feature whose settings include an ARRAY (per-sensor calibration, per-gear ratios) needs a panel
    laid out row by row, and there is no honest way to describe that as a list of field names."""
    p = A.Page(title=title)
    ON = f'[#{mod}.enabled] == 1'
    has_switch = 'enabled' in meta()['config'][mod]
    # The viewport's bar carries the name; this row carries what the feature IS and its own switch.
    # WHERE THE BLURB ACTUALLY ENDS. This threw the answer away and started the panels at the constant
    # A.TOP, so any blurb that wrapped past one line ran down through the first row of panels.
    top = p.head(blurb, enable=f'{mod}.enabled' if has_switch else '')

    # THREE ACROSS, THEN WRAP. A module with five groups of settings ran the fourth and fifth off the
    # right-hand edge — the row was assumed to be the only one.
    COLS = 3
    # `row_y`, not `y`: the field loop below walks `y` down inside the panel, and sharing the name meant
    # every panel started where the last one's fields had finished — a staircase instead of a row.
    x, row_y, tallest, bottom = 10, top, 0, top
    for i, (gt, fields, *rest) in enumerate(groups):
        gate = rest[0] if rest else ON        # None = this group is not gated by the module switch
        if i and i % COLS == 0:
            x, row_y = 10, row_y + tallest + 12
            tallest = 0
        # THE MODULE'S OWN SWITCH IS THE PAGE'S, NEVER A ROW. Listing 'enabled' in a group drew a
        # SECOND checkbox for the same flag, and the row gating below then handed it the condition
        # `[#mod.enabled] == 1` — a tick box disabled by its own value, which could switch itself off
        # and never back on. The page header already carries it.
        rows = [f for f in fields if f in meta()['config'][mod] and f != 'enabled']
        h = A.panel_h(len(rows), A.ROW, top=12, bottom=2)
        tallest = max(tallest, h)
        bottom = max(bottom, row_y + h)
        panel = p.panel(x, row_y, 400, h, gt)
        y = 12
        for f in rows:
            label, unit, kind = _fld(mod, f)
            # The field's own definition decides the control — a 0..1 flag is a tick, an enum a list,
            # anything else a number box. This builder is handed nothing but field NAMES, so it cannot
            # be told; nine settings across four features were spin boxes offering 0 and 1.
            ctl = A.Page.control_for(f'{mod}.{f}', kind)
            # A flag is never gated on ITSELF — that is a control that can only ever be turned off.
            # A MODULE WITHOUT A MASTER SWITCH still has per-field gates. `gate` defaults to the
            # module's own enable, which does not exist here — but FIELD_GATES below may still say
            # this field is dead in some mode, and that has nothing to do with whether the module can
            # be switched off. can.obd_bus is the case: no can.enabled, so every gate was discarded
            # and a bus could be chosen for a responder that is never registered.
            row_gate = '' if (f'{mod}.{f}' == f'{mod}.enabled' or not has_switch) else (gate or '')
            row_ranges = ''
            # A SOURCE THAT DOES NOT RESOLVE, or a banked pair split across a narrowband and a
            # wideband, is a configuration error the ECU sees on a config change — so both selectors
            # paint red from what it publishes, the same way the firing order does.
            if f'{mod}.{f}' in ('lambda.o2_src_1', 'lambda.o2_src_2'):
                row_ranges = '#ff453a,,,,0,[$o2_src_fault]'
            # NOT named `extra`: that is this function's own parameter, and shadowing it here turned
            # the array-panel callback into a string on every page that has one.
            field_gate = FIELD_GATES.get(f'{mod}.{f}')
            if field_gate:
                row_gate = ((row_gate + ' and ') if row_gate else '') + field_gate
            # AN EXPRESSION IS A SENTENCE, not a number: at a spin box's 110 px a condition like
            # `launch_sw and clt > 60` is three visible characters. It gets the room a condition needs
            # and the label gives some of its own back, which still fits the 400 px panel.
            ctl_w = 240 if ctl == 'expression' else 190 if ctl in ('enum', 'combobox') else 110
            y = p.field(panel, 10, y, label, f'{mod}.{f}', ctl, ctl_w, unit,
                        lbl_w=120 if ctl == 'expression' else 0,
                        enable=row_gate, ranges=row_ranges)
        x += 410

    # An array panel goes on its own ROW under the scalar groups — the loop above left x past the right
    # edge, and a second row is where the space is anyway.
    if extra:
        extra(p, 10, bottom + 12, ON if has_switch else '')

    if live and not extra:
        lp = p.panel(10, A.CANVAS_H - 106, 1260, 90, 'Live')
        for i, (lbl, ch, fmt) in enumerate(live):
            p.readout(lp, 14 + i * 150, 12, lbl, ch, fmt, w=140)
    return p


# One entry per module: how its settings group, and what to watch while tuning it. Ordered the way the
# feature is set up — what arms it, then the thresholds, then what it does when it fires.
FEATURES = [
    ('Launch Control', 'launch', 'Configuration/Vehicle Functions/Launch Control',
     [('Arming', ['arm_expr', 'timeout_s']),
      ('Cut', ['cut_method', 'cut_lead', 'cut_adder_rpm']),
      ('Holding', ['cut_type', 'cut_range_rpm', 'resume_band_rpm'])],
     [('Launch Active', 'launch_active', '%.0f'), ('End RPM', 'launch_end_rpm', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Cut', 'launch_cut_pct', '%.0f'),
      ('Launch Advance', 'launch_ign_adv', '%.1f'), ('Fuel Corr', 'fuel_corr_launch', '%.3f')],
     'Holds the engine at the End RPM while the arm condition is true, and runs the launch ignition '
     'and fuel maps for as long as it does. The limit and both maps are tables in the branch below; '
     'everything here is when it arms and how it holds.'),

    ('Deceleration Fuel Cut', 'dfco', 'Configuration/Engine Functions/Deceleration Fuel Cut',
     [('Entry', ['max_tps', 'rpm_high', 'min_clt']), ('Exit', ['rpm_low'])],
     [('DFCO Active', 'dfco_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Throttle', 'tps', '%.1f'), ('Coolant', 'clt', '%.0f'), ('Lambda', 'lambda_1', '%.2f')],
     'Stops injection on a closed throttle above an RPM, and resumes below a lower one.'),

    ('Flat Shift', 'flat_shift', 'Configuration/Vehicle Functions/Flat Shift',
     [('Arming', ['trigger_sig', 'min_rpm', 'min_tps']), ('Cut', ['cut_method', 'max_cut_ms'])],
     [('Shift Cut', 'shift_cut_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f'), ('Throttle', 'tps', '%.1f')],
     'Cuts on the clutch or gear-lever switch so the throttle can stay open through an upshift.'),

    ('Traction Control', 'traction_control', 'Configuration/Vehicle Functions/Traction Control',
     [('Measurement', ['driven_src', 'driven_axle', 'min_speed_kph']),
      ('Throttle Ceiling', ['kp', 'ki', 'cap_min_pct', 'release_pct_s']),
      ('Cut', ['cut_method', 'enable_expr'])],
     [('Slip', 'traction_slip', '%.1f'), ('Slip Error', 'traction_slip_err', '%.1f'),
      ('Throttle Cap', 'traction_cap', '%.0f'), ('Retard', 'traction_retard', '%.1f'),
      ('Cut', 'traction_cut_pct', '%.0f'), ('Vehicle Speed', 'vehicle_spd', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Throttle', 'tps', '%.1f')],
     'Slip is the faster driven wheel against the ROAD SPEED — whichever sensors Vehicle Speed names, '
     'so there is nothing to set here twice. Three paths answer it: the throttle ceiling holds a slip '
     'target, and the retard and cut curves catch what it cannot.'),

    ('Pit Speed Limiter', 'pit_limiter', 'Configuration/Vehicle Functions/Pit Speed Limiter',
     [('Arming', ['arm_sig']), ('Limit', ['speed_kph', 'hyst_kph', 'cut_method'])],
     [('Limiter Active', 'pit_limit_active', '%.0f'), ('Vehicle Speed', 'vehicle_spd', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f')],
     'Holds the car to a set road speed while the pit-lane switch is on.'),

    ('Nitrous', 'nitrous', 'Configuration/Engine Functions/Nitrous',
     [('Arming', ['arm_sig', 'min_rpm', 'max_rpm', 'min_tps']),
      ('Bottle', ['pressure_src', 'min_pressure_kpa']),
      ('Action', ['retard_deg'])],
     [('Nitrous Active', 'nitrous_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Throttle', 'tps', '%.1f'), ('Advance', 'advance', '%.1f'), ('Lambda', 'lambda_1', '%.2f')],
     'Arms the bottle above an RPM and throttle, and pulls timing while it is flowing.'),

    ('Water & Methanol', 'wmi', 'Configuration/Engine Functions/Water & Methanol',
     [('Entry', ['min_map_kpa', 'min_tps']), ('Delivery', ['full_map_kpa', 'min_duty_pct'])],
     [('WMI Active', 'wmi_active', '%.0f'), ('Pump Duty', 'wmi_duty', '%.0f'),
      ('MAP', 'map', '%.0f'), ('Throttle', 'tps', '%.1f'), ('Air Temp', 'iat', '%.0f')],
     'Ramps the pump between a start and a full-flow boost pressure.'),

    ('Anti-Lag', 'anti_lag', 'Configuration/Engine Functions/Anti-Lag',
     [('Arming', ['arm_sig', 'min_rpm', 'max_tps']), ('Action', ['retard_deg', 'max_time_ms'])],
     [('Anti-Lag Active', 'antilag_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Throttle', 'tps', '%.1f'), ('Advance', 'advance', '%.1f'), ('EGT 1', 'egt_1', '%.0f')],
     'Keeps the turbo spinning on a closed throttle. Hard on exhaust parts — mind the time limit.'),

    ('Variable Valve Lift', 'vvl', 'Configuration/Engine Functions/Variable Valve Lift',
     [('Switch Point', ['on_rpm', 'off_rpm']), ('Conditions', ['min_load_kpa', 'min_clt_c'])],
     [('VVL Engaged', 'vvl_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Fuel Load', 'fuel_load', '%.0f'), ('Coolant', 'clt', '%.0f')],
     'Switches the high-lift cam profile in and out, with hysteresis between the two RPMs.'),

    ('EGT Protection', 'egt_protect', 'Configuration/Protection/EGT Protection',
     [('Thresholds', ['enrich_c', 'cut_c']), ('Action', ['max_enrich_pct'])],
     [('EGT Protect', 'egt_protect_active', '%.0f'), ('EGT Corr', 'fuel_corr_egt', '%.3f'),
      ('EGT 1', 'egt_1', '%.0f'), ('Lambda', 'lambda_1', '%.2f')],
     'Enriches to cool the exhaust, then cuts if it keeps climbing.'),

    # Cruise Control is NOT here — it has its own builder (cruise_pages.py). Its inputs are eight
    # expressions and it owns three gain curves, neither of which a (panel, [field names]) tuple can
    # describe, and it needs a live strip AND a links panel, which feature_page() cannot emit together.

    ('Alternator Control', 'alternator', 'Configuration/Electrical/Alternator Control',
     [('Target', ['target_voltage', 'min_rpm']),
      ('Controller', ['kp', 'ki', 'max_duty_pct']),
      ('Behaviour', ['off_above_tps', 'soft_start_s'])],
     [('Field Duty', 'alternator_duty', '%.0f'), ('Battery', 'battery', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Throttle', 'tps', '%.1f')],
     'Drives the field coil to a target voltage, and backs off under throttle.'),

    ('Vehicle Speed', 'vehicle_speed', 'Configuration/Vehicle Functions/Vehicle Speed',
     [('Source', ['main_source', 'max_kph', 'shaft_ppr'])],
     [('Vehicle Speed', 'vehicle_spd', '%.1f'), ('Wheel FL', 'wheel_fl', '%.1f'),
      ('Wheel FR', 'wheel_fr', '%.1f'), ('Wheel RL', 'wheel_rl', '%.1f'),
      ('Wheel RR', 'wheel_rr', '%.1f'), ('Shaft', 'shaft_hz', '%.0f'),
      ('GPS', 'gps_hz', '%.0f')],
     'A pickup counts teeth, so pulses/km is the whole calibration: drive at the known speed and press '
     'Capture. The Main Source names the wheels it comes from, so nothing here has to be read against '
     'anything else — which axle the engine drives is traction control\'s question, and its setting.'),

    ('Gear Detection', 'gear_detect', 'Configuration/Vehicle Functions/Gear Detection',
     [('Setup', ['gear_count', 'tol_pct']), ('Validity', ['min_rpm', 'min_vss'])],
     [('Engine RPM', 'rpm', '%.0f'), ('Vehicle Speed', 'vehicle_spd', '%.0f')],
     'Works out the gear from the ratio of engine speed to road speed.'),

    ('Rev Limiter', 'rev_limiter', 'Configuration/Protection/Rev Limiter',
     [('Limits', ['soft_limit_rpm', 'hard_limit_rpm']), ('Action', ['cut_method', 'resume_band_rpm'])],
     [('Engine RPM', 'rpm', '%.0f'), ('RPM Before Cut', 'rpm_to_limit', '%.0f'),
      ('Fuel Cut', 'fuel_cut', '%.0f'), ('Ign Cut', 'ign_cut', '%.0f'),
      ('Rev Limit Corr', 'fuel_corr_revlimit', '%.3f')],
     'The soft limit pulls it back; the hard limit cuts.'),

    ('Lambda Protection', 'lambda_protect', 'Configuration/Protection/Lambda Protection',
     [('Trip', ['max_lambda', 'lean_margin_pct', 'min_tps', 'min_rpm']), ('Timing', ['timeout_ms'])],
     [('Protection Cut', 'lambda_protect_active', '%.0f'), ('Lambda', 'lambda_1', '%.2f'),
      ('Target', 'lambda_target', '%.2f'), ('Engine RPM', 'rpm', '%.0f'), ('Throttle', 'tps', '%.1f')],
     'Cuts if the mixture stays leaner than a limit under load — the last line before a hole in a piston.'),

    ('Transient Throttle', 'transient_throttle', 'Configuration/Fuel Tuning/Transient Throttle',
     [('Load Signal', ['load_source', 'tps_src', 'map_src']),
      ('Enrichment', ['enr_load_rate_db', 'enr_load_accel_db', 'enr_detect_ms']),
      ('Disenrichment', ['enable_disenrich', 'dis_load_rate_db', 'dis_load_decel_db', 'dis_detect_ms']),
      ('Decay & Async', ['enable_decay', 'enable_async', 'max_async_pulses', 'async_holdoff_ms']),
      ('Overall', ['enable_overall_corr', 'overall_corr_pct', 'ign_corr_decay_ms'])],
     [('Accel Corr', 'fuel_corr_accel', '%.3f'), ('Throttle', 'tps', '%.1f'),
      ('MAP', 'map', '%.0f'), ('Load Rate', 'tt_load_rate', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Inj PW', 'inj_pw', '%.2f')],
     'Fuel for a load that is CHANGING, which the steady-state tables cannot see: the dead bands decide '
     'what counts as a tip-in, the detect duration how long it must persist, and the decay how it goes '
     'away again. The tables that size it are listed below.'),

    ('Torque Model', 'torque_model', 'Configuration/Engine Functions/Torque Model',
     [('Reference', ['best_torque_lambda'])],
     [('Engine Torque', 'engine_torque_nm', '%.0f'), ('Advance', 'advance', '%.1f'),
      ('Lambda', 'lambda_1', '%.2f'), ('Lambda Torque', 'lambda_torque_ratio', '%.2f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'An estimate of the torque the engine makes, and what taking timing or mixture away costs it. '
     'It is for logging and the dash: nothing in the firmware acts on it yet.'),

    ('Misfire Detection', 'misfire', 'Configuration/Protection/Misfire Detection',
     [('Detection', ['threshold_pct', 'window_cycles', 'min_rpm', 'max_rpm']),
      ('Reporting', ['events_to_dtc', 'multi_cyl_for_p0300', 'cut_fuel']),
      ('Segment Learn', ['learn_enabled', 'learn_rate'])],
     [('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'),
      ('Active DTCs', 'dtc_active', '%.0f'), ('Lambda', 'lambda_1', '%.2f')],
     'A misfire is a cylinder that did not contribute: the crank slows where it should have sped up. The '
     'threshold is how much slower counts, and the segment learn is what cancels out a trigger wheel whose '
     'teeth are not perfectly even — without it an uneven wheel reads as a permanent misfire.'),

    ('O2 Control', 'lambda', 'Configuration/Fuel Tuning/O2 Control',
     [# THE SENSOR, and what the loop is aiming at. Target Voltage and Warm Voltage are narrowband-only
      # — a wideband measures lambda and takes its target from the lambda table — but they are shown
      # rather than hidden, because "this field does nothing for your sensor" is a thing the help text
      # can say and an absent control cannot.
      # o2_src_1 FIRST, because it is the question the rest of this panel answers to. It names a ROLE —
      # Overall, Bank 1, Cylinder 3 — and the wideband list under Sensors says which sensor holds it,
      # so re-plumbing is one edit there rather than two in different places. Target Voltage and Warm
      # Voltage below apply only when that role resolves to a narrowband.
      ('Sensor', ['o2_src_1', 'o2_src_2', 'nb_target_mv', 'nb_warm_mv', 'osc_amplitude',
                  'nb_stoich_band', 'nb_bias_pct']),
      # The GAINS are tables now (rpm x MAP), so they live on their own pages; what is left here is
      # the authority, which is the pair of numbers that decide how far this loop may go.
      # …and WHEN THE FAST LOOP MAY RUN: it holds (not resets) its trim through a cut, straight after
      # one, just after start and on a cold engine — a lean reading on overrun is not a mixture error.
      ('Short-Term Trim', ['stft_max_enrich', 'stft_max_disenrich',
                           'cl_start_delay_s', 'cl_after_cut_ms', 'cl_min_clt']),
      # UNGATED, because the firmware does not gate it: LTFT is applied on `ltft_enabled` alone, so
      # this group holds the switch and the limits of a correction that is live with the fast loop
      # stopped. That is not an oversight in the firmware — it is the second sentence of this page's
      # own blurb, and greying it out contradicted the text beside it.
      ('Long-Term Trim', ['ltft_enabled', 'ltft_gain',
                          'ltft_max_enrich', 'ltft_max_disenrich'], None),
      # Learning DOES need the fast loop (learn = stft_on && ltft_on && permit), so this one stays gated.
      # WHEN LEARNING IS ALLOWED. These govern only what gets BAKED IN — the fast loop still corrects
      # whenever it is on — so they are the difference between a trim surface that records the engine
      # and one that records the warm-up curve. Learn While is ANDed with them, never a replacement.
      ('Learn While', ['learn_min_clt', 'learn_min_rpm', 'learn_max_rpm', 'learn_run_time_s',
                       'learn_tps_rate_limit']),
      # Split from the group above only because ten rows overflow the canvas — these are the same gate.
      # The opt-in pair first, then the expression, which is ANDed with every condition above and is
      # the one place an engine-specific rule ("not while the WMI is spraying") can live.
      ('Learn Limits', ['learn_max_tps_en', 'learn_max_tps',
                        'learn_max_map_en', 'learn_max_map', 'ltft_learn_when'])],
     [('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f'),
      ('STFT', 'stft_pct', '%.1f'), ('LTFT', 'ltft_pct', '%.1f'),
      ('LTFT B1', 'ltft_bank_1_pct', '%.1f'), ('LTFT B2', 'ltft_bank_2_pct', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'Closed loop on the O2 sensor you name below. The short-term trim corrects the mixture now and is never stored; '
     'the long-term trim remembers where it kept having to, on the VE table\'s own grid, and is applied '
     'from the moment the engine starts. Two switches because they are two mechanisms: the learned '
     'surface is useful with the fast loop stopped, which is how you measure how wrong the tables are. '
     'There is no hidden rich margin: the surface stores exactly the correction it measured, and any '
     'richness you want for safety belongs in Target Lambda, where it is visible and indexed like any '
     'other target, not buried in a trim. Authority is the leash: a trim reaching 25% is hiding a VE table '
     'that is 25% wrong, and the answer is Apply to Base Table, not a longer leash.'),

    ('Cam Control', 'vvt_control', 'Configuration/Engine Functions/Cam Control',
     [('Setup', ['num_banks', 'mode', 'max_delta_rate']),
      ('Intake Solenoid', ['intake_duty_min', 'intake_duty_max', 'intake_direction',
                           'intake_dead_band', 'intake_overall_corr']),
      ('Exhaust Solenoid', ['exhaust_duty_min', 'exhaust_duty_max',
                            'exhaust_direction', 'exhaust_dead_band',
                            'exhaust_overall_corr']),
      ('Long-Term Trim', ['enable_ltt', 'ltt_authority_pct', 'intake_ltt_gain', 'exhaust_ltt_gain'])],
     [('Intake Angle', 'vvt_angle_1', '%.1f'), ('Intake Duty', 'vvt_duty_1', '%.1f'),
      ('Exhaust Angle', 'vvt_angle_2', '%.1f'), ('Exhaust Duty', 'vvt_duty_2', '%.1f'),
      ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f')],
     'A cam is a position loop: the target says where it should be, the gains decide how hard it is '
     'pushed, and the solenoid settings are what the hardware will actually accept.'),

    # THE BRANCHES THAT WERE FOLDERS. Each has a module behind it and had no page at all, so its settings
    # were reachable only through the dictionary.
    # KNOCK is not here any more: it has a family of its own (knock_pages.py). A generic settings page
    # cannot show which input hears which cylinder, the two decision tables, or the learned noise floor —
    # which is most of what knock control is.
    ('Idle Stepper', 'stepper', 'Configuration/Engine Functions/Idle Stepper',
     [('Driver', ['driver_mode', 'input_sig', 'dir_invert', 'microstep']),
      ('Travel', ['range_steps', 'max_step_per_update', 'step_period_ms']),
      ('Current', ['move_current_pct', 'hold_current_pct'])],
     [('Idle Duty', 'idle_duty', '%.0f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Idle Target', 'idle_target_rpm', '%.0f'), ('Coolant', 'clt', '%.0f')],
     'A stepper idle valve instead of a PWM one: the same idle control drives it, but position is counted '
     'in steps and the driver has to be told how many there are and how hard to hold them.'),

    ('Engine Protection', 'engine_protection', 'Configuration/Protection/Engine Protection',
     [('Sensors', ['sensor_timeout_ms', 'trigger_error_pct_limit']),
      ('Coolant', ['clt_warn_c', 'clt_cut_c']),
      ('Air & Boost', ['iat_cut_c', 'map_cut_kpa'])],
     [('Coolant', 'clt', '%.0f'), ('Air Temp', 'iat', '%.0f'), ('MAP', 'map', '%.0f'),
      ('Active DTCs', 'dtc_active', '%.0f'), ('Engine RPM', 'rpm', '%.0f')],
     'The last-resort limits: what the ECU does when a sensor stops answering, when the coolant or air '
     'gets too hot, or when the trigger starts losing teeth.'),

    # WHERE THE REST OF IT IS. Everything else about on-ECU logging — which channels are recorded, the
    # gate that starts and stops it, its minimum/maximum run and re-arm times, and what to do when the
    # gate cannot be answered — lives in Logging > Onboard Logging..., which edits them as one PROFILE.
    # Eight settings therefore have no control on any page, and that is right: a page duplicating a
    # dialog is two places to change one thing. What was missing is any hint from here that the dialog
    # exists, so this page named a rate and silently implied that was all there was.
    ('Datalogging', 'datalog', 'Configuration/Datalogging',
     [('Logging', ['rate_hz'])],
     [('Engine RPM', 'rpm', '%.0f'), ('Active DTCs', 'dtc_active', '%.0f')],
     'How fast the ECU writes to its own SD card. WHAT it records and WHEN it starts are a recording '
     'profile rather than settings, and they are edited together in Logging \u203a Onboard Logging\u2026 '
     '\u2014 the channel list, the start and stop conditions, the minimum and maximum run times, the '
     're-arm delay, and what to do when a condition cannot be answered. Activate there writes the whole '
     'profile to the ECU.'),

    # NOT UNDER COMMUNICATIONS. A script is not a bus: it reads any channel, publishes its own, and the
    # features downstream consume what it writes — the same shape as a generic table, which is why it sits
    # beside Generic Tables at the top of Configuration rather than filed under CAN and datalogging.
    ('Lua Scripting', 'lua', 'Configuration/Lua Scripting',
     [('Execution', ['max_exec_us'])],
     [('Engine RPM', 'rpm', '%.0f')],
     'A script of your own, running on the ECU: it can read any channel, publish its own, send CAN '
     'frames and raise DTCs. What you type is held until you press Apply to ECU \u2014 a half-typed line '
     'never reaches a running engine \u2014 and Burn keeps it in the tune like any other setting.'),
]


def vehicle_speed_calibration(p, x, y, on):
    """Pulses per unit distance, one row per pickup, in the order Main Source names them.

    This is the whole calibration: a pickup counts teeth, and how many of them make a kilometre depends
    on the tyre and the tooth count, which the ECU cannot know. Drive at a known speed — by a GPS, not by
    the speedometer being calibrated — and divide the live frequency by it. A GPS speedometer module is
    one of the pickups and calibrates identically; it is a pulse train, not somebody else's road speed.

    The box prints its own unit and it follows the unit system (pulses/km or pulses/mi), so no row here
    names a distance — a label saying "per km" beside a value shown per mile is the bug this avoids.
    """
    # …and the SPEED each one resolves to, beside the frequency it comes from. The page had the Hz and
    # the pulses/km and not the answer, so the one question a calibration page exists to settle — "does
    # this row now read the speed I am doing?" — had to be taken somewhere else to be asked. Every
    # pickup publishes its own speed for this (VehicleSpeed), whether or not it is the Main Source.
    rows = [('Drive Shaft', 'shaft_hz', 'shaft_spd'), ('Front Left', 'wheel_hz_fl', 'wheel_fl'),
            ('Front Right', 'wheel_hz_fr', 'wheel_fr'), ('Rear Left', 'wheel_hz_rl', 'wheel_rl'),
            ('Rear Right', 'wheel_hz_rr', 'wheel_rr'), ('GPS', 'gps_hz', 'gps_spd')]
    panel = p.panel(x, y, 440, 40 + A.ROW * len(rows), 'Pickup Calibration', enable=on)
    ry = 12
    for i, (label, chan, _spd) in enumerate(rows):
        ry = p.field(panel, 10, ry, label, f'vehicle_speed.source[{i}].pulses_per_km',
                     'configedit', 190, 'pulses/km', enable=on)
    # …and the CAPTURE, which is the whole procedure in one panel: say what speed you are holding, watch
    # the pickups' live frequencies, press Capture once. Each pulses/km is that pickup's frequency times
    # 3600 divided by that speed — the sum anyone would otherwise do on a phone and type in.
    #
    # ONE BUTTON, NOT ONE PER PICKUP. Six buttons are six different moments: pressed a few seconds apart,
    # the front and rear calibrations disagree by however much the car's speed drifted in between — and
    # traction control divides one axle by the other, so it reads that disagreement as permanent slip on a
    # car doing nothing wrong. The pickups that are not turning are simply not written (the widget skips a
    # write that comes out zero), so the same button serves a car with one pickup and a car with six.
    #
    # THE KNOWN SPEED LIVES HERE, not in a settings group across the page. It is not a setting the ECU
    # reads — nothing in firmware touches calibration_speed — it is this button's other operand, and a
    # number you have to go and find is a step in a procedure that should have none.
    live = p.panel(x + 450, y, 620, 78 + A.ROW * (len(rows) + 1),
                   'Capture  ·  drive at a known speed', enable=on)
    # lbl_w IS STATED, because this row shares its line with the button. Left to itself the name takes
    # everything the panel has left over (author.Page.field), which put a 120px box at x=356 — directly
    # underneath a Capture button spanning 310..480. Nothing said so: check_all was building feature pages
    # without their extra panel, so this whole panel was checked by nobody.
    ly = p.field(live, 10, 12, 'Known Vehicle Speed', 'vehicle_speed.calibration_speed',
                 'configedit', 130, 'km/h', enable=on, lbl_w=150)
    p.add(p._new('panel', 10, ly + 2, 600, 1, {'bgColor': C_DIM, 'padding': '0'}), into=live)
    ly += 12
    for cx, cw, text in ((165, 135, 'Reading'), (500, 110, 'Speed')):
        p.add(p._new('label', cx, ly, cw, 16,
                     {'labelText': text, 'align': 'Right' if cx > 400 else 'Left',
                      'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=live)
    ly += 18
    for i, (label, chan, spd) in enumerate(rows):
        g = p.group()
        p.add(p._new('label', 10, ly + 4, 150, A.LBL_H,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_LBL}, g), into=live)
        p.add(p._new('value', 165, ly, 100, A.CTL_H,
                     {'signalName': chan, 'format': '%.1f', 'showUnit': '0', 'align': 'Right'}, g), into=live)
        p.add(p._new('label', 270, ly + 4, 30, A.LBL_H,
                     {'labelText': 'Hz', 'align': 'Left', 'fontName': A.FONT_LBL,
                      'fgColor': C_DIM}, g), into=live)
        # WHAT IT RESOLVES TO. Its own unit, so it follows km/h or mph like every other speed — and it
        # reads nothing at all until that row's pulses/km is set, which is the row telling you why.
        p.add(p._new('value', 500, ly, 110, A.CTL_H,
                     {'signalName': spd, 'format': '%.1f', 'align': 'Right'}, g), into=live)
        ly += A.ROW
    # Greyed until there is a speed to divide by: every write is Hz x 3600 / speed, and at speed 0 that is
    # a division by zero going into every pickup's calibration at once.
    speed_set = '[#vehicle_speed.calibration_speed] > 0'
    p.action(live, 310, 12, 170, 'Capture All',
             [(f'vehicle_speed.source[{i}].pulses_per_km',
               f'[${chan}] * 3600 / [#vehicle_speed.calibration_speed]')
              for i, (_lbl, chan, _s) in enumerate(rows)],
             enable=f'({on}) and {speed_set}' if on else speed_set)


def array_grid(mod, arr, title, count, cols, note='', vis=None, ranges=None, enable_of=None):
    """A feature's ARRAY, as a grid: one row per element, one column per field.

    The common feature page is a list of scalars, so an array of anything simply did not appear —
    the CAN buses, the wideband scopes, the idle-up requests and the gear ratios were all in the
    config with nowhere to be typed. A grid is the honest shape for them: the fields are the same
    for every element, and it is the ELEMENTS that differ.

    `cols` is [(field, header, width, kind)] — optionally with a fifth element, a gate for that COLUMN
    written against `{e}`, the element's own path. A per-ROW gate would grey the switch you turn the
    element on with; a per-column one leaves that live and greys what the firmware stops reading when
    it is off. `vis` is an optional (index -> condition) for rows that only exist on some engines.
    """
    def extra(p, x, y, on):
        from ruler import ruler
        e = f'{mod}.{arr}'
        # A COLUMN IS AS WIDE AS THE WIDER OF ITS CONTROL AND ITS HEADER. It used to be the control's
        # width flat, so a checkbox column declared 30px wide carried a header of "Listen Only" — 67px of
        # text in a 30px box, with "Listen O" on screen and nothing to say the rest was there. The
        # declared width still sets the CONTROL; the header only ever widens the column.
        # (+8: a caption is drawn one 3px inset in from each edge of its box.)
        widths = [max(c[2], int(ruler().width(c[1], 15)) + 8) for c in cols]
        w = 70 + sum(cw + 10 for cw in widths)
        panel = p.panel(10, y, w, A.panel_h(count + 1, 28, top=34, bottom=8), title, enable=on)
        p.add(p._new('label', 10, 8, 60, 18,
                     {'labelText': '#', 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=panel)
        cx = 70
        for c, colw in zip(cols, widths):
            p.add(p._new('label', cx, 8, colw, 18,
                         {'labelText': c[1], 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
                  into=panel)
            cx += colw + 10
        p.add(p._new('panel', 10, 28, w - 20, 1, {'bgColor': C_DIM, 'padding': '0'}), into=panel)
        ry = 34
        for i in range(count):
            cond = vis(i) if vis else ''
            # A ROW MAY BE INERT WITHOUT BEING ABSENT. `vis` HIDES a row; this GREYS one, which is what
            # a per-element switch wants: the fifteen slots are the fixed shape of the hardware, so a
            # row for a sensor that is switched off should be visibly there and visibly not in play.
            # Hiding it would make the list change length as sensors come and go, and you could no
            # longer tell "sensor 7 is off" from "there is no sensor 7".
            row_on = enable_of(i) if enable_of else ''
            g = p.group()
            p.add(p._new('label', 10, ry + 4, 60, A.LBL_H,
                         {'labelText': str(i + 1), 'align': 'Left', 'fontName': A.FONT_LBL,
                          **({'condition': cond} if cond else {}),
                          **({'enableCondition': row_on} if row_on else {})}, g), into=panel)
            cx = 70
            for c, colw in zip(cols, widths):
                f, cw, kind = c[0], c[2], c[3]
                path = f'{e}[{i}].{f}'
                k = A.Page.control_for(path, kind)
                col_on = c[4].format(e=f'{e}[{i}]') if len(c) > 4 else ''
                gate = ' and '.join(x for x in (row_on, col_on) if x)
                props = {'signalName': path, **({'condition': cond} if cond else {}),
                         **({'enableCondition': gate} if gate else {}),
                         **({'ranges': ranges} if ranges else {})}
                if k == 'checkbox':
                    p.add(p._new('checkbox', cx, ry + 2, A.CHECK_H, A.CHECK_H, props, g), into=panel)
                else:
                    p.add(p._new(k, cx, ry, cw, A.CTL_H, props, g), into=panel)
                cx += colw + 10
            ry += 28
        if note:
            p.note(10, y + A.panel_h(count + 1, 28, top=34, bottom=8) + 6, note, w=w - 10)
    return extra


def both(*extras):
    """Two extra panels on one page — a feature can have an array AND its branch of tables."""
    def extra(p, x, y, on):
        for e in extras:
            e(p, x, y, on)
    return extra


# The features whose settings include an ARRAY, and the panel that lays it out. Kept beside FEATURES
# rather than inside it so the common row stays six fields wide.
def misfire_learned_segments(p, x, y, on):
    """The twelve learned per-cylinder segment corrections, as READOUTS.

    These are not settings. The schema puts them in the LEARNED segment — "runtime RAM, persisted to
    SD; not part of the tune" — so the ECU writes them and a tune neither carries nor restores them.
    They were reported as twelve configuration fields with nowhere to be edited, which is true and the
    wrong conclusion: an editor for them would be a control that the next engine cycle overwrites.

    Worth SEEING, though, and that is the gap this closes. The correction is what cancels out a trigger
    wheel whose teeth are not perfectly even, so a cylinder sitting far from its neighbours is either a
    wheel fault or a real mechanical difference — and until now there was nowhere to look.
    """
    pan = p.panel(x, y, 1260, A.panel_h(2, A.ROW, top=8) + 6,
                  'Learned Segment Correction  (per cylinder, learned — not part of the tune)',
                  enable=on)
    for c in range(12):
        col, row = c % 6, c // 6
        # readout() takes a CHANNEL; these are config paths in the learned segment, so the row is the
        # same two widgets built directly — a name and a value that cannot be typed into.
        g = p.group()
        p.add(p._new('label', 14 + col * 205, 12 + row * A.ROW, 56, A.LBL_H,
                     {'labelText': f'Cyl {c + 1}', 'align': 'Left', 'fontName': A.FONT_LBL,
                      'condition': f'[#engine.cylinder_count] >= {c + 1}'}, g), into=pan)
        p.add(p._new('value', 74 + col * 205, 8 + row * A.ROW, 120, A.CTL_H,
                     {'signalName': f'[#misfire.misfire_seg_{c + 1}]', 'align': 'Center',
                      'fontName': A.FONT_LBL, 'format': '%.2f',
                      'condition': f'[#engine.cylinder_count] >= {c + 1}'}, g), into=pan)


EXTRA_PANELS = {'vehicle_speed': vehicle_speed_calibration,
                'misfire': misfire_learned_segments}


# The tree's children per branch, filled in by the installer before any page is built. A link has to name
# a node that EXISTS, and the schema's table labels are only sometimes what the node is called: the
# transient-throttle branch calls its tables "Enrich Rate" where the schema says "Transient Enrichment
# Rate". Asking the tree is the only way to be right about that, and the link check refuses the write when
# it is not — which is how this was found.
TREE_CHILDREN = {}


def branch_tables_panel(mod, branch, title='Tables'):
    """An extra panel listing the branch's tables as links — so a settings page for a branch is also the
    way into the maps that belong to it, rather than the tree being the only route."""
    def extra(p, x, y, on):
        labels = TREE_CHILDREN.get(branch) or [
            e.get('label') or k
            for k, e in meta()['config'][mod].items()
            if isinstance(e, dict) and e.get('type') == 'table'
            and not k.endswith(('_axis', '_yaxis'))]
        if not labels:
            return
        COLS = 3
        per = -(-len(labels) // COLS)
        for c in range(COLS):
            chunk = labels[c * per:(c + 1) * per]
            if not chunk:
                continue
            panel = p.panel(10 + c * 415, y, 400, A.panel_h(len(chunk), 24, top=10, bottom=4),
                            title if c == 0 else '', enable=on)
            ry = 10
            for lb in chunk:
                # 356, not 360: a panel hands its children w - 4, so a 400px panel's usable width is 396
                # and a row starting at x=8 gets 388. This was two pixels over for as long as nobody
                # checked an extra panel — see check_all.py, which now builds them.
                ry = p.switch(panel, 8, ry, lb, '', link=f'{branch}/{lb}', w=356, pitch=24)
    return extra


CAN_BUSES = array_grid(
    'can', 'bus', 'Buses', 2,
    # PlatformCan.cpp:22 — `if (!enabled) { shutdown(); return; }`, so a bus that is off reads neither
    # its bitrate nor its listen-only flag. The switch itself stays live, or there is no way back on.
    [('enabled', 'Enabled', 30, ''),
     ('bitrate', 'Bitrate', 150, 'enum', '[#{e}.enabled] == 1'),
     ('listen_only', 'Listen Only', 30, '', '[#{e}.enabled] == 1')],
    'Listen Only never transmits — not even an acknowledgement — so the ECU can be put on somebody '
    'else\'s bus without affecting it. A bus that is off is not initialised at all. What each bus '
    'CARRIES is on its own Generic CAN page, and a frame there can be parked without being deleted.')

# RED WHILE TWO SENSORS CLAIM ONE JOB. The ECU marks its own homework — it sweeps the assignments on
# every config change and says so on o2_assign_fault — so this is the same rule the firing order uses
# (bg,fg,accent,border,blink,when) rather than the studio re-deriving a constraint the firmware has to
# enforce anyway.
WIDEBAND_SCOPE = array_grid(
    'lambda', 'wb', 'Wideband Scope', 15,
    [('assign', 'Assignment', 190, 'enum')],
    'Unassigned means the sensor is read and logged but drives nothing — the right answer for a '
    'reference probe. One sensor per job.',
    ranges='#ff453a,,,,0,[$o2_assign_fault]',
    # A ROW IS LIVE ONLY IF ITS SENSOR IS. Assigning a job to a wideband that is switched off is not a
    # setting, it is a note to yourself — and it would sit in the config looking like a configuration.
    # wb[i] is the i'th wideband in catalogue order, which is lambda_(i+1).
    enable_of=lambda i: f'[#sensors.sensor[lambda_{i + 1}].enabled] == 1')

GEAR_RATIOS = array_grid(
    'gear_detect', 'gear_ratio', 'Ratios', 8,
    # The header does not name the unit and the box does: the ratio converts with the speed underneath
    # it, so a column headed "RPM per kph" over a value shown per mph would be a caption contradicting
    # the number beside it. 170px because the suffix lives INSIDE the control.
    [('rpm_per_kph', 'Ratio', 170, '')],
    'Engine RPM divided by road speed, per gear. Measure each one at a steady cruise rather than '
    'calculating it: the number that matters includes the tyres actually fitted.')

def lua_script_panel(p, x, y, on):
    """THE EDITOR ITSELF — this page is the script, so the script is on it.

    A `script` element: ScriptWidget hosting LuaScriptEditor. It holds what you type until Apply, which
    is the whole reason a script is not an ordinary bound field — the ECU live-reloads what it is given,
    and half a line of Lua is a syntax error in a running engine.

    It used to be a `text` element with `multiline: 1`. That is a jf::JLineEdit, a SINGLE-LINE control
    which ignored the flag and was then stretched to 1240x800, so four kilobytes of Lua rendered as one
    unreadable band — and it wrote the field on every keystroke. Then it was a note pointing at the dock,
    which left a page called Lua Scripting with no script on it.

    Gated with the rest of the page: with the module switched off the firmware does not read `source` at
    all, and this page greys what the firmware has stopped reading — tick Enabled at the top to write one.
    """
    h = A.CANVAS_H - y - 10
    panel = p.panel(10, y, 1260, h, 'Script', enable=on)
    # PanelWidget gives its children `h - titleBarH() - 4`, so the editor is the panel less its bar, its
    # 12px top inset and a matching one at the bottom — not `h` less a guessed title height.
    p.add(p._new('script', 10, 12, 1240, h - (A.PANEL_TITLE + 4) - 12 - 12, {}), into=panel)


EXTRA_PANELS['lua'] = lua_script_panel
# The buses have pages of their own; CAN Bus keeps OBD-II.
EXTRA_PANELS['gear_detect'] = GEAR_RATIOS


def page_wideband_scope():
    """Which exhaust each wideband is smelling.

    Its own page rather than a panel on O2 Control: fifteen rows do not fit under a branch that already
    lists a dozen tables, and this is a question you answer once per installation rather than while
    tuning.
    """
    p = A.Page(title='Wideband Scope')
    y0 = p.head('WHICH exhaust each wideband is smelling, and what the closed loop resolves: O2 '
                'Control names a ROLE, and the sensor holding it is the one the loop listens to. '
                'A role belongs to ONE sensor — assign it twice and the rows turn red.',
                enable='lambda.enabled')
    WIDEBAND_SCOPE(p, 10, y0, '[#lambda.enabled] == 1')

    live = p.panel(700, y0, 570, 320, 'Right Now')
    for n, (lbl, ch) in enumerate((('Lambda (overall)', 'lambda_1'), ('Target', 'lambda_target'),
                                   ('STFT', 'stft_pct'), ('LTFT', 'ltft_pct'))):
        p.readout(live, 15 + (n % 2) * 280, 14 + (n // 2) * 70, lbl, ch, A.Page.chan_fmt(ch), 260)
    p.wrapped(15, 160, 540,
              'A bank number is 1 or 2; a cylinder number is 1 to 12, and it is the '
                               'CYLINDER, not the firing position.\n\n'
                               'A sensor the loop is not pointed at still reads and logs — only the '
                               'role named on O2 Control drives the mixture.',
              into=live)
    p.add(p._new('label', 15, 262, 300, A.LBL_H,
                 {'labelText': 'O2 Control', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': 'Configuration/Fuel Tuning/O2 Control'}), into=live)
    return p


# Pages that belong to a feature's branch but are not the feature's own page. Installed exactly like a
# feature page (the installer makes the node when the tree has none), which is what lets an array of
# fifteen elements have the room it needs.
EXTRA_PAGES = {'Configuration/Fuel Tuning/O2 Control/Wideband Scope': page_wideband_scope}

def page_generic_can(bus: int, transmit: bool):
    """One bus, one direction, on its own page.

    The page knows which bus and which way round it is by BEING that page, so neither is a control:
    a Bus picker and a direction switch would re-ask what the tree already answered, and add two more
    states to get out of step with it. Receive and transmit are opposite jobs — one arrives and
    becomes a signal, the other is unsolicited traffic on somebody else's wire — so they are separate
    pages rather than two lists sharing one.
    """
    name = f'CAN{bus + 1}'
    what = 'Transmit' if transmit else 'Receive'
    p = A.Page(title=f'{name} {what}')
    if transmit:
        blurb = (f'Frames this ECU SENDS on {name}, each on its own period, built from whatever the '
                 f'signal bus is carrying at the time. A transmit frame is unsolicited traffic \u2014 '
                 f'it goes on a wire that may already belong to something else \u2014 so a frame can be '
                 f'parked with its Enabled tick without losing how it was set up.')
    else:
        blurb = (f'Frames this ECU DECODES on {name}. Each signal lands on the bus with a TTL, so a '
                 f'sender going quiet leaves an absent channel rather than a number frozen at its last '
                 f'value. A signal left as \u201cnone\u201d is one a SENSOR reads: point that sensor\'s '
                 f'CAN Field at the field and it publishes the channel with its own calibration and '
                 f'diagnostics.')
    y0 = p.head(blurb + f' {name} must be enabled on its own page (CAN Bus ▸ {name}); edits live in ECU RAM until you burn.')
    p.add(p._new('genericcan', 10, y0, A.CANVAS_W - 20, A.CANVAS_H - y0 - 10,
                 {'bus': str(bus), 'dir': what}))
    return p


def page_obd(_=None):
    """OBD-II: what a scan tool gets when it asks.

    Its own page because it is its own thing — a responder to a standard nobody here chose, on one
    bus, answering questions somebody else's tool asks. It used to share a page with the physical
    buses, which is how the switchboard ended up with a row called "CAN Bus" that toggled this.
    """
    p = A.Page(title='OBD-II')
    y0 = p.head('A generic scan tool plugged into the car talks to this. It ANSWERS \u2014 mode 01 live '
                'data, mode 03 stored codes \u2014 and says nothing unprompted, which is the opposite of '
                'everything under CAN1 and CAN2. Independent of them: it can sit on a bus that also '
                'carries your own frames, or on one of its own.')
    box = p.panel(10, y0, 520, 200, 'OBD-II')
    ry = p.field(box, 10, 12, 'Enabled', 'can.obd_enabled', 'configedit', 60, lbl_w=110)
    ry = p.field(box, 10, ry + 4, 'Bus', 'can.obd_bus', 'enum', 150, lbl_w=110,
                 cond='[#can.obd_enabled] == 1')
    p.wrapped(10, ry + 10, 500,
              'Put the scan tool on a bus of its own and it cannot see, or interfere with, the traffic '
              'on the other one \u2014 which on a tuning or instrument bus is usually what you want. The '
              'bus it names must itself be enabled.', into=box, colour=C_DIM)
    return p


def page_can_branch(_=None):
    """CAN Bus: the branch, and what is under it.

    It is not a thing with a switch. It groups the scan-tool responder and the two physical
    controllers, and each of those has its own enable on its own page — a switch here would have to
    mean one of theirs, which is exactly the confusion it used to cause.
    """
    # A branch's own page is a list of what is under it — the same shape every other branch uses, so
    # it reads as "this one is only a heading" rather than as a link that went nowhere.
    rows = [('CAN1',   'Configuration/CAN Bus/CAN1'),
            ('CAN2',   'Configuration/CAN Bus/CAN2'),
            ('OBD-II', 'Configuration/CAN Bus/OBD-II')]
    p = A.Page(title='CAN Bus')
    y = p.head('Two physical CAN controllers and the OBD-II responder. Each bus has a page for the '
               'wire itself \u2014 bit rate, listen-only, and what it is doing right now \u2014 with the '
               'frames it sends and decodes underneath it. There is no switch here: each of the three '
               'has its own, on its own page.')
    box = p.panel(10, y, 320, A.panel_h(len(rows), 25, top=12, bottom=4), 'In this branch')
    ry = 12
    for label, link in rows:
        ry = p.switch(box, 10, ry, label, '', link=link)
    return p


def page_can_bus(bus: int):
    """One physical bus: how it is set up, and what it is actually doing.

    Its settings and its health belong together — "the wire is quiet" and "the wire is at the wrong
    bit rate" are answered by the same two panels, and reading one on a page and the other under
    Diagnostics is how an afternoon goes into a bus that was simply switched off. The frames it
    carries are its children, one page per direction.
    """
    name = f'CAN{bus + 1}'
    e = f'can.bus[{bus}]'
    p = A.Page(title=name)
    y0 = p.head(f'{name} is a physical CAN controller. Nothing here is about what the bus CARRIES '
                f'\u2014 that is Transmit and Receive below it \u2014 this is the wire itself: whether '
                f'it is brought up, how fast, and whether the ECU is allowed to talk on it.')

    # AS TALL AS WHAT IS IN IT. This height was hand-tuned twice — 250, then 272 when the note grew
    # a sentence about the rate locking — and the note has since outgrown 272 as well: its last line
    # sat 8px past the content box, drawn but clipped. Guessing it again would only move the next
    # failure. Page.wrapped_h() answers exactly what wrapped() will draw, so the panel is measured
    # from its own contents below, once the rows that precede the note are placed.
    box = p.panel(10, y0, 430, 272, 'Setup')
    ry = p.field(box, 10, 12, 'Enabled', f'{e}.enabled', 'configedit', 60, lbl_w=110)
    # THE BIT RATE LOCKS ONCE THE BUS CARRIES SOMETHING. Every frame on a bus was added for the rate
    # the bus was at; moving it afterwards breaks all of them at once and reports nothing, because a
    # controller at the wrong rate simply never ACKs — which reads as nothing being plugged in. While
    # the bus is empty it is the user's to set, template or no template; the first template loaded
    # onto an empty bus sets it to match, and says so.
    # GREYED, NOT HIDDEN: `cond` is visibility and `enable` is the lock. A rate the bus is committed
    # to is exactly the number the user needs to READ while being told they cannot change it — hiding
    # the row would take the answer away with the control.
    ry = p.field(box, 10, ry + 4, 'Bitrate', f'{e}.bitrate', 'enum', 150, lbl_w=110,
                 cond=f'[#{e}.enabled] == 1', enable=f'canframes({bus}) == 0')
    ry = p.field(box, 10, ry + 4, 'Listen Only', f'{e}.listen_only', 'configedit', 60, lbl_w=110,
                 cond=f'[#{e}.enabled] == 1')
    setup_note = (
        'Listen Only never transmits \u2014 not even an acknowledgement \u2014 so the ECU can be '
        'put on somebody else\'s bus without affecting it. A bus that is off is not initialised '
        'at all. Every node on a bus must agree on the bit rate: a mismatch never ACKs, which '
        'looks exactly like nothing being connected \u2014 which is why the rate is fixed once '
        'this bus carries a frame. Clear its Transmit and Receive lists to change it.')
    NOTE_W = 410
    p.wrapped(10, ry + 10, NOTE_W, setup_note, into=box, colour=C_DIM)
    # The note's real bottom, plus the title bar and the bottom padding check() measures against.
    box['h'] = ry + 10 + A.Page.wrapped_h(setup_note, NOTE_W) + A.PANEL_TITLE + 8

    # WHAT THE CONTROLLER KNOWS ABOUT THE WIRE. A CAN bus does not fail by going quiet, it fails by
    # counting errors — so these, not a frame count, are what says a bus is unhealthy.
    # THE LAST ERROR GETS ITS OWN ROW, because its reading is a sentence. The controller's error names
    # say what the fault IS ("Bit recessive - something driving against us"), which is the useful half
    # of the answer and 380px of text; in a third of the panel it was guillotined mid-word. This is the
    # one page with room to print it whole, so it prints it whole and the dense Diagnostics card shows
    # the same channel shrunk.
    _CAN_NOTE = ('A transmit error count above 127 is error-passive and 255 is bus-off; Refused counts '
                 'frames the driver would not take, which is a jammed mailbox rather than a quiet wire. '
                 'A bus reading zero load with errors climbing is talking to nobody.')
    # 272 = the note's own top (238) plus the panel's title bar and its bottom padding. Sized rather
    # than guessed: the fourth row of readouts pushed the note 20px past the content box.
    live = p.panel(456, y0, 430, 272 + A.Page.wrapped_h(_CAN_NOTE, 410), 'Right Now')
    p.readout(live, 14, 14, 'Bus Load',  f'[${name.lower()}_load_pct]', '%.1f', 120)
    p.readout(live, 150, 14, 'Sent',     f'[${name.lower()}_tx_fps]',   '%.0f', 120)
    p.readout(live, 286, 14, 'Received', f'[${name.lower()}_rx_fps]',   '%.0f', 120)
    p.readout(live, 14, 70, 'State',     f'[${name.lower()}_state]',    '%.0f', 120)
    p.readout(live, 150, 70, 'TX Errors',f'[${name.lower()}_tec]',      '%.0f', 120)
    p.readout(live, 286, 70, 'RX Errors',f'[${name.lower()}_rec]',      '%.0f', 120)
    p.readout(live, 14, 126, 'Bus-Off',  f'[${name.lower()}_bus_off]',  '%.0f', 120)
    p.readout(live, 150, 126, 'Refused', f'[${name.lower()}_tx_fail]',  '%.0f', 120)
    p.readout(live, 14, 182, 'Last Error', f'[${name.lower()}_last_err]', '%.0f', 396)
    p.wrapped(14, 238, 410, _CAN_NOTE, into=live, colour=C_DIM)
    return p


# Four pages: a bus and a direction each. Nested under CAN Bus, which stays the page for OBD and the
# per-bus hardware settings.
_GC = 'Configuration/CAN Bus'
EXTRA_PAGES[_GC] = page_can_branch
EXTRA_PAGES[f'{_GC}/OBD-II'] = page_obd
for _b in (0, 1):
    EXTRA_PAGES[f'{_GC}/CAN{_b + 1}'] = (lambda b=_b: page_can_bus(b))
    for _tx in (True, False):
        EXTRA_PAGES[f'{_GC}/CAN{_b + 1}/{"Transmit" if _tx else "Receive"}'] = \
            (lambda b=_b, t=_tx: page_generic_can(b, t))

# PATHS THIS INSTALLER USED TO MAKE AND NO LONGER DOES. An installer adds nodes and never removes
# them, so a page that is renamed or split leaves its old node behind pointing at nothing — which is
# how two "Generic CAN — CANn" nodes outlived the split into four. Listing them is the only way the
# installer can know; a rule that deleted anything undeclared would take the user's own pages with it.
# MALFORMED NODES a previous install left behind. These carry a slash INSIDE the name — the shape
# apply_top's old parent_and_leaf produced when a page sat more than one level below anything that
# existed — so their joined path collides with the properly nested node that should be there, and a
# path-keyed prune cannot tell them apart. Named explicitly, because a slash in a node name is
# legal in general and a blanket rule would eventually eat a real one.
# CHILD ORDER, where alphabetical is wrong. Nodes are created in sorted(pages) order, so a branch's
# children come out alphabetically — which put OBD-II after CAN2 while the branch's own switchboard
# listed it first. Naming the order here is the only way the two can agree; anything not listed keeps
# the order it already had.
CHILD_ORDER = {
    'Configuration/CAN Bus':      ['CAN1', 'CAN2', 'OBD-II'],
    # Transmit before Receive: it is the half you set up deliberately, and the half a dash or logger
    # on the other end is waiting for. Alphabetical put Receive first for no reason at all.
    'Configuration/CAN Bus/CAN1': ['Transmit', 'Receive'],
    'Configuration/CAN Bus/CAN2': ['Transmit', 'Receive'],
}

RETIRED_NODE_NAMES = [
    ('Configuration/CAN Bus', 'CAN1/Transmit'),
    ('Configuration/CAN Bus', 'CAN1/Receive'),
    ('Configuration/CAN Bus', 'CAN2/Transmit'),
    ('Configuration/CAN Bus', 'CAN2/Receive'),
]

RETIRED_PAGES = [
    'Configuration/Communications',   # the heading its children no longer need
    'Configuration/Generic CAN \u2014 CAN0',
    'Configuration/Generic CAN \u2014 CAN1',
    'Configuration/Generic CAN \u2014 CAN2',
]

EXTRA_PAGE_CONDS = {
    'Configuration/Fuel Tuning/O2 Control/Wideband Scope': '[#lambda.enabled] == 1',
    _GC: '',                       # a branch, not a feature — nothing to gate it on
    f'{_GC}/OBD-II': '',           # holds its own enable; hiding it would hide the way back
    # …and the same reason for a bus node: the page carries the switch that turns it back on, and
    # the switchboard row is the other way in.
    **{f'{_GC}/CAN{b + 1}': '' for b in (0, 1)},
    **{f'{_GC}/CAN{b + 1}/{d}': f'[#can.bus[{b}].enabled] == 1'
       for b in (0, 1) for d in ('Transmit', 'Receive')},
}


EXTRA_PANELS['lambda'] = branch_tables_panel('lambda', 'Configuration/Fuel Tuning/O2 Control')
EXTRA_PANELS['vvt_control'] = branch_tables_panel('vvt_control',
                                                  'Configuration/Engine Functions/Cam Control')


EXTRA_PANELS['transient_throttle'] = branch_tables_panel(
    'transient_throttle', 'Configuration/Fuel Tuning/Transient Throttle')
EXTRA_PANELS['torque_model'] = branch_tables_panel(
    'torque_model', 'Configuration/Engine Functions/Torque Model')
