"""Electronic throttle: the drive, the feedback, and the three calibration steps — one page per body.

NOT a template page. The two throttle bodies look identical in configuration and are not identical in
anything else: their live channels are separate names (etb_position_1 / etb_position_2, not one channel
indexed by an element), and every calibration command takes the body's INDEX as an argument. The page
that was here was body A's, mirrored to B through "[*]" — so B's Step 1 button sent a bare `findlimits`
(which the firmware reads as body 0) and its buttons went green off `[$etb_state_1]`. Pressing "Find
limits" on the B page calibrated A and then reported A's state back as if it were B's.

So both pages are GENERATED FROM ONE FUNCTION with the index baked in. Same shape, right channels.

The order is the whole point of the page, and it is the order the bench recipe uses:

    enable → feedback signals → 1 find the stops → 2 fill the feed-forward → 3 autotune the PID

Each step's button carries the state colour the firmware publishes (command_state: amber running, green
done, red failed) and is disabled while the body is in a state where that step means nothing. The knobs
each step reads live on the body's Calibration Settings page — they are read once at setup and never
again, and they were crowding out the three buttons that matter.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from author import C_DIM, C_GREEN, C_AMBER, C_RED, C_BLUE

CFG  = 'Configuration'
ETB  = f'{CFG}/Engine Functions/Electronic throttle'
# THE BRIDGES ARE NOT PART OF THE THROTTLE. They lived under Electronic throttle because that was the
# first thing wired to one, which put the driver a coil-driven stepper idle valve needs inside the
# ELECTRONIC THROTTLE branch. They are general output hardware — the board calls them "usable as ETB
# servo / stepper / BAC idle valve / DC-PWM" — and the only bidirectional hardware the ECU has, so they
# sit beside Outputs under Electrical, with both consumers linking to them.
HB   = 'Configuration/Electrical/Half Bridges'
NAME = ['A', 'B']
# The tree's own spelling. A's node is "Throttle body A" and B's "Throttle Body B" — a page key and a
# link have to match the node exactly, and the difference is one letter's case.
NODE = ['Throttle body A', 'Throttle Body B']

# command_state, as the firmware packs it (Comms/CommandState.h): (op << 2) | phase, phase 1 running,
# 2 done, 3 failed. The ETB ops are (base + body index), so body B's codes are body A's plus four — which
# is exactly the trap hard-coded numbers fall into: B's buttons watched A's codes and went green when A
# finished, on a page that is otherwise entirely about body B.
OP_BASE = {'findlimits': 1, 'fillff': 3, 'autotune': 5}
PEDALCAL_OP = 7


def _code(op, phase):
    return (op << 2) | phase

# ETB run states (EtbState): 0 uncalibrated, 1 calibrating, 2 running (READY), 3 fault.
CALIBRATING, RUNNING, FAULT = 1, 2, 3


def _cmd_ranges(cmd, st, i):
    """A button that shows what the command is doing: amber while it runs, green when it finished, red if
    it failed or left the body in fault. The codes are the firmware's own — the studio does not track the
    command, it reads the state the ECU publishes."""
    op = OP_BASE[cmd] + i
    run, ok, bad = _code(op, 1), _code(op, 2), _code(op, 3)
    return (f'{C_AMBER},,,,0,[$command_state] == {run};'
            f'{C_GREEN},,,,0,[$command_state] == {ok} && [$etb_state_{st}] == {RUNNING};'
            f'{C_RED},,,,0,[$command_state] == {bad} || [$etb_state_{st}] == {FAULT}')


def page_throttle(i):
    """One throttle body: what it reads, what it may do, and the three steps that make it work."""
    st = i + 1                                   # live channels are 1-based: etb_position_1 / _2
    e = f'electronic_throttle.etb[{i}]'
    p = A.Page(title=f'Throttle Body {NAME[i]}')
    y0 = p.head(f'Throttle body {NAME[i]}: the two position sensors it believes, the travel it is allowed, '
                f'and the three calibration steps — done in order, with the engine stopped.',
                enable=f'{e}.enabled')
    on = f'[#{e}.enabled] == 1'

    # ---- what it reads -------------------------------------------------------------------------
    fb = p.panel(10, y0, 310, A.panel_h(4, A.ROW, top=12, bottom=6), 'Feedback', enable=on)
    y = 12
    y = p.field(fb, 10, y, 'TPS A Signal', f'{e}.tps_a_src', 'enum', 150, lbl_w=120)
    y = p.field(fb, 10, y, 'TPS B Signal', f'{e}.tps_b_src', 'enum', 150, lbl_w=120)
    y = p.field(fb, 10, y, 'Match Limit', f'{e}.tps_match_err_pct', 'configedit', 100, '%', lbl_w=120)
    p.field(fb, 10, y, 'Match Debounce', f'{e}.tps_match_ms', 'configedit', 100, 'ms', lbl_w=120)
    h1 = A.panel_h(4, A.ROW, top=12, bottom=6)
    n1 = p.note(10, y0 + h1 + 6,
                'Two sensors that must agree: a throttle that can open itself is only safe while something '
                'is checking the one against the other. Disagree by more than the limit for longer than '
                'the debounce and the body is cut to its relax position.', w=300)

    # ---- what it may do ------------------------------------------------------------------------
    # UNDER THE NOTE, not 96px under the panel. The literal was a guess at how tall the note would be,
    # and a guess that is one line short puts the next panel on top of the note's last line.
    y1 = n1['y'] + n1['h'] + 6
    lim = p.panel(10, y1, 310, A.panel_h(5, A.ROW, top=12, bottom=6), 'Travel & Rate', enable=on)
    y = 12
    y = p.field(lim, 10, y, 'Min TPS', f'{e}.min_tps_pct', 'configedit', 100, '%', lbl_w=120)
    y = p.field(lim, 10, y, 'Max TPS', f'{e}.max_tps_pct', 'configedit', 100, '%', lbl_w=120)
    y = p.field(lim, 10, y, 'Relax Position', f'{e}.relax_pct', 'configedit', 100, '%', lbl_w=120)
    y = p.field(lim, 10, y, 'Open Rate', f'{e}.open_rate_pct_s', 'configedit', 100, '%/s', lbl_w=120)
    p.field(lim, 10, y, 'Close Rate', f'{e}.close_rate_pct_s', 'configedit', 100, '%/s', lbl_w=120)
    p.note(10, y1 + A.panel_h(5, A.ROW, top=12, bottom=6) + 6,
           'Min and Max keep the target clear of the stops Step 1 finds — in plate percent, not pedal. '
           'Relax is where the plate rests with no drive at all, which on a spring-return body is where it '
           'goes the instant anything is wrong.', w=300)

    # ---- the feed-forward map ------------------------------------------------------------------
    ff = p.panel(330, y0, 450, 432, 'Feed-forward', enable=on)
    tbl = p.add(p._new('table', 10, 10, 420, 150,
                       {'signalName': f'{e}.ff_table', 'axisMode': '1', 'displayUnit': 'Auto',
                        'displayUnitX': 'Auto', 'displayUnitY': 'Auto', 'cellTrace': '2'}), into=ff)
    # The curve gives up 60px so the map's own reading fits under it; it is a shape, and 130px says the
    # same shape 190 did.
    p.add(p._new('curve', 10, 168, 420, 130,
                 {'signalName': f'{e}.ff_table', 'displayUnit': 'Auto', 'axisUnit': 'Auto',
                  'valueUnit': 'Auto'}), into=ff)
    # WHAT THE MAP IS ASKING FOR RIGHT NOW, under the grid you are editing. The ETB publishes its TOTAL
    # duty, which is feed-forward PLUS the PID — so nothing on this page said what the map on its own
    # was contributing. Cache::solveTable answers it from the table's own path.
    p.readout(ff, 10, 302, 'Feed-forward', f'{e}.ff_table', A.table_fmt(f'{e}.ff_table'), w=140)
    p.wrapped(10, 356, 420,
              'The duty each position needs with no help from the PID. Step 2 measures '
                               'one ROW of it — the row selected in the table above.',
              into=ff)

    # ---- the controller ------------------------------------------------------------------------
    pid = p.panel(330, y0 + 442, 450, A.panel_h(2, A.ROW, top=12, bottom=6) + 30, 'PID', enable=on)
    y = 12
    # All four the same box: they are the same kind of number, and a gain that looks bigger than its
    # neighbour because its box is wider is a page telling you something that is not true.
    for col, rows in ((10, (('Kp', 'kp', ''), ('Ki', 'ki', ''))),
                      (220, (('Kd', 'kd', ''), ('I Clamp', 'iterm_max_pct', '%')))):
        y = 12
        for label, field, unit in rows:
            y = p.field(pid, col, y, label, f'{e}.{field}', 'configedit', 100, unit, lbl_w=90)

    # ---- the three steps -----------------------------------------------------------------------
    cal = p.panel(790, y0, 480, 390, 'Calibrate, In Order', enable=on)
    steps = [
        ('findlimits', f'Step 1 · Find limits', None,
         'Drives the plate to both stops with the engine off; writes the TPS cal and Relax.'),
        ('fillff',     f'Step 2 · Fill feed-forward', f'[@{tbl["uid"]}.selectedRow]',
         'Sweeps the selected ff row and records the duty each position needs.'),
        # autotune takes a RULE as well as the body: "autotune <etb> <rule 0..4>". Sending only the index
        # is not a no-op — the firmware answers with its usage line and tunes nothing, which reads on the
        # page as a button that does not work. The rule comes from a host variable so it can be chosen
        # here without inventing an ECU setting for something that is an argument to a command.
        ('autotune',   f'Step 3 · Autotune PID', '[#pc.etb_autotune_rule]',
         'Relay-feedback tune at each ff bin; keeps the worst-case gains, so it holds everywhere.'),
    ]
    ry = 12
    for cmd, label, arg1, why in steps:
        props = {'labelText': label, 'command': cmd, 'arg0': str(i),
                 'ranges': _cmd_ranges(cmd, st, i),
                 # Step 1 is the one that may run while calibrating; the other two need a working body.
                 'enableCondition': (f'[$etb_state_{st}] != {CALIBRATING}' if cmd == 'findlimits'
                                     else f'[$etb_state_{st}] == {RUNNING}')}
        if arg1: props['arg1'] = arg1
        p.add(p._new('command', 10, ry, 200, 40, props), into=cal)
        p.wrapped(220, ry - 2, 250,
                  why,
                  into=cal)
        ry += 56
    # …and the rule that Step 3 sends with it.
    p.add(p._new('label', 10, ry + 4, 90, A.LBL_H,
                 {'labelText': 'Rule', 'align': 'Left', 'fontName': A.FONT_LBL}), into=cal)
    p.add(p._new('combobox', 104, ry, 240, A.CTL_H, {'signalName': 'pc.etb_autotune_rule'}), into=cal)
    p.add(p._new('label', 352, ry + 4, 120, A.LBL_H,
                 {'labelText': 'for Step 3', 'align': 'Left', 'fontName': A.FONT_SMALL,
                  'fgColor': C_DIM}), into=cal)
    ry += 34

    for label, node in ((f'Half Bridge {NAME[i]}  (the driver)', f'{HB}/Half Bridge {NAME[i]}'),
                        ('Calibration Settings  (what each step uses)',
                         f'{ETB}/{NODE[i]}/Calibration Settings')):
        ry = p.switch(cal, 10, ry + 2, label, '', link=node, w=420, pitch=26)
    p.wrapped(10, ry + 4, 460,
              'ENGINE STOPPED for all three: each one drives the plate against its stops '
                               'or oscillates it deliberately. The button goes amber while the ECU is '
                               'working, green when it finishes, red if it fails or the body faults. The '
                               'numbers each step uses are on Calibration Settings.',
              into=cal)

    # ---- what it is doing now ------------------------------------------------------------------
    live = p.panel(790, y0 + 400, 480, A.CANVAS_H - (y0 + 400) - 8, 'Right Now')
    # DEMAND, not "target": etb_target_N and etb_iterm_N are declared channels with no producer — nothing
    # in ElectronicThrottle.cpp ever sets them, so both read a steady 0 and looked like a throttle that
    # was asked for nothing while it was plainly moving. throttle_demand is what the module actually
    # publishes and chases (ElectronicThrottle.cpp:822), and the pair of TPS tracks says whether the two
    # sensors that guard this body still agree.
    p.readout(live, 15, 14, 'Demand', '[$throttle_demand]', '%.1f', 110)
    p.readout(live, 130, 14, 'Position', f'[$etb_position_{st}]', '%.1f', 110)
    p.readout(live, 245, 14, 'Duty', f'[$etb_duty_{st}]', '%.1f', 110)
    p.readout(live, 360, 14, 'Pedal', '[$pedal_demand]', '%.1f', 110)
    lamps = (('UNCAL', f'[$etb_state_{st}] == 0', C_DIM),
             ('CALIBRATING', f'[$etb_state_{st}] == {CALIBRATING}', C_AMBER),
             ('RUNNING', f'[$etb_state_{st}] == {RUNNING}', C_GREEN),
             ('FAULT', f'[$etb_state_{st}] == {FAULT}', C_RED))
    for n, (title, expr, colour) in enumerate(lamps):
        p.add(p._new('indicator', 12 + n * 115, 74, 108, 38,
                     {'signalName': expr, 'onTitle': title, 'offTitle': title, 'onBg': colour,
                      'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    # Target over position, with duty under them: the gap between the first two is what the PID is
    # failing to close, which is the whole reason to watch this while a step runs.
    p.traces(15, 120, 450, 84, [('[$throttle_demand]', 0, 100), (f'[$etb_position_{st}]', 0, 100),
                                (f'[$etb_duty_{st}]', -100, 100)], into=live)

    return p


def page_cal_settings(i):
    """The knobs the three steps read — set once at installation, then never again.

    They were on the main page, eleven of them, around the three buttons that matter. A page of their own
    is not a demotion: it is the difference between a page you use and a page you read.
    """
    e = f'electronic_throttle.etb[{i}]'
    p = A.Page(title='Calibration Settings')
    y0 = p.head(f'What Steps 1 and 3 do while they run, for throttle body {NAME[i]}. The defaults suit a '
                f'normal drive-by-wire body; change them when a step fails, not before.')

    ac = p.panel(10, y0, 420, A.panel_h(4, A.ROW, top=12, bottom=6), 'Step 1 · Finding the stops')
    y = 12
    y = p.field(ac, 10, y, 'Duty Cap', f'{e}.ac_duty_cap_pct', 'configedit', 100, '%', lbl_w=180)
    y = p.field(ac, 10, y, 'Ramp Rate', f'{e}.ac_ramp_pct_s', 'configedit', 100, '%/s', lbl_w=180)
    y = p.field(ac, 10, y, 'Step Timeout', f'{e}.ac_timeout_ms', 'configedit', 100, 'ms', lbl_w=180)
    p.field(ac, 10, y, 'Motion Threshold', f'{e}.ac_move_min', 'configedit', 100, 'ADC', lbl_w=180)
    h = A.panel_h(4, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h + 6,
           'The duty cap is the safety limit on how hard the plate is pushed into its stop; motion '
           'threshold is the least sensor travel the sweep must see from stop to stop — with any '
           'less it fails, as a stuck, dead or miswired plate.', w=410)

    at = p.panel(450, y0, 420, A.panel_h(7, A.ROW, top=12, bottom=6), 'Step 3 · Autotuning the PID')
    y = 12
    for label, field, unit in (('Relay Amplitude', 'at_relay_pct', '%'),
                               ('Settle Band', 'at_settle_band_pct', '%'),
                               ('Settle Dwell', 'at_settle_dwell_ms', 'ms'),
                               ('Cycles', 'at_cycles', ''),
                               ('Settle Kp', 'at_settle_kp', ''),
                               ('Settle Ki', 'at_settle_ki', ''),
                               ('Settle Slew', 'at_slew_pct_s', '%/s')):
        y = p.field(at, 10, y, label, f'{e}.{field}', 'configedit', 100, unit, lbl_w=180)

    chk = p.panel(890, y0, 380, A.panel_h(3, A.ROW, top=12, bottom=6), 'Step 3 · Its safety checks')
    y = 12
    y = p.field(chk, 10, y, 'Check Duty', f'{e}.at_check_duty_pct', 'configedit', 100, '%', lbl_w=160)
    y = p.field(chk, 10, y, 'Check Minimum', f'{e}.at_check_min_pct', 'configedit', 100, '%', lbl_w=160)
    p.field(chk, 10, y, 'Relay Timeout', f'{e}.at_relay_timeout_ms', 'configedit', 100, 'ms', lbl_w=160)
    p.note(890, y0 + A.panel_h(3, A.ROW, top=12, bottom=6) + 6,
           'Before it tunes, the body is nudged with Check Duty and must move at least Check Minimum — a '
           'plate that cannot move is a mechanical fault, and oscillating it deliberately would only make '
           'the fault worse. Relay Timeout gives up on a cycle that never comes back.', w=370)

    y1 = y0 + A.panel_h(7, A.ROW, top=12, bottom=6) + 20
    live = p.panel(10, y1, 1260, A.CANVAS_H - y1 - 8, 'While It Runs')
    st = i + 1
    p.readout(live, 20, 14, 'Demand', '[$throttle_demand]', '%.1f', 120)
    p.readout(live, 150, 14, 'Position', f'[$etb_position_{st}]', '%.1f', 120)
    p.readout(live, 280, 14, 'Duty', f'[$etb_duty_{st}]', '%.1f', 120)
    p.wrapped(420, 20, 400,
              'Watch these while a step runs: a stop search that never sees motion, or a '
                               'relay cycle that will not settle, shows here before the button turns red.',
              into=live)
    p.add(p._new('label', 850, 20, 390, A.LBL_H,
                 {'labelText': f'Throttle Body {NAME[i]}', 'align': 'Left', 'fontName': A.FONT_LBL,
                  'link': f'{ETB}/{NODE[i]}'}), into=live)
    return p


def page_half_bridge(i):
    """One of the two bidirectional drivers: what commands it, which way round it is, and proof it moves.

    NOT a throttle page. The bridge does not know or care what is bolted to it — it realises whichever
    bus signal it is pointed at — and describing it as "the driver under throttle body A" was the reason
    it lived in the throttle branch, where nobody setting up a stepper would look for it.
    """
    h = f'h_bridge.half[{i}]'
    st = i + 1
    p = A.Page(title=f'Half Bridge {NAME[i]}')
    y0 = p.head('A bidirectional driver: it realises whichever bus signal it is pointed at. An electronic '
                'throttle servo, a PWM or BAC idle valve, a DC motor — or, with the other bridge, one '
                'coil of a bipolar stepper. There are two, and that is the whole supply.',
                enable=f'{h}.enabled')
    on = f'[#{h}.enabled] == 1'

    sig = p.panel(10, y0, 420, A.panel_h(2, A.ROW, top=12, bottom=6), 'Commanded By', enable=on)
    y = 12
    y = p.field(sig, 10, y, 'Demand Signal', f'{h}.demand_sig', 'enum', 200, lbl_w=140)
    p.field(sig, 10, y, 'Enable Signal', f'{h}.enable_sig', 'enum', 200, lbl_w=140)
    h1 = A.panel_h(2, A.ROW, top=12, bottom=6)
    p.note(10, y0 + h1 + 6,
           'Demand is the number the bridge drives to; Enable is the signal that permits it to drive at '
           'all — and a bridge drives only while it is actively enabled, which is what makes an '
           'unconfigured one safe. Whichever module owns the actuator publishes both: the throttle its '
           'etb_duty and etb_en, the stepper its coil demand and enable. A module with no bridge pointed '
           'at it moves nothing at all, however well it is tuned.', w=410)

    drv = p.panel(10, y0 + h1 + 128, 420, A.panel_h(5, A.ROW, top=12, bottom=6), 'Drive', enable=on)
    y = 12
    y = p.field(drv, 10, y, 'DC Map', f'{h}.dc_map', 'enum', 200, lbl_w=140)
    y = p.field(drv, 10, y, 'Max Authority', f'{h}.dc_max_pct', 'configedit', 100, '%', lbl_w=140)
    y = p.check(drv, 10, y + 2, 'Direction Invert', f'{h}.dir_invert')
    p.field(drv, 10, y + 4, 'PWM Frequency', 'h_bridge.pwm_freq_hz', 'configedit', 100, 'Hz', lbl_w=140)
    p.note(10, y0 + h1 + 128 + A.panel_h(5, A.ROW, top=12, bottom=6) + 6,
           'DC MAP follows the PRODUCER\'s sign convention, not the load: unipolar passes a signed demand '
           'straight through (-100..+100, which is what the throttle publishes), bipolar reads 0..100 with '
           '50 as stop. Getting it backwards is quiet and nasty — a signed demand read as bipolar gives '
           'zero drive at +50 and full reverse at -50, so a calibration sweep measures no travel at all. '
           'The PWM frequency is shared by BOTH halves.', w=410)

    # ---- prove it moves ------------------------------------------------------------------------
    test = p.panel(450, y0, 400, 270, 'Prove It Moves', enable=on)
    # The argument is a SIGNED DUTY (-100..100, 0 = off), mapped by this half's own dc_map / dc_max_pct /
    # dir_invert — which is why the note below can say what a one-way result means. 40% is enough to move
    # a throttle plate off either stop without slamming it, and dc_max_pct still caps it.
    for n, (label, arg) in enumerate((('Off', 0), ('Drive forward 40%', 40), ('Drive reverse 40%', -40))):
        p.add(p._new('command', 12, 14 + n * 52, 180, 40,
                     {'labelText': label, 'command': 'hbridge', 'arg0': str(i), 'arg1': str(arg),
                      'enableCondition': on}), into=test)
    p.wrapped(202, 14, 180,
              'Drives the bridge directly, with no controller in the way: the plate '
                               'should move one way, then the other. If it moves only one way, the DC '
                               'map is wrong; if it moves the wrong way, invert the direction. Each '
                               'press drives for four seconds and then releases on its own.',
              into=test)
    p.wrapped(12, 176, 370,
              'Engine stopped only — the ECU refuses to drive otherwise, and a start '
                               'drops it mid-nudge. This bypasses the throttle controller entirely.',
              into=test, colour=C_AMBER)

    # THE BRIDGE'S OWN CHANNELS, not the throttle module's. etb_duty/etb_en say what the controller
    # asked for; hbridge_duty/hbridge_en say what the pins are doing, after dc_map, dir_invert and the
    # authority clamp — and they are the only ones that move during a bench nudge, which runs with the
    # throttle module standing down. Position stays the throttle's: that is a sensor, and it is the
    # answer to "did the plate actually go anywhere".
    live = p.panel(450, y0 + 280, 400, 200, 'Right Now')
    p.readout(live, 15, 14, 'Duty', f'[$hbridge_duty_{st}]', '%.1f', 120)
    p.readout(live, 145, 14, 'Position', f'[$etb_position_{st}]', '%.1f', 120)
    p.add(p._new('indicator', 275, 14, 105, 44,
                 {'signalName': f'[$hbridge_en_{st}]', 'onTitle': 'DRIVING', 'offTitle': 'OFF',
                  'onBg': C_GREEN, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_SMALL}), into=live)
    p.wrapped(15, 74, 370,
              'Duty is what this bridge is driving and position is what came of it: '
                               'duty at its limit with no movement is a mechanical or wiring fault, not '
                               'a tuning one. DRIVING means these pins are live — during a bench nudge '
                               'it is lit while the throttle controller itself is standing down.',
              into=live)

    nxt = p.panel(870, y0, 400, A.panel_h(2, 28, top=12, bottom=6), 'Its Parts')
    y = 12
    y = p.switch(nxt, 10, y, f'Throttle Body {NAME[i]}  (what it drives)', '',
                 link=f'{ETB}/{NODE[i]}', w=350, pitch=28)
    p.switch(nxt, 10, y, f'Half Bridge {NAME[1 - i]}  (the other one)', '',
             link=f'{HB}/Half Bridge {NAME[1 - i]}', w=350, pitch=28)
    return p


def page_hb_branch():
    """The branch page: what the two bridges ARE, and the one thing about them that runs out."""
    p = A.Page(title='Half Bridges')
    y0 = p.head('Two bidirectional drivers — the only ones the ECU has, and the third kind of output '
                'hardware beside the 22 low-side and 8 high-side drivers next door.')
    box = p.panel(10, y0, 420, A.panel_h(3, 28, top=12, bottom=6), 'Bridges')
    y = 12
    for i in (0, 1):
        y = p.switch(box, 10, y, f'Half Bridge {NAME[i]}', f'h_bridge.half[{i}].enabled',
                     link=f'{HB}/Half Bridge {NAME[i]}', w=350, pitch=28)
    p.field(box, 10, y + 2, 'PWM Frequency', 'h_bridge.pwm_freq_hz', 'configedit', 100, 'Hz', lbl_w=160)

    uses = p.panel(10, y0 + A.panel_h(3, 28, top=12, bottom=6) + 12, 420, 240, 'What They Can Drive')
    p.wrapped(12, 14, 396,
              'Each bridge is a full H-bridge — PWM, direction and enable — so on its own it drives an '
              'electronic throttle servo, a PWM or BAC idle valve, or any DC motor that needs to go both '
              'ways. It does not know what is bolted to it: it realises whichever bus signal its Demand '
              'points at, and every integrator, ramp and limit belongs to the module publishing that '
              'signal.', into=uses)
    p.wrapped(12, 108, 396,
              'AS A PAIR they drive one bipolar stepper, one coil each — an idle valve, an electronic '
              'wastegate, anything stepped. That uses BOTH, so a stepper and an electronic throttle '
              'cannot share this ECU; two throttle bodies use both as well. Two is the whole supply.',
              into=uses, colour=C_AMBER)

    live = p.panel(450, y0, 420, 210, 'Right Now')
    # THE BRIDGES' OWN CHANNELS, not the throttle's. etb_duty says what a controller asked for;
    # hbridge_duty says what these pins are doing — which is the only honest readout on a page that is
    # no longer about throttles.
    p.readout(live, 15, 14, 'A Duty', '[$hbridge_duty_1]', '%.1f', 120)
    p.readout(live, 145, 14, 'B Duty', '[$hbridge_duty_2]', '%.1f', 120)
    p.add(p._new('indicator', 275, 14, 60, 44,
                 {'signalName': '[$hbridge_en_1]', 'onTitle': 'A', 'offTitle': 'A',
                  'onBg': C_GREEN, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_SMALL}), into=live)
    p.add(p._new('indicator', 340, 14, 60, 44,
                 {'signalName': '[$hbridge_en_2]', 'onTitle': 'B', 'offTitle': 'B',
                  'onBg': C_GREEN, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_SMALL}), into=live)
    p.wrapped(15, 76, 390,
              'Duty is what each bridge is driving, after its DC map, direction and authority clamp — '
              'not what a controller asked for. A lit letter means those pins are live. A bridge with no '
              'demand signal assigned drives nothing, however well the module above it is tuned.',
              into=live)
    return p


def page_etb_branch():
    p = A.Page(title='Electronic throttle')
    y0 = p.head('Drive-by-wire: two throttle bodies, each with its own H-bridge, position feedback and '
                'calibration. Tick what this car has.')
    box = p.panel(10, y0, 460, A.panel_h(4, 28, top=12, bottom=6), 'Throttle Bodies')
    y = 12
    for i in (0, 1):
        y = p.switch(box, 10, y, f'Throttle Body {NAME[i]}', f'electronic_throttle.etb[{i}].enabled',
                     link=f'{ETB}/{NODE[i]}', w=390, pitch=28)
    for i in (0, 1):
        y = p.switch(box, 10, y, f'Half Bridge {NAME[i]}', f'h_bridge.half[{i}].enabled',
                     link=f'{HB}/Half Bridge {NAME[i]}', w=390, pitch=28)

    auth = p.panel(490, y0, 380, A.panel_h(3, A.ROW, top=12, bottom=6), 'Authority')
    y = 12
    y = p.field(auth, 10, y, 'Idle Authority', 'electronic_throttle.idle_authority', 'configedit', 100,
                '%', lbl_w=160)
    y = p.field(auth, 10, y, 'Traction Cap', 'electronic_throttle.traction_cap_src', 'enum', 150, lbl_w=160)
    p.field(auth, 10, y, 'Torque Cap', 'electronic_throttle.torque_cap_src', 'enum', 150, lbl_w=160)

    live = p.panel(890, y0, 380, 210, 'Right Now')
    p.readout(live, 15, 14, 'A Position', '[$etb_position_1]', '%.1f', 110)
    p.readout(live, 130, 14, 'B Position', '[$etb_position_2]', '%.1f', 110)
    p.readout(live, 245, 14, 'Pedal', '[$app_1]', '%.1f', 110)
    p.wrapped(15, 78, 350,
              'A body is set up in one order: assign its half bridge, prove the plate '
                               'moves both ways, then Find limits, Fill feed-forward and Autotune — with '
                               'the engine stopped, in that order, on the body\'s own page.',
              into=live)
    return p


# ---- the input side: the accelerator pedal -------------------------------------------------------

PEDAL_NODE = f'{CFG}/Engine Functions/Accelerator Pedal'


def page_pedal_map():
    """The pedal map at the size it can grow to — 16x16 over four gear planes.

    It lived in a 430x160 box on the pedal page, which is 46% of the width and 41% of the height the
    grid needs for its full allocation. Sized from author.table_box, so the box is what the table can
    become rather than what it ships at: the axes are the tuner's to lengthen, and a page authored for
    today's bins is a scroll bar the day somebody adds one.
    """
    a = 'app'
    p = A.Page(title='Pedal to Throttle')
    y0 = p.head('Pedal percent in, throttle percent out — and a plane of it per gear. A straight line is '
                'a pedal that commands what it is pressed to; bending it down low is what makes a car '
                'driveable on a big throttle body.')
    tw, th = A.table_box(f'{a}.pedal_to_throttle_table', fallback=(938, 390))
    tw = min(tw, A.CANVAS_W - 20)
    th = min(th, A.CANVAS_H - y0 - 130)
    p.table(10, y0, tw, th, f'{a}.pedal_to_throttle_table', anchor_x='both', anchor_y='both')
    p.add(p._new('curve', 10, y0 + th + 10, tw, A.CANVAS_H - y0 - th - 76,
                 {'signalName': f'{a}.pedal_to_throttle_table', 'displayUnit': 'Auto',
                  'axisUnit': 'Auto', 'valueUnit': 'Auto'}))
    live = p.panel(10, A.CANVAS_H - 62, tw, 54, '')
    p.readout(live, 15, 4, 'Pedal to Throttle', f'{a}.pedal_to_throttle_table',
              A.table_fmt(f'{a}.pedal_to_throttle_table'), 150)
    p.readout(live, 170, 4, 'Track A', '[$app_1]', '%.1f', 130)
    p.readout(live, 305, 4, 'Track B', '[$app_2]', '%.1f', 130)
    p.readout(live, 440, 4, 'Demand', '[$pedal_demand]', '%.1f', 130)
    return p


def page_pedal():
    """The pedal: two tracks that must agree, a calibration that learns their span, and the map from
    pedal to throttle.

    Same shape as a throttle body, because it is the same problem from the other end (App.h says so: "the
    INPUT-side counterpart to the ETB actuator"). Two analogue tracks are cross-checked, track A is mapped
    through the pedal-to-throttle table to publish pedal_demand, and a disagreement fails the demand SAFE
    to zero rather than to whatever the good track says.
    """
    a = 'app'
    p = A.Page(title='Accelerator Pedal')
    y0 = p.head('The driver\'s request. Two pedal tracks that must agree, the calibration that learns their '
                'travel, and the map from pedal to throttle.', enable=f'{a}.enabled')
    on = f'[#{a}.enabled] == 1'

    # ---- the two tracks ------------------------------------------------------------------------
    tr = p.panel(10, y0, 330, A.panel_h(4, A.ROW, top=12, bottom=6), 'The Two Tracks', enable=on)
    y = 12
    y = p.field(tr, 10, y, 'Track A Sense', f'{a}.app1_sense', 'enum', 190, lbl_w=120)
    y = p.field(tr, 10, y, 'Track B Sense', f'{a}.app2_sense', 'enum', 190, lbl_w=120)
    y = p.field(tr, 10, y, 'Match Limit', f'{a}.match_err_pct', 'configedit', 100, '%', lbl_w=120)
    p.field(tr, 10, y, 'Match Debounce', f'{a}.match_ms', 'configedit', 100, 'ms', lbl_w=120)
    h1 = A.panel_h(4, A.ROW, top=12, bottom=6)
    n1 = p.note(10, y0 + h1 + 6,
                'SENSE is declared, not discovered: a calibration sweep sees the same extremes whether '
                'the pedal was pressed then released or the other way round, so nothing in the signals '
                'can tell the studio which end is "released". A redundant pedal usually runs its second '
                'track the opposite way, so a short between the two is detectable — which is why B '
                'defaults to falling.', w=320)

    # ---- what the sensors themselves are -------------------------------------------------------
    # UNDER THE NOTE, not a literal 140px under the panel: a guess one line short lands the next panel
    # on the note's last line.
    y1 = n1['y'] + n1['h'] + 6
    # TWO DIFFERENT SWITCHES, said out loud. The one at the top of this page runs the MODULE; these two
    # run the INPUTS it reads. Neither implies the other — the sensors can be on with the pedal function
    # off — and the page that had only the module's switch left the difference to be discovered.
    SENS_NOTE = ('These are the INPUTS. The switch at the top of the page is the pedal function itself — '
                 'it needs both of them.')
    sens = p.panel(10, y1, 330,
                   A.panel_h(2, 28, top=12, bottom=6) + A.Page.wrapped_h(SENS_NOTE, 310) + 8,
                   'Its Sensors', enable=on)
    y = 12
    for label, sid in (('Accelerator Pedal', 'app_1'), ('Accelerator Pedal 2', 'app_2')):
        y = p.switch(sens, 10, y, label, f'sensors.sensor[{sid}].enabled',
                     link=f'{CFG}/Sensors/Engine/{label}', w=280, pitch=28)
    p.wrapped(10, y + 4, 310, SENS_NOTE, into=sens)

    # ---- pedal to throttle ---------------------------------------------------------------------
    # THE CURVE HERE, THE GRID ON ITS OWN PAGE. The map is allocated 16x16 (and four gear planes on top
    # of that): 938x390 to show every cell it can hold, against the 430x160 this panel had room for —
    # 41% of the height, so most of the rows a tuner could add would be behind a scroll bar on a page
    # that looks like it is showing them the table. It was the only crammed grid in the document with
    # nowhere else to go, so it has a page now.
    #
    # What stays is the CURVE, which is the view worth having beside the calibration and the live
    # tracks: the shape of the pedal is the question this page asks, and a curve answers it at a glance
    # where a grid of numbers does not.
    map_ = p.panel(350, y0, 460, 576, 'Pedal to Throttle', enable=on)
    p.switch(map_, 10, 10, 'Pedal to Throttle Table', '', link=f'{PEDAL_NODE}/Pedal to Throttle', w=430)
    p.add(p._new('curve', 10, 48, 430, 430,
                 {'signalName': f'{a}.pedal_to_throttle_table', 'displayUnit': 'Auto', 'axisUnit': 'Auto',
                  'valueUnit': 'Auto'}), into=map_)
    p.wrapped(10, 486, 430,
              'Pedal percent in, throttle percent out. A straight line is a pedal that '
                               'commands what it is pressed to; bending it down low is what makes a car '
                               'driveable on a big throttle body.',
              into=map_)

    # NO AXIS PANEL. What each axis reads, how many bins it has and what it spans belong to the TABLE, and
    # the table already offers them on its own context menu (Axis Setup…). Repeating them as a panel of
    # pickers is a second place to change the same thing, and the page is about the pedal.

    # ---- calibrate -----------------------------------------------------------------------------
    cal = p.panel(820, y0, 450, 250, 'Calibrate', enable=on)
    p.add(p._new('command', 10, 12, 200, 40,
                 {'labelText': 'Calibrate pedal', 'command': 'pedalcal',
                  'enableCondition': '[$app_state] != 1',
                  'ranges': (f'{C_AMBER},,,,0,[$command_state] == {_code(PEDALCAL_OP, 1)};'
                             f'{C_GREEN},,,,0,[$command_state] == {_code(PEDALCAL_OP, 2)};'
                             f'{C_RED},,,,0,[$command_state] == {_code(PEDALCAL_OP, 3)}')}), into=cal)
    p.wrapped(220, 10, 220,
              'Engine stopped. Press the pedal fully and release it while it runs.',
              into=cal)
    p.field(cal, 10, 62, 'Minimum Span', f'{a}.cal_min_span', 'configedit', 100, 'ADC', lbl_w=140)
    p.wrapped(10, 100, 430,
              'The sweep captures each track\'s raw extremes, in either order — pressing '
                               'first and releasing first give the same four numbers, which is why the '
                               'direction comes from the sense above instead. A pedal that moves less '
                               'than the minimum span did not really move, and the calibration aborts '
                               'rather than writing a span that would make a twitch read as full '
                               'throttle. While it runs, demand is held at zero.',
              into=cal)

    # ---- live ----------------------------------------------------------------------------------
    live = p.panel(820, y0 + 260, 450, A.CANVAS_H - (y0 + 260) - 8, 'Right Now')
    p.readout(live, 15, 14, 'Track A', '[$app_1]', '%.1f', 130)
    p.readout(live, 150, 14, 'Track B', '[$app_2]', '%.1f', 130)
    p.readout(live, 285, 14, 'Demand', '[$pedal_demand]', '%.1f', 130)
    lamps = (('OK', '[$app_state] == 0', C_GREEN),
             ('CALIBRATING', '[$app_state] == 1', C_AMBER),
             ('A/B DISAGREE', '[$app_state] == 2', C_RED),
             ('NO SIGNAL', '[$app_state] == 3', C_RED))
    for n, (title, expr, colour) in enumerate(lamps):
        p.add(p._new('indicator', 12 + n * 110, 76, 104, 38,
                     {'signalName': expr, 'onTitle': title, 'offTitle': title, 'onBg': colour,
                      'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                      'fontName': A.FONT_SMALL}), into=live)
    p.traces(15, 122, 420, 96, [('[$app_1]', 0, 100), ('[$app_2]', 0, 100),
                                ('[$pedal_demand]', 0, 100)], into=live)
    p.wrapped(15, 224, 420,
              'The two tracks should track each other exactly; demand follows track A '
                               'through the map. Disagreement past the limit fails demand to ZERO — the '
                               'engine idles rather than guessing which track to believe.',
              into=live)
    return p
