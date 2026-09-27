"""The Boost branch, and its workspace tab.

Boost is a control loop you judge by four traces moving together — target, actual, error and the duty the
controller is asking for — so the workspace gives those the room and keeps the tables small. The branch
is longer than it was (nine tables now, including the four correction slots and the learned trim), but
the instrumentation is still where the value is: the settings are read once and the traces every pull.

Deviation from the reference, on purpose: instead of one long channel list of everything boost-ish, the
readouts are grouped by the QUESTION they answer — what it is trying to do, what it is doing about it,
what is limiting it — and the operator's own list sits beside them for whatever this particular chase
needs.
"""
import author as A
from author import C_DIM

B = 'boost.'
ON = '[#boost.enabled] == 1'
CL = ON + ' && [#boost.mode] == 1'
SCR = '[#boost.scramble_sig] >= 0'
LTT = '[#boost.ltt_en] == 1'
EWG = '[#boost.output_mode] == 1'

TABLES = [
    ('Boost Target', 'boost_target_table',
     [('Boost Target', 'boost_target', '%.0f'), ('MAP', 'map', '%.0f'), ('Engine RPM', 'rpm', '%.0f')],
     'What the engine is asked for. Everything below chases this.'),
    ('Wastegate Base Duty', 'base_boost_duty_table',
     [('Wastegate Duty', 'wastegate_duty', '%.0f'), ('Boost Target', 'boost_target', '%.0f'),
      ('MAP', 'map', '%.0f')],
     'What duty this much boost takes — read against the TARGET, so a cell answers one question. '
     'Get it close and the PID only has to trim.'),
    ('Controller Start Delay', 'start_delay_table',
     [('Engine RPM', 'rpm', '%.0f'), ('MAP', 'map', '%.0f'), ('Boost Target', 'boost_target', '%.0f')],
     'How long the loop waits after activation before it takes over — spool time, which is not one '
     'number. Runs alongside the control point; whichever is still unsatisfied keeps the loop out.',
     CL + ' and [#boost.start_delay_en] == 1'),
    ('Integral Gain vs Error', 'ki_table',
     [('Boost Error', 'boost_error', '%.1f'), ('Boost Target', 'boost_target', '%.0f'),
      ('MAP', 'map', '%.0f')],
     'Integral gain against how far boost is from target. Gentle in the middle and firmer at the '
     'ends: close to target a large Ki hunts on what is mostly noise, far from it the same difference '
     'is a real offset the feed-forward did not cover.',
     CL + ' and [#boost.ki_sched_en] == 1'),
] + [
    # THE LEARNED TRIM, with its own workflow on its own page: Reset and Apply to Base are what the
    # surface is FOR, and they belong beside the grid rather than behind a menu of table operations.
    ('Long Term Trim', 'boost_ltt',
     [('Boost Long-Term Trim', 'boost_ltt_pct', '%.1f'), ('Wastegate Duty', 'wastegate_duty', '%.0f'),
      ('Boost Error', 'boost_error', '%.1f')],
     'What the closed loop keeps having to correct, migrated out of the integrator and into the cell '
     'it belongs to. Apply it to the base duty table and the feed-forward knows next time; the loop '
     'then starts from nothing instead of re-discovering the same offset on every pull.'),
] + [
    # THE FOUR SLOTS. A slot is a channel, a curve and which number it trims, so its channel and its
    # Applies To live on the page with the curve — picking them anywhere else would split one decision
    # across two screens. Shipped pointed at air temp, coolant, gear and throttle, all off, all flat.
    (f'Correction {n}', f'corr{n}_table',
     [('Boost Target', 'boost_target', '%.0f'), ('MAP', 'map', '%.0f'),
      ('Wastegate Duty', 'wastegate_duty', '%.0f')],
     'Per cent, against whatever channel this slot reads. On the TARGET it multiplies — the closed-loop '
     'answer, and the one that actually changes boost. On the DUTY it adds, which is the open-loop '
     'answer: close the loop and the integrator unwinds it within a second or two.',
     f'corr{n}_en',
     [('Reads', f'boost.corr{n}_table_x_src', 'enum', ''),
      ('Applies To', f'boost.corr{n}_applies', 'enum', '')])
    for n in (1, 2, 3, 4)
]

# Panels whose settings the firmware only reads in CLOSED loop. An open-loop tune has no target, so it
# has no error, no handover and no gains — and a live control that changes nothing is worse than a grey
# one, because it invites tuning against it.
CLOSED_LOOP_PANELS = {'Closed Loop', 'Handover'}

SETTINGS = [
    ('Control', [('mode', ''), ('output_mode', ''),
                 ('activation_rpm', 'RPM'), ('activation_kpa', 'kPa'),
                 ('min_duty_pct', '%'), ('max_duty_pct', '%')]),
    # THE MOTORISED GATE'S OWN LOOP, greyed entirely on a solenoid. Everything above is the same
    # question either way — how much boost to ask for — and only this panel is about the actuator.
    ('Motorised Gate', [('gate_pos_sig', '', EWG), ('gate_min_pos_pct', '%', EWG),
                        ('gate_max_pos_pct', '%', EWG), ('gate_rate_pct_s', '%/s', EWG),
                        ('gate_kp', '%/%', EWG), ('gate_ki', '%/%/s', EWG), ('gate_kd', '%*s/%', EWG),
                        ('gate_iterm_max_pct', '%', EWG)]),
    # ACTIVATION IS NOT THE HANDOVER, and keeping them in separate panels is the point. Activation asks
    # whether the module acts at all; these ask when the loop takes over from the base duty table.
    ('Handover', [('control_point_kpa', 'kPa'), ('spool_assist', ''), ('start_delay_en', '')]),
    ('Closed Loop', [('kp', '%/kPa'), ('ki', '%/kPa/s', '[#boost.ki_sched_en] == 0'),
                     ('ki_sched_en', ''), ('kd', '%*s/kPa'),
                     ('iterm_max_pct', '%'),
                     # A derivative CEILING on a loop with no derivative term is a control that takes
                     # a value nothing reads — the firmware skips the whole D branch when kd is zero.
                     ('max_deriv_kpa_s', 'kPa/s', '[#boost.kd] != 0'),
                     ('min_tps_pct', '%')]),
    # Two overboost questions, and they sit together because choosing between them is the decision:
    # the limit is what the engine cannot survive, the offset is how much more than asked for.
    ('Limits', [('overboost_limit_kpa', 'kPa'), ('overboost_offset_kpa', 'kPa'),
                ('overboost_hyst_kpa', 'kPa'), ('overboost_cut_method', '')]),
    # THE DRIVER'S CONTROLS. All three are bus signals rather than pins, so any of them can equally be
    # an expression, a CAN frame or a dash button — the module cannot tell the difference.
    # The gates say WHEN a reading is worth keeping; the rate and authority say how much of it to
    # believe. Everything greys together on the enable — except that a learned table stays APPLIED
    # whether or not learning is on, which is why the switch is called Enabled and not Used.
    ('Long-Term Trim', [('ltt_en', ''),
                        ('ltt_min_tps_pct', '%', LTT), ('ltt_min_rpm', 'RPM', LTT),
                        ('ltt_max_rpm', 'RPM', LTT), ('ltt_min_gear', '', LTT),
                        ('ltt_dwell_ms', 'ms', LTT), ('ltt_learn_pct', '%', LTT),
                        ('ltt_authority_pct', '%', LTT)]),
    ('Driver', [('arm_sig', ''), ('trim_sig', ''),
                ('trim_max_kpa', 'kPa', '[#boost.trim_sig] >= 0')]),
    # The bump is dead until an input is assigned to ask for it, and so are its timers.
    ('Scramble', [('scramble_sig', ''),
                  ('scramble_kpa', 'kPa', SCR), ('scramble_base_pct', '%', SCR),
                  ('scramble_hold_s', 's', SCR), ('scramble_max_s', 's', SCR),
                  ('scramble_rest_s', 's', SCR)]),
]


# WHICH PANELS GO ON WHICH PAGE. One page could not hold them: the canvas is the size the workspace
# tab reserves, and a panel taller than what is left of it is silently CLIPPED — the last rows of
# Scramble came out 8px high, captions cut in half, with nothing to say so but check_doc.
#
# The split is by subject rather than to fill space. The branch page is the loop: what it is asked for,
# when the loop takes over, how hard it pushes and what it may not exceed. What the answer is given to,
# who is allowed to move it, and how it learns are each their own page — and each is a thing you set up
# once and then leave alone, which is exactly what belongs off the page you come back to.
# (title, panels, blurb, node gate). The gate is what keeps a page out of the tree when its whole
# subject is switched off — the servo page on a car with a solenoid is not a page to grey, it is a page
# that does not apply.
PAGES = [
    ('Boost Control',   ['Control', 'Handover', 'Closed Loop', 'Limits'],
     'Open loop is the base duty table alone; closed loop trims it toward the target.', ''),
    ('Actuator',        ['Motorised Gate'],
     'A solenoid takes a duty and the wastegate spring does the rest. A motorised gate takes a '
     'position, and the loop below drives the motor until the valve is there.',
     '[#boost.output_mode] == 1'),
    ('Driver Controls', ['Driver', 'Scramble'],
     'The arm switch, the trim knob and the scramble button. All three are signals rather than pins, '
     'so any of them can equally be an expression or a button on the dash.', ''),
    ('Trim Learning',   ['Long-Term Trim'],
     'When a correction is worth keeping. The gates decide which readings belong to the engine and '
     'which belong to the conditions they were taken in.',
     '[#boost.ltt_en] == 1'),
]


def _settings_page(meta, title, want, blurb):
    p = A.Page(title=title)
    p.head(blurb, enable=B + 'enabled')
    # AS MANY ACROSS AS THIS TAB HAS ROOM FOR — see the same note in idle_pages: the page and the VIEW
    # its own tab reserves are both stated in this file, and they have to agree or the window opens wider
    # than the space and scrolls over the instruments beside it.
    per_row = max(1, int((VIEW[2] - 10) // 410))
    panels = [(t, rows) for t, rows in SETTINGS if t in want]
    x, row_y, row_h = 10, A.TOP, 0
    for n, (name, rows) in enumerate(panels):
        if n and n % per_row == 0:
            x, row_y, row_h = 10, row_y + row_h + 12, 0
        h = A.panel_h(len(rows), A.ROW, top=12, bottom=2)
        row_h = max(row_h, h)
        panel = p.panel(x, row_y, 400, h, name)
        y = 12
        for row in rows:
            # A row may carry its own gate as a third element — a setting the firmware only reads in
            # some mode greys rather than taking a value nobody consults (tools/gate_audit.py).
            field, unit = row[0], row[1]
            e = meta['config']['boost'].get(field, {})
            kind = 'enum' if e.get('kind') in ('enum', 'signal') else 'configedit'
            gate = CL if name in CLOSED_LOOP_PANELS else ON
            if len(row) > 2:
                gate = f'{gate} and {row[2]}'
            y = p.field(panel, 10, y, e.get('label', field), B + field, kind, 110, unit, enable=gate)
        x += 410
    return p


def page_setup(meta):
    title, want, blurb, _gate = PAGES[0]
    return _settings_page(meta, title, want, blurb)


EXTRA_PAGES = {
    title: ((lambda m, w=want, t=title, b=blurb: _settings_page(m, t, w, b)), gate)
    for title, want, blurb, gate in PAGES[1:]
}


VIEW = (10, 92, 900, 400)     # the two boost tables are small; the loop needs the room


def workspace_widgets(next_id, uuid4):
    out, i = [], next_id
    def w(t, x, y, ww, hh, props, g=0):
        nonlocal i
        i += 1
        return {'id': i - 1, 'uid': uuid4(), 'type': t, 'x': x, 'y': y, 'w': ww, 'h': hh,
                'groupId': g, 'props': props}

    def readout(x, y, label, ch, fmt, ww=160, enable=''):
        g = 940 + len(out)
        lp = {'labelText': label, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}
        vp = {'signalName': ch, 'format': fmt, 'fontName': '|20|0|0', 'align': 'Left',
              'borderWidth': '1', 'borderRadius': '3', 'padding': '3'}
        if enable: lp['enableCondition'] = vp['enableCondition'] = enable
        out.append(w('label', x, y, ww, 18, lp, g))
        out.append(w('value', x, y + 18, ww, 30, vp, g))

    def heading(x, y, text, ww=340):
        out.append(w('label', x, y, ww, 18,
                     {'labelText': text, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}))

    # WHAT IT IS TRYING TO DO — the loop, in the order you read it.
    heading(720, 100, 'The loop')
    for n, (label, ch, fmt) in enumerate((('Boost Target', 'boost_target', '%.0f'),
                                          ('MAP', 'map', '%.0f'),
                                          ('Boost Error', 'boost_error', '%.1f'),
                                          ('Wastegate Duty', 'wastegate_duty', '%.0f'))):
        readout(920 + (n % 2) * 175, 92 + (n // 2) * 60, label, ch, fmt)

    # WHAT IS LIMITING IT — the answer to "why won't it make target".
    heading(720, 245, 'What is limiting it')
    for n, (label, ch, fmt) in enumerate((('Protection Boost', 'prot_boost_corr', '%.0f'),
                                          ('Knock Retard', 'knock_retard', '%.1f'),
                                          ('Lambda', 'lambda_1', '%.2f'),
                                          ('Air Temp', 'iat', '%.0f'))):
        readout(920 + (n % 2) * 175, 240 + (n // 2) * 60, label, ch, fmt)

    # …and the hardware, only where the car HAS it: a wastegate position sensor is not a given, so these
    # grey out on an engine whose sensors are not enabled rather than reading a permanent zero.
    heading(720, 390, 'Hardware  (greys when the sensor is not fitted)')
    readout(920, 400, 'Wastegate Pos', 'wastegate_valve_pos_1', '%.1f',
            enable='[#sensors.sensor[wastegate_valve_pos_1].enabled] == 1')
    # The servo's own two numbers, and only on a car that has one: where the valve was asked to be and
    # what the motor is being driven with. Reading them together is how a stuck gate shows itself —
    # full motor duty and a position that is not moving.
    readout(1270, 400, 'Gate Target', 'wastegate_pos_target', '%.1f', enable=EWG)
    readout(1270, 460, 'Gate Motor', 'wastegate_pos_duty', '%.1f', enable=EWG)
    readout(1095, 400, 'Turbo Speed', 'turbo_speed_1', '%.0f',
            enable='[#sensors.sensor[turbo_speed_1].enabled] == 1')
    readout(920, 460, 'Wastegate Temp', 'wastegate_1_temp', '%.0f',
            enable='[#sensors.sensor[wastegate_1_temp].enabled] == 1')
    readout(1095, 460, 'Pre-IC Boost', 'boost_pressure_pre_ic', '%.0f',
            enable='[#sensors.sensor[boost_pressure_pre_ic].enabled] == 1')

    # The loop on one graph: what it wants, what it has, and what it is doing about it. Fixed ranges so
    # a spike is a spike rather than a rescale.
    out.append(w('livegraph', 10, 502, 900, 288,
                 {'lines': 'boost_target,0,300,0,0\nmap,0,300,0,0\nwastegate_duty,0,100,0,0\n'
                           'boost_error,-100,100,0,0',
                  'showLegend': '1', 'displayUnit': 'Auto', 'borderWidth': '1',
                  'labelText': 'Target · MAP · duty'}))
    out.append(w('livegraph', 920, 530, 350, 260,
                 {'lines': 'knock_level,0,60,0,0\nlambda_1,0.6,1.4,0,0', 'showLegend': '1',
                  'displayUnit': 'Auto', 'borderWidth': '1', 'labelText': 'What limits it'}))
    out.append(w('channels', 1290, 92, 300, 698,
                 {'labelText': 'Boost Channels', 'rowHeight': '24', 'showUnits': '1', 'editable': '1',
                  'format': '%.2f', 'borderWidth': '1', 'borderRadius': '3',
                  'channels': 'boost_target;map;wastegate_duty;rpm;gear;knock_level;lambda_1;iat;'
                              'boost_error;prot_boost_corr;baro_kpa'}))
    return out, i
