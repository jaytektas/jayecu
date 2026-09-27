"""One page per sensor: what it is wired to, what its curve says, and what it must complain about.

Written ONCE, with `[*]` bindings, and stored under every sensor's node — the viewport that shows it
carries the element (`sensors.sensor[clt]`), which is what "[*]" resolves to. That is the same shape the
four hand-authored sensor pages used; this generates the other 118 as well, so a sensor is no longer a
tree node with nothing behind it.

Three views on one page rather than three nodes in the tree (Page.subtabs): a sensor is one thing, and
its calibration, its checks and its wiring are three questions about that thing, not three places to go.
The reading stays on screen across all three, because every one of those questions is asked while
watching it.
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from author import C_DIM, C_RED, C_AMBER

S = 'sensors.sensor[*]'          # the element the hosting viewport supplies
ON = f'[#{S}.enabled] == 1'      # the whole page greys when the sensor is switched off

IF_ANALOG, IF_SYNC, IF_ONBOARD, IF_FREQ, IF_SWITCH, IF_CAN, IF_SENT, IF_PULSE = range(8)
PIN_IFACES = (IF_ANALOG, IF_SYNC, IF_FREQ, IF_SWITCH, IF_SENT, IF_PULSE)

# The six checks, in the order they run: the electrical pair on the raw input, the plausibility pair on
# the calibrated reading, then the two about how the value BEHAVES. Each carries its own threshold field
# and its own severity — severity is two bits PER CHECK, not one setting for the sensor, which is why a
# single "Severity" box could only ever show 0.
#
# A check is only offered when this sensor has a DTC code for it: the catalogue assigns them per sensor
# per check, and a check with no code cannot report anything it finds. That is also what keeps Detect
# Stuck off a throttle position — it is a switch-input check, and a percent sensor has no code for it.
CHECKS = [
    ('raw_min',    'Detect Raw Low',      'diag_raw_min',   'electrical'),
    ('raw_max',    'Detect Raw High',     'diag_raw_max',   'electrical'),
    ('op_min',     'Detect Reading Low',  'diag_op_min',    'plausibility'),
    ('op_max',     'Detect Reading High', 'diag_op_max',    'plausibility'),
    ('stuck',      'Detect Stuck',        'diag_stuck_ms',  'behaviour'),
    ('max_deriv',  'Detect Rate Spike',   'diag_max_deriv', 'behaviour'),
]
GROUP_TITLE = {'electrical':  'Electrical  (raw input, before calibration)',
               'plausibility': 'Plausibility  (calibrated reading)',
               'behaviour':   'Behaviour'}

# What each check actually catches. Two of these names are one word apart and a world apart: raw low is a
# broken wire, reading low is a sensor that works and is saying something implausible.
MEANS = {
    'raw_min':   'The pin reads below this — an open circuit, or a short to ground.',
    'raw_max':   'The pin reads above this — a short to power, or a missing pull-down.',
    'op_min':    'The calibrated reading is below what this engine could plausibly produce.',
    'op_max':    'The calibrated reading is above what this engine could plausibly produce.',
    'stuck':     'The reading has not moved at all for this long — a switch input that never changes.',
    'max_deriv': 'The reading changed faster than the physical thing behind it can change.',
}


def page(checks=None, dtc=None, type_label='', diag_link=''):
    """One sensor, read left to right the way the signal travels: INPUT -> CALIBRATION -> OUTPUT.

    That is the whole page in one sentence. A pin carries a number; a curve says what that number means;
    the ECU acts on what comes out. Laying the three side by side in that order means the page explains
    the sensor rather than merely listing its settings — and it puts the raw value next to the wiring
    that produces it, and the reading next to the type that gives it units.

    THE CHECKS ARE A PAGE OF THEIR OWN. They were a full-width strip across the bottom of this one, which
    left the calibration — the reason anyone opens a sensor — sharing the page with six rows of threshold
    fields, and squeezed the curve into 176px. They are also a different question, asked at a different
    time: the curve is set up once from a data sheet, the checks are tuned when something starts reporting
    a fault it should not. A tree node costs nothing and the page each of them gets is worth more than the
    half-page both were sharing.

    No dial. A 190px round gauge on a tuning page is decoration: it is harder to read than the number it
    duplicates, and the number is already there. Dials earn their place on a dashboard, at dashboard size.

    `checks` — the bit names this sensor has a DTC for; `dtc` — {bit: code}; `type_label` — the fixed type
    for the 108 sensors whose type the catalogue decides; `diag_link` — the node its checks live on. All
    are per-sensor facts.
    """
    p = A.Page(title='Sensor')
    y0 = p.head('What this input is wired to, and what its curve makes of it.', enable=f'{S}.enabled')
    h = A.CANVAS_H - y0 - 8
    _input(p, y0, h)
    _calibration(p, y0, h)
    _output(p, y0, h, type_label, diag_link, checks or [c[0] for c in CHECKS])
    return p


def diag_page(checks=None, dtc=None):
    """The same sensor's CHECKS: what it must complain about, when, and with which code."""
    p = A.Page(title='Diagnostics')
    y0 = p.head('What this input must complain about — and with which code. A check with no code is one '
                'this sensor cannot report, and is not offered.', enable=f'{S}.enabled')
    _diagnostics(p, y0, checks or [c[0] for c in CHECKS], dtc or {})
    return p


def _input(p, y, h):
    """1 — what arrives: the pin, how it is read, and the number on it."""
    box = p.panel(10, y, 300, h, '1 · Input', enable=ON)
    # NO FORMAT, and now that actually means something. '%.0f' was authored when this could only be ADC
    # counts; then an unset format fell through to CanvasWidget's '%.1f' last resort and it read '3.8V'.
    # Precision now lives on the UNIT (UnitManager::Unit::digits — V is 3, mV is 0, counts is 0), so an
    # unset format lets this readout re-scale itself when the run-mode Units menu switches the pin
    # between volts, millivolts and counts. A format string here could only ever be right for one of them.
    p.add(p._new('value', 12, 10, 274, 54,
                 {'signalName': '[$~]', 'fontName': '|26|0|0', 'align': 'Center',
                  'borderWidth': '1', 'borderRadius': '4'}), into=box)
    p.add(p._new('label', 12, 66, 274, 18,
                 {'labelText': 'Raw input  (as the pin reads it)', 'align': 'Center',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=box)
    ry = p.field(box, 10, 96, 'Interface', f'{S}.interface', 'enum', 150, lbl_w=90)
    pin = ' || '.join(f'[#{S}.interface] == {i}' for i in PIN_IFACES)
    p.add(p._new('wiring', 10, ry + 6, 274, 76, {'signalName': f'{S}.source', 'condition': pin}), into=box)
    # A CAN sensor owns no pin. Its reading comes from a generic CAN receive FIELD, laid out on
    # Configuration > CAN Bus > CANn > Receive — this only says which of them is this sensor, so the
    # choice is next to the interface that gives it meaning rather than buried on a page that knows
    # nothing about sensors.
    #
    # ONE CONTROL OVER TWO SETTINGS. The tune stores the frame (`can_frame`, its bus/ext/id as one key)
    # and the field's start bit (`can_bit`) — a pair, and neither a number anyone would type. The
    # picker binds the frame and derives the start bit as its sibling, and offers the fields by the
    # frame's NAME. It replaced a spin box over the field's index in the 512-deep pool, which nobody
    # could read and which the studio silently renumbered on every structural edit of the frames.
    ry = p.field(box, 10, ry + 90, 'CAN Field', f'{S}.can_frame', 'canfield', 210, lbl_w=90,
                 cond=f'[#{S}.interface] == {IF_CAN}')
    ry = p.check(box, 10, ry + 6, 'Invert  (switch inputs)', f'{S}.flags.invert',
                 cond=f'[#{S}.interface] == {IF_SWITCH}')
    p.wrapped(10, ry + 6, 274,
              'Interface says HOW the pin is read, and decides which inputs the picker offers — a '
              'frequency sensor cannot be wired to an analogue pin. Two sensors on one pin is a '
              'fault, not a preference. A CAN sensor has no pin at all: it reads a field defined on '
              'the Receive page for its bus, named here by that frame and the field\'s start bit. A field '
              'whose own Signal is set is greyed in the list \u2014 the frame writes that channel itself, and '
              'the sensor publishing it too would put two producers on one channel.', into=box, colour=C_DIM)


def _calibration(p, y, h):
    """2 — what it means: the curve that turns the raw number into a reading.

    The table and the curve are the same points, because a calibration is numbers before it is a shape:
    a data sheet gives volts and kPa, and typing them is the job. With the checks moved to their own node
    the curve gets the height it needs to be read as a shape rather than glanced at as a squiggle — which
    is the whole reason to draw it beside the numbers.
    """
    cal = p.panel(320, y, 630, h, '2 · Calibration', enable=ON)
    inner = h - A.PANEL_TITLE - 4
    p.add(p._new('table', 10, 10, 600, 96,
                 {'signalName': f'{S}.cal', 'axisMode': '1', 'displayUnit': 'Auto',
                  'displayUnitX': 'Auto', 'displayUnitY': 'Auto', 'cellTrace': '2'}), into=cal)
    # The curve stops where the note starts. It used to claim down to inner-44 while the note began at
    # inner-68, so the note's first line was drawn across the curve's own bottom axis.
    p.add(p._new('curve', 10, 114, 600, inner - 114 - 72,
                 {'signalName': f'{S}.cal', 'displayUnit': 'Auto', 'axisUnit': 'Auto',
                  'valueUnit': 'Auto'}), into=cal)
    note = p.wrapped(10, inner - 68, 600,
                     'Raw in along the top, reading out underneath. Right-click either for insert, delete, '
                     'linearise — or to save this curve to a file and load it into the next car with the '
                     'same sensor.', into=cal)
    # BOUND TO THE CALIBRATION, so it goes where the curve goes. A caption is shown or hidden with the
    # calibration it describes (CanvasWidget::showsCalibrationOf) only if it says which one it describes;
    # unbound, this sentence about a curve stayed on screen over a switch that has no curve.
    note['props']['signalName'] = f'{S}.cal'
    # A MULTI-POSITION SWITCH'S BANDS, in the same place. Its calibration is one voltage band per
    # position, not a curve; the band editor shows for that calibration and the table, curve and note
    # above hide for it, so the two never share the panel.
    p.add(p._new('bands', 10, 10, 600, inner - 20, {'signalName': f'{S}.cal'}), into=cal)


def _output(p, y, h, type_label, diag_link, checks):
    """3 — what the ECU acts on: the reading, the type that gives it its units and domain, and the way to
    the checks that watch it."""
    out = p.panel(960, y, 310, h, '3 · Output', enable=ON)
    p.add(p._new('value', 12, 10, 284, 62,
                 {'signalName': '[$*]', 'format': '%.2f', 'fontName': '|30|0|0', 'align': 'Center',
                  'borderWidth': '1', 'borderRadius': '4'}), into=out)
    p.add(p._new('label', 12, 74, 284, 18,
                 {'labelText': 'Reading  (what everything downstream sees)', 'align': 'Center',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=out)
    if type_label:
        g = p.group()
        p.add(p._new('label', 10, 106, 90, A.LBL_H,
                     {'labelText': 'Type', 'align': 'Left', 'fontName': A.FONT_LBL}, g), into=out)
        p.add(p._new('label', 106, 106, 190, A.LBL_H,
                     {'labelText': type_label, 'align': 'Left', 'fontName': A.FONT_LBL}, g), into=out)
        note = ("This input's type is fixed by the ECU — it is what the sensor IS. Only the generic "
                "inputs (aux, rotary trim) can be re-typed.")
    else:
        p.field(out, 10, 102, 'Type', f'{S}.type', 'enum', 150, lbl_w=90)
        note = ('The type decides the units, the sensible domain and how many points the curve needs. '
                'Changing it re-seeds the curve.')
    p.wrapped(10, 142, 284, note, into=out)
    p.wrapped(10, 214, 284,
              'Input and output are both on screen because a calibration is a claim about how one '
              'becomes the other: watch the pair move together and a wrong curve shows itself '
              'immediately.', into=out)
    # THE WAY TO THE CHECKS. The tree has them as this sensor's child, and a page that sends the reader
    # somewhere should say where from — here, under the value the checks are watching.
    if diag_link:
        p.add(p._new('panel', 10, 308, 284, 1, {'bgColor': C_DIM, 'padding': '0'}), into=out)
        p.add(p._new('label', 10, 318, 284, A.LBL_H,
                     {'labelText': f'Diagnostics  ({len(checks)} check'
                                   f'{"" if len(checks) == 1 else "s"})',
                      'align': 'Left', 'fontName': A.FONT_LBL, 'link': diag_link}), into=out)
        p.wrapped(10, 342, 284,
                  'What this input must complain about, when, and with which code.', into=out)


def _diagnostics(p, y, checks, dtc):
    """What this input must complain about — and with which code.

    Every row is one check: its switch, the threshold it trips at, how serious that is, and the DTC it
    raises. Units are left to the widgets: a raw threshold is in the input's own units, a reading
    threshold in the sensor type's.

    Each row says what it MEANS underneath, because "Detect Raw Low" and "Detect Reading Low" are two
    words apart and a world apart: one is a broken wire, the other a sensor that works and is telling you
    something implausible. Getting them the wrong way round is how a fault ends up reported as the wrong
    thing entirely — and there is room to say so, now that the checks have a page.
    """
    # SIZED TO WHAT THIS SENSOR HAS. Some inputs carry six checks and some carry two, and a panel stretched
    # to the page bottom either way leaves a switch sensor's page mostly empty box — which reads as
    # something missing rather than as a sensor with little to complain about.
    shown = [c for c in CHECKS if c[0] in checks]
    rows_h = 34 + len({g for _b, _l, _f, g in shown}) * 22 + len(shown) * 46
    # …and never shorter than the column beside it, which carries the delay, the precondition and the
    # note whatever the check list says. A.panel_h adds what the title bar takes.
    h = min(A.CANVAS_H - y - 8, A.panel_h(1, max(rows_h, 262), top=0, bottom=8))
    dia = p.panel(10, y, 1260, h, 'Checks', enable=ON)

    # The column heads. Three controls in a row with nothing naming them is what made a severity picker
    # look like a box that says 0.
    for cx, cw, text in ((34, 200, 'Check'), (240, 96, 'Trips at'), (346, 110, 'Severity'),
                         (462, 60, 'Code')):
        p.add(p._new('label', cx, 6, cw, 16,
                     {'labelText': text, 'align': 'Left', 'fontName': '|16|1|0', 'fgColor': C_DIM}),
              into=dia)
    p.add(p._new('panel', 10, 26, 512, 1, {'bgColor': C_DIM, 'padding': '0'}), into=dia)

    ry, seen = 34, set()
    for bit, label, field, group in CHECKS:
        if bit not in checks:
            continue
        if group not in seen:
            seen.add(group)
            p.add(p._new('label', 10, ry, 500, 18,
                         {'labelText': GROUP_TITLE[group], 'align': 'Left', 'fontName': '|15|1|0',
                          'fgColor': C_DIM}), into=dia)
            ry += 22
        on = f'[#{S}.diag_enable.{bit}] == 1'
        g = p.group()
        p.add(p._new('checkbox', 10, ry + 2, A.CHECK_H, A.CHECK_H, {'signalName': f'{S}.diag_enable.{bit}'}, g), into=dia)
        p.add(p._new('label', 10 + A.CHECK_H + A.CHECK_GAP, ry, 194, A.LBL_H,
                     {'labelText': label, 'align': 'Left', 'fontName': A.FONT_LBL}, g), into=dia)
        p.add(p._new('configedit', 240, ry, 96, A.CTL_H,
                     {'signalName': f'{S}.{field}', 'displayUnit': 'Auto', 'enableCondition': on}, g),
              into=dia)
        p.add(p._new('enum', 346, ry, 110, A.CTL_H,
                     {'signalName': f'{S}.diag_severity.{bit}', 'enableCondition': on}, g), into=dia)
        if dtc.get(bit):
            p.add(p._new('label', 462, ry + 4, 60, A.LBL_H,
                         {'labelText': dtc[bit], 'align': 'Left', 'fontName': A.FONT_LBL,
                          'fgColor': C_DIM, 'enableCondition': on}, g), into=dia)
        # 515, not 480: the longest of these (Detect Stuck's) is 494px of text, so at 480 it lost its
        # last two words. There is room to 554 before the second column starts, and one line is all this
        # row has — the next check's tick is 46px below it.
        p.add(p._new('label', 34, ry + 25, 515, 16,
                     {'labelText': MEANS[bit], 'align': 'Left', 'fontName': A.FONT_SMALL,
                      'fgColor': C_DIM}, g), into=dia)
        ry += 46

    # WHEN it is allowed to complain, beside WHAT it complains about.
    ry2 = p.field(dia, 560, 34, 'DTC Delay', f'{S}.diag_delay_ms', 'configedit', 110, 'ms', lbl_w=140)
    p.wrapped(560, ry2 + 4, 400,
              'How long a check has to keep failing before it becomes a fault. A sensor that blips '
              'once on a starter crank is not a broken sensor.', into=dia)
    p.add(p._new('label', 560, ry2 + 46, 400, 18,
                 {'labelText': 'Precondition', 'align': 'Left', 'fontName': '|15|1|0', 'fgColor': C_DIM}),
          into=dia)
    p.add(p._new('expression', 560, ry2 + 68, 400, A.CTL_H, {'signalName': f'{S}.precond_expr'}), into=dia)
    p.wrapped(560, ry2 + 100, 400,
              'The plausibility checks only run while this holds — an oil pressure of zero is a fault '
              'at 3000 rpm and the truth with the engine stopped.', into=dia)
    p.wrapped(990, 34, 250,
              'The electrical pair tests the RAW input in the input\'s own units, before '
                               'calibration; the plausibility pair tests the calibrated reading. Severity '
                               'is what the DTC records and what the protection levels read. A check with '
                               'no code is one this sensor cannot report, and is not offered — which is '
                               'why two sensors show different rows here.',
              into=dia)
