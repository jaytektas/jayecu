"""The Idle branch, and the workspace tab that goes with it.

Two different things, which is the point the reference software makes by having both:

  the BRANCH is pages — one per table, reached through the tree, showing the numbers you edit;
  the WORKSPACE is a tab — its own viewport geometry with instrumentation arranged AROUND it, chosen for
  the job rather than for the page.

Idle is the clearest case for a workspace. Judging a base-duty cell means watching four things at once
(target, actual, error, output) while the engine hunts, and none of them is on the page you are editing.
"""
import author as A

ROOT = 'Configuration'
import fuel_tree as FT
from author import C_DIM, C_GREEN, C_AMBER, C_BLUE

I = 'idle.'
ON = '[#idle.enabled] == 1'
CL = ON + ' && [#idle.mode] == 1'          # the PID only exists in closed loop

# ONE SET OF NUMBERS, ON EVERY PAGE IN THE BRANCH. They were per-table and ad hoc — the base duty page
# showed Duty + Active + Coolant, the gain pages showed Error + Duty, the target page showed neither
# error nor duty — so moving between the pages of one tuning job changed which half of the picture you
# could see, and the module's own page showed none of it at all. The set is the four numbers idle is
# judged by (what it wants, what it has, the gap, what it is doing), led by the STATE, because the same
# four numbers mean different things in open loop, standing off, and closed loop.
LIVE = [('Idle State', 'idle_state', '%.0f'), ('Idle Target', 'idle_target_rpm', '%.0f'),
        ('Engine RPM', 'rpm', '%.0f'), ('RPM Error', 'idle_rpm_error', '%.0f'),
        ('Idle Duty', 'idle_duty', '%.0f'), ('Coolant', 'clt', '%.0f')]

# …and the states, in the firmware's own order (definition/ecu.schema.yaml `idle_state`). A page that
# shows only `idle_active` cannot tell OPEN LOOP from a closed loop standing off from a closed loop at
# work, and the gain tables only mean anything in the last of the three.
STATES = [('OFF', 0, C_DIM), ('NOT RUNNING', 1, C_DIM), ('OFF IDLE', 2, C_DIM),
          ('OPEN LOOP', 3, C_BLUE), ('WAITING', 4, C_AMBER), ('CLOSED LOOP', 5, C_GREEN)]

TABLES = [
    ('Target RPM',            'target_rpm_table',
     LIVE,
     'What the engine is asked to idle at, against coolant temperature.',
     'mode'),
    ('Base Duty',             'base_duty_table',
     LIVE + [('Idle LTT', 'idle_ltt_pct', '%.1f')],
     'The open-loop starting point. Get this close and the PID has little left to do.'),
    ('Min Output',            'min_output_table',
     LIVE,
     'The floor the controller may not close below.'),
    ('Start Target Offset',   'start_target_offset_table',
     LIVE + [('Run Time', 'run_time', '%.1f')],
     'Extra target RPM just after a start, decaying with run time.',
     'mode'),
    ('Start Base Offset',     'start_base_offset_table',
     LIVE + [('Run Time', 'run_time', '%.1f')],
     'Extra opening just after a start, on the same run-time axis.'),
    ('Proportional Gain',     'p_gain_table',
     LIVE,
     'How hard the controller answers the error it can see right now.',
     'mode'),
    ('Integral Gain',         'i_gain_table',
     LIVE,
     'How hard it answers an error that will not go away. Too much and it hunts.',
     'mode'),
    ('Derivative Gain',       'd_gain_table',
     LIVE,
     'How hard it answers the error CHANGING — the brake on an overshoot.',
     'mode'),
    ('Ignition Correction',   'idle_ign_corr_table',
     LIVE + [('Idle Ign Corr', 'idle_ign_corr', '%.1f'), ('Advance', 'advance', '%.1f')],
     'Timing is the fast way to hold idle: a spark change moves torque on the very next cycle, where an '
     'air valve takes several to make the same difference.'),
    ('Throttle Follower',     'throttle_follower_target_table',
     LIVE + [('Follower', 'idle_follower', '%.1f'), ('Throttle', 'tps', '%.1f')],
     'Holds the valve open as the throttle closes, so the engine is caught rather than dropped.',
     'throttle_follower_enabled'),
    ('Follower Decay',        'throttle_follower_decay_table',
     LIVE + [('Follower', 'idle_follower', '%.1f')],
     'How quickly that hold bleeds away.',
     'throttle_follower_enabled'),
    ('Long-Term Trim',        'idle_ltt',
     LIVE + [('Idle LTT', 'idle_ltt_pct', '%.1f')],
     'What the controller has learned it needs, remembered between runs.'),
]

# Closed loop is the only thing that acts on the TARGET: open loop drives the valve from the base
# tables alone, so everything that only moves the target moves a published number and nothing else.
CL_ONLY = '[#idle.mode] == 1'

# Learning happens only in closed loop, and only when the trim is switched on.
LTT_LEARN = '[#idle.mode] == 1 and [#idle.ltt_enabled] == 1'

SETTINGS = [
    ('Control', [('mode', ''), ('tps_closed_pct', '%'), ('idle_lockout_rpm', 'RPM'),
                 ('cl_activation_offset_rpm', 'RPM'), ('pid_scaler_pct', '%'), ('max_duty_pct', '%')]),
    ('Target Movement', [('rpm_rate_rising_limit', 'RPM/s', CL_ONLY),
                         ('rpm_rate_falling_limit', 'RPM/s', CL_ONLY),
                         # The decel offset decays over decel_decay_ms; at zero there is no decel step at all.
                         ('decel_offset_pct', '%', '[#idle.decel_decay_ms] > 0'),
                         ('decel_decay_ms', 'ms'), ('stall_offset_rpm', 'RPM')]),
    # The trim is APPLIED in both modes; it is only LEARNED in closed loop (Idle.cpp:216 — the learn
    # block is behind `closed`). So the switch and the runtime floor stay live and the three knobs that
    # describe how it learns do not.
    ('Long-Term Trim', [('ltt_enabled', ''),
                        ('ltt_authority_pct', '%', LTT_LEARN), ('ltt_learn_pct', '%', LTT_LEARN),
                        ('ltt_dwell_ms', 'ms', LTT_LEARN), ('ltt_min_runtime_s', 's', LTT_LEARN)]),
    ('Lockouts', [('throttle_follower_enabled', ''), ('vss_check_enabled', ''), ('max_vehicle_speed', 'km/h'),
                  ('close_on_boost_enabled', ''), ('close_on_boost_kpa', 'kPa')]),
]


def page_setup(meta):
    p = A.Page(title='Idle Control')
    p.head('Open loop is the base duty table alone; closed loop adds the PID on top of it.',
           enable=I + 'enabled')
    # AS MANY ACROSS AS THIS TAB HAS ROOM FOR. It was three — 10 + 3x410 = 1240 wide — on a tab that
    # reserves 900 for a page (VIEW, below: the readouts start at x=920). The page and the tab were both
    # stated in this file and disagreed with each other, so the window opened wider than the space and
    # scrolled sideways over the instruments it was supposed to sit beside. One number decides both now.
    per_row = max(1, int((VIEW[2] - 10) // 410))
    x, row_y, row_h, bottom = 10, A.TOP, 0, A.TOP
    for n, (title, rows) in enumerate(SETTINGS):
        if n and n % per_row == 0:
            x, row_y, row_h = 10, row_y + row_h + 12, 0
        h = A.panel_h(len(rows), A.ROW, top=12, bottom=2)
        row_h = max(row_h, h)
        panel = p.panel(x, row_y, 400, h, title)
        y = 12
        for row in rows:
            # A row may carry its own gate as a third element — see boost_pages and tools/gate_audit.py.
            field, unit = row[0], row[1]
            e = meta['config']['idle'].get(field, {})
            kind = 'checkbox' if (e.get('kind') == 'bool' or field.endswith('_enabled')) else \
                   ('enum' if e.get('kind') in ('enum', 'signal') else 'configedit')
            gate = CL if field in ('pid_scaler_pct', 'cl_activation_offset_rpm') else ON
            if len(row) > 2:
                gate = f'{gate} and {row[2]}'
            y = p.field(panel, 10, y, e.get('label', field), I + field, kind, 110, unit, enable=gate)
        x += 410
        bottom = max(bottom, row_y + h)

    # RIGHT NOW, on the page where the mode is chosen. This page had no live view at all: you set the
    # closed-loop gains and the activation offset here and had to go somewhere else to find out whether
    # the loop was even engaged. The lamps say WHICH state, the numbers say what it is doing in it, and
    # the same six numbers appear on every table page in the branch so nothing changes as you move.
    live = p.panel(10, bottom + 12, VIEW[2] - 20, A.panel_h(2, 40, top=12, bottom=8), 'Right Now')
    for i, (title, val, colour) in enumerate(STATES):
        p.add(p._new('indicator', 10 + i * 145, 12, 138, 30,
                     {'signalName': f'[$idle_state] == {val}', 'onTitle': title, 'offTitle': title,
                      'onBg': colour, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    for i, (lbl, ch, fmt) in enumerate(LIVE[1:]):          # the state is the lamps above
        p.readout(live, 10 + i * 175, 50, lbl, ch, fmt, w=165)
    return p


def page_idle_up():
    """The six idle-up requests: a load switches on, the idle target and base duty go up to carry it.

    Air conditioning, power steering at full lock, a cooling fan, an electrical load — each is a slot,
    and each slot is a bus signal, a threshold, what it adds, and how it comes and goes. It is an array,
    so the module page (a list of scalars) could not show it at all: six rows of seven fields is a grid
    or it is nothing.
    """
    p = A.Page(title='Idle Up')
    y0 = p.head('A load the engine has to carry, and the idle-up that carries it. Each slot watches one '
                'signal; while that signal is active the idle target and the base duty are raised by '
                'what the row says.', enable=I + 'enabled')

    COLS = [('input_sig', 'Input Signal', 200, 'enum'), ('threshold_x10', 'Above', 80, ''),
            ('rpm_offset', '+ RPM', 80, ''), ('base_offset_x10', '+ Duty', 80, ''),
            ('on_delay_ms', 'On Delay', 90, ''), ('decay_ms', 'Decay', 90, '')]
    N = 6
    grid = p.panel(10, y0, 820, A.panel_h(N + 1, A.ROW, top=34, bottom=8), 'Requests')
    p.add(p._new('label', 10, 8, 34, 18,
                 {'labelText': 'On', 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}), into=grid)
    cx = 48
    for _f, header, cw, _k in COLS:
        p.add(p._new('label', cx, 8, cw, 18,
                     {'labelText': header, 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
              into=grid)
        cx += cw + 10
    p.add(p._new('panel', 10, 28, 790, 1, {'bgColor': C_DIM, 'padding': '0'}), into=grid)
    y = 34
    for i in range(N):
        e = f'{I}idle_up[{i}]'
        on = f'[#{e}.enabled] == 1'
        g = p.group()
        p.add(p._new('checkbox', 12, y + 2, A.CHECK_H, A.CHECK_H, {'signalName': f'{e}.enabled'}, g), into=grid)
        cx = 48
        for f, _h, cw, kind in COLS:
            path = f'{e}.{f}'
            # +RPM raises the closed-loop TARGET and nothing acts on that in open loop; +Duty raises the
            # base directly and works in both. One column of this grid is mode-specific, the rest is not.
            gate = on + (' and [#idle.mode] == 1' if f == 'rpm_offset' else '')
            p.add(p._new(A.Page.control_for(path, kind), cx, y, cw, A.CTL_H,
                         {'signalName': path, 'enableCondition': gate}, g), into=grid)
            cx += cw + 10
        y += A.ROW

    note = ('ABOVE is the level the input signal must exceed to count as active — 0.5 for a plain 0/1 '
            'switch, a real number for an analogue input like a pressure sender.\n\n'
            'ON DELAY is how long it must hold before the offset engages, which keeps a switch that '
            'chatters from stepping the idle. DECAY is how long the offset takes to ramp away after the '
            'input releases — an offset that vanishes instantly makes the idle dip.\n\n'
            'The RPM offset raises the closed-loop TARGET; the duty offset raises the base directly, '
            'which is what an open-loop idle needs. A slot may use either or both.')
    nh = A.Page.wrapped_h(note, 400)
    side = p.panel(840, y0, 430, A.panel_h(1, nh + 2 * A.ROW + 12, top=10, bottom=8), 'How a Slot Works')
    p.wrapped(10, 10, 400,
              note,
              into=side)
    y = nh + 18
    for label, node in (('Idle Control', f'{ROOT}/Engine Functions/Idle Control'),
                        ('Target RPM', f'{ROOT}/Engine Functions/Idle Control/Target RPM')):
        y = p.switch(side, 10, y, label, '', link=node, w=376, pitch=28)

    live = p.panel(10, y0 + A.panel_h(N + 1, A.ROW, top=34, bottom=8) + 10, 1260, 100, 'Right Now')
    for n, (lbl, ch, fmt) in enumerate(LIVE + [('Air Con', 'ac_request', '%.0f')]):
        p.readout(live, 15 + n * 175, 8, lbl, ch, fmt, w=165)
    return p


# The branch's non-table children, installed alongside the table pages (see apply_workspaces).
EXTRA_PAGES = {'Idle Up': page_idle_up}


# ---- the workspace ------------------------------------------------------------------------------
# A workspace is a job, not a page: the viewport is deliberately smaller than Main's (idle tables are
# small — a curve over coolant, a gain table) and the room that buys goes to the instruments that answer
# "is it settling?". Laid out on the 1600x800 canvas as three columns: pages left with the target/actual
# trace under them, the numbers in the middle, the operator's own list down the right.
# The room this tab leaves a page: from under the tab strip to just above the trace, and left of the
# readouts that start at x=920. 490 rather than 480 because the setup page needs exactly that — ten pixels
# short of it meant a vertical scroll bar on a page that otherwise fitted perfectly.
VIEW = (10, 92, 900, 490)


def workspace_widgets(next_id, uuid4):
    """The static furniture of the Idle tab: readouts, a watch list and graphs, arranged around a viewport
    that is deliberately smaller than Main's. None of it changes as the tree moves — that is the point."""
    out, i = [], next_id
    def w(type_, x, y, ww, hh, props, group=0):
        nonlocal i
        i += 1
        return {'id': i - 1, 'uid': uuid4(), 'type': type_, 'x': x, 'y': y, 'w': ww, 'h': hh,
                'groupId': group, 'props': props}

    def readout(x, y, label, ch, fmt, ww=160):
        g = 920 + len(out)
        out.append(w('label', x, y, ww, 18,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_SMALL, 'fgColor': C_DIM}, g))
        out.append(w('value', x, y + 18, ww, 30,
                     {'signalName': ch, 'format': fmt, 'fontName': '|20|0|0', 'align': 'Left',
                      'borderWidth': '1', 'borderRadius': '3', 'padding': '3'}, g))

    # THE FOUR NUMBERS THIS TAB EXISTS FOR: what it wants, what it has, the gap, and what it is doing
    # about it. Everything else on the page is context for these.
    for n, (label, ch, fmt) in enumerate((('Idle Target', 'idle_target_rpm', '%.0f'),
                                          ('Engine RPM', 'rpm', '%.0f'),
                                          ('RPM Error', 'idle_rpm_error', '%.0f'),
                                          ('Idle Duty', 'idle_duty', '%.0f'))):
        readout(920 + (n % 2) * 175, 92 + (n // 2) * 60, label, ch, fmt)

    for n, (label, ch, fmt) in enumerate((('Idle State', 'idle_state', '%.0f'),
                                          ('Idle LTT', 'idle_ltt_pct', '%.1f'),
                                          ('Ign Corr', 'idle_ign_corr', '%.1f'),
                                          ('Follower', 'idle_follower', '%.1f'),
                                          ('Throttle', 'tps', '%.1f'),
                                          ('Coolant', 'clt', '%.0f'))):
        readout(920 + (n % 2) * 175, 216 + (n // 2) * 60, label, ch, fmt)

    # Target against actual over time is the whole diagnosis: hunting, drooping or settling.
    # TARGET AGAINST ACTUAL, on ONE graph. Idle is judged by the two traces together — hunting, drooping
    # or settling is a shape they make jointly — and a graph per channel says nothing about the gap
    # between them. Fixed 0..2000 rpm so the shape stays comparable between runs instead of the
    # auto-scale flattering every wobble to full height.
    out.append(w('livegraph', 10, 592, 900, 198,
                 {'lines': 'idle_target_rpm,0,2000,0,0\nrpm,0,2000,0,0', 'showLegend': '1',
                  'displayUnit': 'Auto', 'borderWidth': '1', 'labelText': 'Target vs actual'}))
    out.append(w('livegraph', 920, 416, 350, 374,
                 {'lines': 'idle_duty,0,100,0,0\nidle_ltt_pct,-20,20,0,0\nidle_rpm_error,-500,500,0,0',
                  'showLegend': '1', 'displayUnit': 'Auto', 'borderWidth': '1',
                  'labelText': 'What the controller is doing'}))
    # …and the operator's own list, because a diagnosis always needs one more channel than anyone planned.
    out.append(w('channels', 1290, 92, 300, 698,
                 {'labelText': 'Idle Channels', 'rowHeight': '24', 'showUnits': '1', 'editable': '1',
                  'format': '%.2f', 'borderWidth': '1', 'borderRadius': '3',
                  'channels': 'idle_state;idle_target_rpm;rpm;idle_rpm_error;idle_duty;idle_ltt_pct;idle_ign_corr;'
                              'idle_follower;tps;clt;vehicle_spd'}))
    return out, i
