"""The Fuel branch: its tree shape and the pages that hang off it.

Ordering is most-used and simplest first, least-used and most complicated last, so the branch reads as the order you actually do the work in: set the engine
and the model up, fill the VE table, give it a target, get it starting and warming up, then the injector
data, then the corrections, then the trims.

Every page is laid out here by hand; author.py only holds the pen.
"""
import author as A
from author import C_DIM, C_RED, C_AMBER, C_GREEN, C_BLUE

CFG = 'Configuration'
FC = 'fuel_calculator.'
ENG = 'engine.'
TT = 'transient_throttle.'


def _live_strip(p, y=498, h=202, title='Live'):
    """The strip along the bottom of a setup page: what the engine is doing while you edit it."""
    # NEVER PAST THE SHEET. The height is a literal and the y it starts at is not always the one this
    # default assumes, so it is clamped to what is left — a strip two pixels past the bottom is a page
    # that reports overflow and grows a scroll bar for nothing.
    panel = p.panel(10, y, 1260, min(h, A.CANVAS_H - y), title)
    cols = [
        ('Engine RPM',   'rpm',           '%.0f'),
        ('MAP',          'map',           '%.0f'),
        ('Throttle',     'tps',           '%.1f'),
        ('Coolant',      'clt',           '%.0f'),
        ('Air Temp',     'iat',           '%.0f'),
        ('Lambda',       'lambda_1',      '%.2f'),
        ('Target',       'lambda_target', '%.2f'),
        ('VE',           've',            '%.0f'),
        ('Inj PW',       'inj_pw',        '%.2f'),
        ('Fuel Press',   'fuel_pressure', '%.0f'),
        ('Ethanol',      'ethanol',       '%.0f'),
    ]
    for i, (lbl, ch, fmt) in enumerate(cols):
        p.readout(panel, 12 + i * 112, 14, lbl, ch, fmt)
    # The correction CHAIN, in the order the firmware applies it — the answer to "why is it rich here?"
    # is usually one of these being somewhere you did not expect, and they are otherwise invisible.
    chain = [('Warmup', 'fuel_corr_warmup'), ('Air Temp', 'fuel_corr_iat'), ('MAP', 'fuel_corr_map'),
             ('Baro', 'fuel_corr_baro'), ('Fuel Comp', 'fuel_corr_fuelcomp'), ('Accel', 'fuel_corr_accel'),
             ('STFT', 'fuel_corr_stft'), ('LTFT', 'fuel_corr_ltft'), ('Rev Limit', 'fuel_corr_revlimit'),
             ('Protection', 'fuel_corr_protection'), ('Overall', 'fuel_corr_overall')]
    p.add(p._new('label', 12, 92, 300, 20,
                 {'labelText': 'Fuel correction chain  (x1.00 = doing nothing)', 'align': 'Left',
                  'fontName': A.FONT_SMALL, 'fgColor': C_DIM}), into=panel)
    for i, (lbl, ch) in enumerate(chain):
        p.readout(panel, 12 + i * 112, 116, lbl, ch, '%.3f')
    return panel


def page_setup():
    p = A.Page(title='Fuel Setup')
    p.head('What the engine is, how its air mass is worked out, and which signals feed the model.')

    # WHAT THE MODEL READS. Each source and constant below is gated on the air model that actually uses
    # it, quoting the field's own help: the blend crossover exists only in Blend, MAF is "used only by the
    # MAF model", TPS is "the load axis for alpha-N, and half of the blend model's predicted-MAP lookup".
    # Greyed, not hidden — the setting still exists, it just has no effect in this mode.
    air = p.panel(10, A.TOP, 300, 286, 'Air Model')
    y = 10
    y = p.field(air, 10, y, 'Air Model', FC + 'fuel_model', 'enum', 240)
    blend = '[#fuel_calculator.fuel_model] == 3'   # the crossover only exists in Blend
    y = p.field(air, 10, y, 'Blend Start RPM', FC + 'blend_rpm_lo', 'configedit', 110, 'RPM', enable=blend)
    y = p.field(air, 10, y, 'Blend End RPM', FC + 'blend_rpm_hi', 'configedit', 110, 'RPM', enable=blend)
    p.field(air, 10, y, 'Charge Temp IAT Weight', FC + 'charge_temp_iat_pct', 'configedit', 110, '%')

    src = p.panel(320, A.TOP, 300, 430, 'Signal Sources')
    y = 10
    MODEL = '[#fuel_calculator.fuel_model]'
    for lbl, path, en in (('Manifold Pressure', 'map_src', ''),          # SD load axis AND the density term
                          ('Throttle Position', 'tps_src', f'{MODEL} == 1 || {MODEL} == 3'),
                          ('Coolant Temp', 'clt_src', ''),
                          ('Air Temp', 'iat_src', ''),
                          ('Mass Air Flow', 'maf_src', f'{MODEL} == 2'),
                          # Barometric: optional hardware, so the ASSUMPTION sits beside it — the number
                          # that stands in when nothing publishes the channel.
                          ('Barometric', 'baro_src', '')):
        y = p.field(src, 10, y, lbl, FC + path, 'enum', 240, enable=en)
    y = p.field(src, 10, y, 'Assumed Baro', FC + 'baro_assumed_kpa', 'configedit', 110, 'kPa')
    # FUEL PRESSURE IS NOT HERE ANY MORE. It is per stage — a stage can be a second set of injectors on
    # its own rail with its own regulator — so it lives on each stage's page. This panel kept a
    # `fuel_press_src` binding that stopped existing when the field became stage1_fuel_press_src.

    fuel = p.panel(630, A.TOP, 300, 230, 'Fuel Properties')
    y = 10
    # THE FUEL ITSELF IS PER STAGE — its stoich ratio, its ethanol (fixed, or measured by a flex sensor)
    # and its density live on each stage's Setup page, because a second stage is often a second fuel.
    PER_STAGE = 'Stoich, ethanol and density are per stage: Fuel Tuning \u25b8 Stage N \u25b8 Setup.'
    p.wrapped(10, y, 280, PER_STAGE, into=fuel, colour=C_DIM)
    y += A.Page.wrapped_h(PER_STAGE, 280) + 8
    y = p.field(fuel, 10, y, 'Overall Fuel Trim', FC + 'overall_corr_pct', 'configedit', 110, '%')
    # A fuel property like the ones above it, and the only thing that turns the level sender's
    # percentage of its own travel into a quantity anybody can act on. 0 leaves the litres
    # channel unpublished rather than reading zero on a full tank.
    p.field(fuel, 10, y, 'Tank Capacity', FC + 'tank_capacity_l', 'configedit', 110, 'L')

    ww = p.panel(630, A.TOP + 240, 300, 190, 'Wall Film')
    y = p.check(ww, 10, 16, 'Wall-film model enabled', FC + 'wallfilm_enabled')
    on = '[#fuel_calculator.wallfilm_enabled] == 1'
    y = p.field(ww, 10, y + 6, 'Deposit Fraction', FC + 'wallfilm_x_pct', 'configedit', 110, '%',
                enable=f'{on} and [#fuel_calculator.wallfilm_tau_ms] > 0')
    p.field(ww, 10, y, 'Evaporation Tau', FC + 'wallfilm_tau_ms', 'configedit', 110, 'ms', enable=on)

    eng = p.panel(940, A.TOP, 330, 430, 'Engine')
    y = 10
    y = p.field(eng, 10, y, 'Cylinders', ENG + 'cylinder_count', 'configedit', 110)
    y = p.field(eng, 10, y, 'Displacement', ENG + 'displacement', 'configedit', 110, 'cc')
    y = p.field(eng, 10, y, 'Engine Cycle', ENG + 'cycle_type', 'enum', 240)
    y = p.field(eng, 10, y, 'Injection Stages', ENG + 'num_inj_stages', 'configedit', 110)
    y = p.field(eng, 10, y, 'Injector Timing Method', ENG + 'injector_timing_method', 'enum', 240)
    p.field(eng, 10, y, 'Cranking Threshold', ENG + 'cranking_rpm', 'configedit', 110, 'RPM')

    _live_strip(p)
    return p


def _table_page(title, table, axes, live, note=None, blurb=None):
    """A correction page. One line now: the shared routine in fuel_tree IS this shape, and keeping a
    second copy here is how the two drifted apart in the first place — one grew a side panel, the other
    a top strip, and neither knew about the other.

    `axes` is vestigial and ignored: axis setup lives in the table's own context menu (Table ▸ Axis
    Setup), once, rather than as a control on every page that shows a table."""
    # Imported HERE, not at module scope: fuel_tree imports this module, so a top-level import
    # would be a cycle. One routine is still worth the two lines.
    import fuel_tree as FT
    return FT.table_page(title, table, live, ' '.join(x for x in (blurb, note) if x))


def page_ve():
    # THE SAME SHAPE AS EVERY OTHER TABLE PAGE. This had a bespoke strip of live numbers across the top
    # instead, on the argument that a 32x32 table wants the full width — but a page that is laid out
    # unlike every other page costs more than the column it saves. You look for the readouts where the
    # readouts are, and here they were somewhere else and overlapping each other.
    return _table_page('VE Table', FC + 've_table', None, [
        ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'), ('MAP', 'map', '%.0f'),
        ('VE', 've', '%.1f'), ('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f'),
        ('STFT', 'stft_pct', '%.1f'), ('LTFT', 'ltft_pct', '%.1f')])


def page_target_lambda():
    # The trims are the point here: they say whether the target is one the engine can actually hold.
    return _table_page('Target Lambda', FC + 'target_lambda_table', None, [
        ('Engine RPM', 'rpm', '%.0f'), ('Fuel Load', 'fuel_load', '%.0f'),
        ('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f'),
        ('STFT', 'stft_pct', '%.1f'), ('LTFT', 'ltft_pct', '%.1f'),
        ('LTFT B1', 'ltft_bank_1_pct', '%.1f'), ('LTFT B2', 'ltft_bank_2_pct', '%.1f')])


def page_start_warmup():
    """The four stages of a cold start, in the order they happen — and each one's TABLE on its own page.

    Three of the four grids here were drawn at under half the box they can grow to: prime at 41% of its
    height, cranking at 42% of its width, post-start at 12% — an 16x16 map in 46 pixels, which is five
    pixels a row. They are sized against what a tune can RESIZE THEM TO, not what they ship at, because
    the page is authored once and the axes are the tuner's to lengthen; a box built for today's eight
    rows is a scroll bar the day somebody adds a ninth.

    All three already have a full-size page of their own, so what was here was a second copy that could
    not be read or clicked. The settings, the switches and the live corrections stay — they are what you
    watch while the engine is starting — and the grid is one click away.

    Warmup keeps its table, and gets the whole width the top band gave back: 900x390 is its full
    allocation, which is the first time it has had it.
    """
    p = A.Page(title='Start & Warmup')
    p.head('Everything between key-on and a warm idle, in the order it happens: prime, crank, '
           'post-start decay, warmup enrichment.')

    # FOUR COLUMNS, not three plus one stacked. Each step is now its settings and its live reading, so
    # they are the same size and the same shape, and the row is the sequence read left to right.
    TOP_H, COL_W = 215, 305
    COL = [10, 325, 640, 955]

    prime = p.panel(COL[0], A.TOP, COL_W, TOP_H, '1 — Prime  (at first sync)')
    y = p.check(prime, 10, 16, 'Prime pulse enabled', FC + 'prime_enable')
    on = '[#fuel_calculator.prime_enable] == 1'
    y = p.field(prime, 10, y + 4, 'Prime Mode', FC + 'prime_mode', 'enum', 150, enable=on)
    p.switch(prime, 10, y + 6, 'Prime Pulse Table', '', link=f'{CFG}/Fuel Tuning/Fuel Prime Pulse',
             w=COL_W - 20)
    p.wrapped(10, y + 38, COL_W - 20,
              'Held back while flood clear is cutting — the prime is not spent, it '
              'fires on the next crank with the throttle closed.', into=prime)

    crank = p.panel(COL[1], A.TOP, COL_W, TOP_H, '2 — Cranking')
    y = p.check(crank, 10, 16, 'Cranking enrichment enabled', FC + 'enable_cranking')
    y = p.field(crank, 10, y + 4, 'Cranking Threshold', ENG + 'cranking_rpm', 'configedit', 110, 'RPM')
    p.switch(crank, 10, y + 6, 'Cranking Fuel Table', '', link=f'{CFG}/Fuel Tuning/Cranking',
             w=COL_W - 20)
    p.readout(crank, 10, y + 44, 'Cranking Corr', 'fuel_corr_cranking', '%.3f', w=140)
    p.readout(crank, 155, y + 44, 'Engine RPM', 'rpm', '%.0f', w=140)

    post = p.panel(COL[2], A.TOP, COL_W, TOP_H, '3 — Post-Start  (decays out)')
    y = p.check(post, 10, 16, 'Post-start enrichment enabled', FC + 'enable_poststart')
    p.switch(post, 10, y + 6, 'Post-Start Table', '', link=f'{CFG}/Fuel Tuning/Corrections/Post Start',
             w=COL_W - 20)
    p.readout(post, 10, y + 44, 'Post-Start Corr', 'fuel_corr_poststart', '%.3f', w=140)
    p.readout(post, 155, y + 44, 'Run Time', 'run_time', '%.0f', w=140)

    # FLOOD CLEAR belongs on this page and nowhere else: it is a cranking-time decision about fuel, and
    # the thing you reach for when the engine in front of you will not start. Its own box, because it is
    # a CUT rather than an enrichment — the opposite of everything else here.
    flood = p.panel(COL[3], A.TOP, COL_W, TOP_H, 'Flood Clear  (throttle open = no fuel)')
    fon = f'[#{FC}flood_clear_enabled] == 1'
    y = p.check(flood, 10, 16, 'Flood clear enabled', FC + 'flood_clear_enabled')
    y = p.field(flood, 10, y + 4, 'Cut Above Throttle', FC + 'flood_clear_tps_pct', 'configedit', 100,
                '%', enable=fon)
    p.add(p._new('indicator', 10, y + 8, 130, 30,
                 {'signalName': '[$flood_clear]', 'onTitle': 'CUTTING', 'offTitle': 'off',
                  'onBg': C_RED, 'onFg': '#000000', 'offBg': '#2a2a2e', 'offFg': '#6e7178',
                  'fontName': A.FONT_LBL}), into=flood)
    p.readout(flood, 150, y + 4, 'Throttle', 'tps', A.Page.chan_fmt('tps'), 140)

    # …AND THE ONE TABLE THAT FITS, at the full 900x390 it is allocated for. It kept its place because
    # it was the only one of the four already over half its box (57%), and the top band giving back
    # 160px is what makes the rest of it up.
    warm = p.panel(10, A.TOP + TOP_H + 10, 1260, A.CANVAS_H - A.TOP - TOP_H - 18,
                   '4 — Warmup Enrichment  (CLT)')
    warm_on = f'[#{FC}enable_warmup] == 1'
    p.check(warm, 930, 12, 'Enabled', FC + 'enable_warmup')
    wt_w, wt_h = A.table_box(FC + 'clt_corr_table', fallback=(900, 224))
    p.table(10, 12, min(wt_w, 900), min(wt_h, A.CANVAS_H - A.TOP - TOP_H - 40), FC + 'clt_corr_table',
            into=warm, enable=warm_on)
    p.readout(warm, 930, 46, 'Coolant', 'clt', '%.0f', w=150)
    p.readout(warm, 1090, 46, 'Warmup Corr', 'fuel_corr_warmup', '%.3f', w=150)
    p.readout(warm, 930, 116, 'Lambda', 'lambda_1', '%.2f', w=150)
    p.readout(warm, 1090, 116, 'Target', 'lambda_target', '%.2f', w=150)
    p.readout(warm, 930, 186, 'Engine RPM', 'rpm', '%.0f', w=150)
    p.readout(warm, 1090, 186, 'Idle Target', 'idle_target_rpm', '%.0f', w=150)
    # THE TABLE'S OWN OUTPUT. "Warmup Corr" above is what FuelCalculator APPLIED; this is what the grid
    # you are editing says at the cell the trace is on. They differ whenever the module clamps or the
    # enable is off, which is exactly when you want to see both.
    p.readout(warm, 930, 256, 'This Table', FC + 'clt_corr_table', A.table_fmt(FC + 'clt_corr_table'),
              w=150)
    # The coolant trace was the last thing on this panel and no longer fits it — the enable row and the
    # flood-clear box took the height. The reading above says where the engine is; the trace over time
    # belongs on the sensor's own page, which is one click away and shows it full size.
    return p


def page_injector_stage(n):
    """One stage's injector data on ONE page. Dead time, flow, small-pulse and timing are four views of
    the same injector, and tuning one against the others across four pages is how you end up with a
    stage that only behaves at one pressure."""
    p = A.Page(title=f'Stage {n}')
    p.head('Dead time and flow are the injector\'s own data — from its datasheet or a flow '
           'bench. Timing and staging are yours.')
    el = f'{ENG}inj_stage[{n-1}].'

    cfg = p.panel(10, A.TOP, 250, 244, 'Stage Setup')
    y = p.field(cfg, 10, 10, 'Outputs', el + 'num_outputs', 'configedit', 90)
    y = p.field(cfg, 10, y, 'Mode', el + 'mode', 'enum', 190)
    # "Multi-Point and Bank ONLY read it; the sequential modes derive their own" — the field's own help.
    # So it greys outside those two rather than sitting there looking like it does something.
    mode = f'[#engine.inj_stage[{n-1}].mode]'
    p.field(cfg, 10, y, 'Injections / Cycle', el + 'injections_per_cycle', 'configedit', 90,
            enable=f'{mode} == 2 || {mode} == 3')

    # THE RAIL THIS STAGE IS ON. Its own regulator, its own pressure, its own sensor or none — because
    # a staged set of injectors is very often a second rail entirely. The differential this produces is
    # the X axis of the two tables to the right of it, which is the whole of what pressure does here.
    rail = p.panel(10, A.TOP + 250, 250, 190, 'Fuel Rail')
    rmode = f'[#fuel_calculator.stage{n}_fuel_press_mode]'
    y = p.field(rail, 10, 10, 'Pressure', f'{FC}stage{n}_fuel_press_mode', 'enum', 190)
    # A sensor install ignores the base; the other two ignore the source. Greyed rather than hidden so
    # the page still says what the other answers would need.
    y = p.field(rail, 10, y, 'Base Pressure', f'{FC}stage{n}_fuel_press_base_kpa', 'configedit', 110, 'kPa',
                enable=f'{rmode} != 0')
    y = p.field(rail, 10, y, 'Ratio', f'{FC}stage{n}_fuel_press_ratio', 'configedit', 110, ':1',
                enable=f'{rmode} == 2')
    p.field(rail, 10, y, 'Sensor', f'{FC}stage{n}_fuel_press_src', 'enum', 190, enable=f'{rmode} == 0')

    dead = p.panel(270, A.TOP, 500, 244, 'Dead Time  (battery x fuel pressure)')
    p.table(10, 12, 480, 200, f'{FC}stage{n}_dead_time_table', into=dead)

    flow = p.panel(780, A.TOP, 490, 244, 'Flow Rate')
    p.table(10, 12, 470, 200, f'{FC}stage{n}_inj_flow_table', into=flow)

    # A.table_box, not a guess: the grid is as wide as its own numbers need (8 columns at one decimal
    # is 499, not the 480 this was drawn at), and a box 19px short is a scroll bar over a map that fits.
    #
    # IT GETS THE WHOLE ROW NOW. It used to share it with Small-Pulse Correction and was clamped to
    # whatever was left after that panel had room for its own title — see below for where small-pulse
    # went and why it was never going to fit here.
    ang_w, _ang_h = A.table_box(f'{FC}stage{n}_inj_angle_table')
    ang_w = min(ang_w, 1260 - 270 - 20)
    ang = p.panel(270, A.TOP + 250, ang_w + 20, 246, 'Injection Timing  (BTDC)')
    p.table(10, 12, ang_w, 200, f'{FC}stage{n}_inj_angle_table', into=ang)

    # SMALL-PULSE CORRECTION IS NOT ON THIS PAGE, and the reason is arithmetic rather than taste.
    #
    # It is a one-row curve of injector pulse width against its own adder — fourteen bins as it ships,
    # thirty-two allocated — and at three decimals a bin is wide. table_box cannot even size it (it is
    # a curve, so the meta gives it no `rows`, and the call falls back to 1260x400): the honest answer
    # is that it wants the full width of a page. It had 164px here, which is 10.9px per column with the
    # shipped fourteen — a grid of vertical lines, not numbers you can read or click.
    #
    # It already HAS the page it needs: "Stage {n} / Short Pulse Width Adder", 1260 wide, one click down
    # the tree from this node. Drawing an unreadable second copy here bought nothing and cost this row
    # the width the timing table wanted.

    # THE TWO THAT CANNOT FIT A SHARED ROW, as links to the pages where they are full size.
    #
    # Sized against what they can GROW to, not what they ship at, because a page is authored once and a
    # tune resizes its axes whenever it likes. Staging Duty is allocated 16x16 and needs 938x390; this
    # row gave it 780x92, which is 24% of the height — eleven of its sixteen rows behind a scroll bar on
    # a page that looks like it is showing you the table. Small-Pulse is worse and is described above.
    #
    # Both already have a full-size page directly beneath this node in the tree, so what was here was a
    # second, unreadable copy. The link is the honest version of it.
    more = p.panel(10, A.TOP + 508, 340, A.CANVAS_H - A.TOP - 512, 'Full-Size Tables')
    p.switch(more, 10, 10, 'Staging Duty', '', link=f'{CFG}/Fuel Tuning/Stage {n}/Staging Duty', w=310)
    p.switch(more, 10, 35, 'Small-Pulse Correction', '',
             link=f'{CFG}/Fuel Tuning/Stage {n}/Short Pulse Width Adder', w=310)
    p.switch(more, 10, 60, 'Fuel  (stoich, flex, density)', '', link=f'{CFG}/Fuel Tuning/Stage {n}/Fuel', w=310)

    live = p.panel(360, A.TOP + 508, 910, A.CANVAS_H - A.TOP - 512, 'Live')
    # TOP ROW: WHAT THE THREE TABLES ON THIS PAGE ARE PUTTING OUT. That is the number you watch while
    # tuning the cells above it, and it was the one thing this strip did not carry — the module readings
    # are context. Bound to each table's own path, which Cache::solveTable interpolates against its live
    # axes in float, bit-for-bit with the firmware (see fuel_tree.table_page for the same change on the
    # table pages).
    OUTS = (('Dead Time', f'{FC}stage{n}_dead_time_table'),
            ('Flow Rate', f'{FC}stage{n}_inj_flow_table'),
            ('Inj Angle', f'{FC}stage{n}_inj_angle_table'))
    READS = (('Inj PW', 'inj_pw', '%.2f'), ('Dead Time', 'pw_add_deadtime', '%.0f'),
             ('Press Diff', 'inj_press_diff', '%.0f'), ('Base PW', 'base_pw', '%.0f'),
             ('Fuel Press', 'fuel_pressure', '%.0f'), ('Battery', 'battery', '%.1f'))
    for i, (lbl, tbl) in enumerate(OUTS):
        p.readout(live, 12 + i * 148, 4, lbl, tbl, A.table_fmt(tbl), w=140)
    # Three beside them, three under — the panel is 910 wide and nine readouts do not fit one row.
    for i, (lbl, ch, fmt) in enumerate(READS):
        x, y = (460 + i * 148, 4) if i < 3 else (12 + (i - 3) * 148, 58)
        p.readout(live, x, y, lbl, ch, fmt, w=140)
    return p


def page_stage_fuel(n):
    """What stage n burns, on one page: its fuel's settings, its density table and its composition trim.

    A second stage is often a second fuel as well as a second set of injectors — port petrol with a
    secondary on E85 or methanol — so the fuel is the stage's, not the engine's. Its ethanol is fixed, or
    measured by a flex sensor several stages may share; its density converts the mass it delivers into
    the volume its injectors' flow is quoted in; its trim corrects what the stoich blend alone does not.
    The firmware mixes the stages' fuels by the mass each delivers (FuelCalculator)."""
    p = A.Page(title=f'Stage {n} Fuel')
    p.head(f'What stage {n} burns. Stages sharing one fuel system point at the same flex sensor.')
    flex = f'[#fuel_calculator.stage{n}_flex_enabled] == 1'
    sfx = '' if n == 1 else f'_{n}'

    fuel = p.panel(10, A.TOP, 330, 250, 'Fuel')
    y = p.field(fuel, 10, 10, 'Flex Fuel', f'{FC}stage{n}_flex_enabled', 'checkbox')
    y = p.field(fuel, 10, y, 'Ethanol Source', f'{FC}stage{n}_ethanol_src', 'enum', 190, enable=flex)
    y = p.field(fuel, 10, y, 'Ethanol %', f'{FC}stage{n}_ethanol_pct', 'configedit', 110, '%')
    y = p.field(fuel, 10, y, 'Stoich AFR', f'{FC}stage{n}_stoich_x10', 'configedit', 110)
    y = p.field(fuel, 10, y, 'Stoich AFR (Ethanol)', f'{FC}stage{n}_stoich_ethanol_x10', 'configedit', 110)
    # WHERE THIS STAGE'S FUEL TEMPERATURE COMES FROM is the density table's temperature axis — a flex
    # sensor reports its own line's, and a stage sharing that line can point at the same one. Only read
    # when the table has a temperature axis, which it does not as it ships (a single density).
    p.field(fuel, 10, y, 'Fuel Temp Source', f'{FC}stage{n}_specific_gravity_table_x_src', 'enum', 190,
            enable=f'[#fuel_calculator.stage{n}_specific_gravity_table_x_en] == 1')

    NOTE = ('Flex Fuel off: Ethanol % is this stage\'s fixed blend (0 petrol, 85 E85; methanol is its Stoich '
            'AFR at 0 %). On: the sensor while it reads, the last good reading if it drops out, and Ethanol % '
            'until it has read once since power-up.')
    note = p.panel(10, A.TOP + 256, 330, A.Page.wrapped_h(NOTE, 306) + A.PANEL_TITLE + 16, 'How It Is Used')
    p.wrapped(10, 8, 306, NOTE, into=note)

    sg_w, sg_h = A.table_box(f'{FC}stage{n}_specific_gravity_table')
    sg = p.panel(350, A.TOP, 920, 250, 'Specific Gravity  (density: fuel temp x ethanol)')
    p.table(10, 12, min(sg_w, 900), 210, f'{FC}stage{n}_specific_gravity_table', into=sg)

    fc = p.panel(350, A.TOP + 256, 920, 300, 'Fuel Comp Correction  (ethanol x load, while Flex Fuel is on)')
    p.table(10, 12, 900, 260, f'{FC}stage{n}_fuel_comp_corr_table', into=fc)

    live = p.panel(10, A.TOP + 562, 1260, A.CANVAS_H - A.TOP - 566, 'Live')
    for i, (lbl, ch, fmt) in enumerate((('Stage Ethanol', f'stage{n}_ethanol', '%.0f'),
                                        ('Charge Ethanol', 'flex_ethanol', '%.0f'),
                                        ('Fuel SG', f'fuel_sg{sfx}', '%.3f'),
                                        ('Fuel Comp Corr', f'fuel_corr_fuelcomp{sfx}', '%.3f'),
                                        ('Fuel Temp', 'fuel_temp', '%.0f'))):
        p.readout(live, 12 + i * 200, 4, lbl, ch, fmt, w=190)
    return p


# ---- corrections: same page shape, one per multiplier -------------------------------------------
CORRECTIONS = [
    ('Air Temp', FC + 'iat_corr_table',
     [('X Channel', FC + 'iat_corr_table_x_src'), ('Y Channel', FC + 'iat_corr_table_y_src'),
      ('X bins', FC + 'iat_axis_n'), ('Y bins', FC + 'iat_map_axis_n')],
     [('Air Temp Corr', 'fuel_corr_iat', '%.3f'), ('Air Temp', 'iat', '%.0f'), ('MAP', 'map', '%.0f')],
     'Denser air needs more fuel. This is the correction for it.'),
    ('MAP', FC + 'map_corr_table',
     [('X Channel', FC + 'map_corr_table_x_src'), ('Y Channel', FC + 'map_corr_table_y_src'),
      ('X bins', FC + 'map_corr_map_axis_n'), ('Y bins', FC + 'map_corr_rpm_axis_n')],
     [('MAP Corr', 'fuel_corr_map', '%.3f'), ('MAP', 'map', '%.0f'), ('Engine RPM', 'rpm', '%.0f')],
     'A load-and-speed trim on top of the VE table.'),
    ('Barometric', FC + 'baro_corr_table',
     [('X Channel', FC + 'baro_corr_table_x_src'), ('Y Channel', FC + 'baro_corr_table_y_src'),
      ('X bins', FC + 'baro_corr_axis_n'), ('Y bins', FC + 'baro_corr_rpm_axis_n')],
     [('Baro Corr', 'fuel_corr_baro', '%.3f'), ('Barometric', 'baro_kpa', '%.0f'), ('MAP', 'map', '%.0f')],
     'Altitude and weather. Thin air, less fuel.'),
    ('Gear', FC + 'fuel_gear_table',
     [('X Channel', FC + 'fuel_gear_table_x_src'), ('Y Channel', FC + 'fuel_gear_table_y_src'),
      ('Z Channel', FC + 'fuel_gear_table_z_src'),
      ('X bins', FC + 'gear_load_axis_n'), ('Y bins', FC + 'gear_rpm_axis_n')],
     [('Gear Corr', 'fuel_corr_gear', '%.3f'), ('Engine RPM', 'rpm', '%.0f'),
      ('Fuel Load', 'fuel_load', '%.0f')],
     'Per-gear fuelling, for the gears that see more load than the others.'),
    ('Rev Limiter', FC + 'rev_limit_fuel_corr_table',
     [('X Channel', FC + 'rev_limit_fuel_corr_table_x_src'),
      ('Y Channel', FC + 'rev_limit_fuel_corr_table_y_src'),
      ('X bins', FC + 'rl_headroom_axis_n'), ('Y bins', FC + 'rl_map_axis_n')],
     [('Rev Limit Corr', 'fuel_corr_revlimit', '%.3f'), ('RPM Before Cut', 'rpm_to_limit', '%.0f'),
      ('Engine RPM', 'rpm', '%.0f')],
     'Enrichment as the limiter approaches — softens the cut and cools the chambers.'),
]
for _i in (1, 2, 3, 4):
    CORRECTIONS.append((
        f'Generic {_i}', f'{FC}generic{_i}_corr_table',
        [('X Channel', f'{FC}generic{_i}_corr_table_x_src'), ('Y Channel', f'{FC}generic{_i}_corr_table_y_src'),
         ('X bins', f'{FC}gen{_i}_x_axis_n'), ('Y bins', f'{FC}gen{_i}_y_axis_n')],
        [(f'Generic {_i} Corr', f'fuel_corr_generic{_i}', '%.3f'), ('Engine RPM', 'rpm', '%.0f'),
         ('Fuel Load', 'fuel_load', '%.0f')],
        'A spare correction: pick its axes and it does whatever you need it to.'))


def page_correction(entry):
    title, table, axes, live, blurb = entry
    return _table_page(title, table, axes, live, blurb=blurb)


def page_cyl_trim(n):
    return _table_page(
        f'Cylinder {n}', f'{FC}cyl{n}_fuel_corr_table',
        [('X Channel', f'{FC}cyl{n}_fuel_corr_table_x_src'), ('Y Channel', f'{FC}cyl{n}_fuel_corr_table_y_src'),
         ('X bins', FC + 'cyl_load_axis_n'), ('Y bins', FC + 'cyl_rpm_axis_n')],
        [(f'Lambda {n}', f'lambda_{n}', '%.2f'), ('Target', 'lambda_target', '%.2f'),
         ('LTFT', 'ltft_pct', '%.1f')],
        blurb='Trim for one cylinder, set by hand. Closed loop does not learn per cylinder — it '
              'corrects the whole engine — so this is where per-cylinder evidence goes: watch this '
              'cylinder\'s own wideband against the target and trim until they agree. The LTFT '
              'readout is the engine-wide learned correction already applied underneath.')


def page_transient():
    p = A.Page(title='Transient Fuel')
    p.head('Extra fuel while the load is CHANGING — the throttle-tip enrichment. Detection '
           'first, then how much, then how fast it decays.')

    ON = '[#transient_throttle.enabled] == 1'      # the feature switch gates everything it owns
    cfg = p.panel(10, A.TOP, 320, 450, 'Detection')
    y = p.check(cfg, 10, 16, 'Transient enrichment enabled', TT + 'enabled')
    y = p.field(cfg, 10, y + 4, 'Load Source', TT + 'load_source', 'enum', 190, enable=ON)
    y = p.field(cfg, 10, y, 'Load Rate Dead Band', TT + 'enr_load_rate_db', 'configedit', 100, '/s', enable=ON)
    y = p.field(cfg, 10, y, 'Load Accel Dead Band', TT + 'enr_load_accel_db', 'configedit', 100, '/s²', enable=ON)
    y = p.field(cfg, 10, y, 'Detect Duration', TT + 'enr_detect_ms', 'configedit', 100, 'ms', enable=ON)
    y = p.field(cfg, 10, y, 'Overall Correction', TT + 'overall_corr_pct', 'configedit', 100, '%', enable=ON)
    y = p.check(cfg, 10, y + 6, 'Async pulses enabled', TT + 'enable_async', enable=ON)
    ASYNC = ON + ' && [#transient_throttle.enable_async] == 1'
    y = p.field(cfg, 10, y + 6, 'Max Async Pulses', TT + 'max_async_pulses', 'configedit', 100, '', enable=ASYNC)
    p.field(cfg, 10, y, 'Async Holdoff', TT + 'async_holdoff_ms', 'configedit', 100, 'ms', enable=ASYNC)

    rate = p.panel(340, A.TOP, 460, 450, 'Enrichment Rate  (load rate x start load)', enable=ON)
    p.table(10, 12, 440, 400, TT + 'tt_enrich_rate_table', into=rate)

    amt = p.panel(810, A.TOP, 460, 450, 'Sync Amount  (rpm x MAP)', enable=ON)
    p.table(10, 12, 440, 400, TT + 'tt_enrich_sync_table', into=amt)

    live = p.panel(10, A.TOP + 460, 1260, A.CANVAS_H - A.TOP - 468, 'Live')
    # THE TWO TABLES ON THIS PAGE FIRST, then the module. Accel Corr is every transient contribution
    # multiplied together; it is not what either of these grids is putting out.
    for i, (lbl, tbl) in enumerate((('Enrich Rate', TT + 'tt_enrich_rate_table'),
                                    ('Sync Amount', TT + 'tt_enrich_sync_table'))):
        p.readout(live, 12 + i * 122, 14, lbl, tbl, A.table_fmt(tbl), w=114)
    for i, (lbl, ch, fmt) in enumerate((('Transient Active', 'transient_active', '%.0f'),
                                        ('Accel Corr', 'fuel_corr_accel', '%.3f'),
                                        ('Load Rate', 'tt_load_rate', '%.1f'),
                                        ('Start Load', 'tt_start_load', '%.1f'),
                                        ('Throttle', 'tps', '%.1f'),
                                        ('MAP', 'map', '%.0f'),
                                        ('Instant MAP', 'imap', '%.0f'),
                                        ('Lambda', 'lambda_1', '%.2f'),
                                        ('Inj PW', 'inj_pw', '%.2f'))):
        p.readout(live, 256 + i * 112, 14, lbl, ch, fmt, w=104)
    p.graph(12, 84, 1236, 62, '[$tps]', into=live)   # fits the strip it sits in, which is 174 tall
    return p


def page_transient_decay():
    p = A.Page(title='Decay & Disenrich')
    p.head('How the added fuel goes away, and what happens on a closing throttle.')
    ON = '[#transient_throttle.enabled] == 1'
    dec = p.panel(10, A.TOP, 630, 300, 'Enrichment Decay',
                  enable=ON + ' && [#transient_throttle.enable_decay] == 1')
    p.table(10, 12, 610, 250, TT + 'tt_enrich_decay_table', into=dec)
    asy = p.panel(650, A.TOP, 620, 300, 'Async Amount',
                  enable=ON + ' && [#transient_throttle.enable_async] == 1')
    p.table(10, 12, 600, 250, TT + 'tt_enrich_async_table', into=asy)

    DIS = ON + ' && [#transient_throttle.enable_disenrich] == 1'
    cfg = p.panel(10, A.TOP + 310, 320, A.CANVAS_H - A.TOP - 318, 'Disenrichment')
    y = p.check(cfg, 10, 16, 'Disenrich enabled', TT + 'enable_disenrich', enable=ON)
    y = p.field(cfg, 10, y + 4, 'Load Rate Dead Band', TT + 'dis_load_rate_db', 'configedit', 100, '/s', enable=DIS)
    y = p.field(cfg, 10, y, 'Load Decel Dead Band', TT + 'dis_load_decel_db', 'configedit', 100, '/s²', enable=DIS)
    p.field(cfg, 10, y, 'Detect Duration', TT + 'dis_detect_ms', 'configedit', 100, 'ms', enable=DIS)

    r = p.panel(340, A.TOP + 310, 300, A.CANVAS_H - A.TOP - 318, 'Disenrich Rate', enable=DIS)
    p.table(10, 12, 280, 286, TT + 'tt_disenrich_rate_table', into=r)
    a = p.panel(650, A.TOP + 310, 300, A.CANVAS_H - A.TOP - 318, 'Disenrich Amount', enable=DIS)
    p.table(10, 12, 280, 286, TT + 'tt_disenrich_amount_table', into=a)
    # DECAY IS DECAY, whichever direction. The firmware reads BOTH decay tables inside
    # `if (cfg_->enable_decay && ...)` (TransientThrottle.cpp:183) — the enrichment one is already
    # behind that flag above, and this one was only behind enable_disenrich, so with decay switched off
    # it stayed live and took values nothing read.
    d = p.panel(960, A.TOP + 310, 310, A.CANVAS_H - A.TOP - 318, 'Disenrich Decay',
                enable=DIS + ' && [#transient_throttle.enable_decay] == 1')
    p.table(10, 12, 290, 286, TT + 'tt_disenrich_decay_table', into=d)
    return p


def page_map_prediction():
    """MAP prediction + the wall-film model — the pair that replaces classic transient throttle.

    Two functions on one page because they are two halves of one answer: prediction fixes the
    MEASUREMENT (a MAP signal averaged over a cylinder period is late, and a throttle stab fills the
    plenum long before it says so), the film model fixes the FUEL (a port-injected engine wets its
    walls, and the film has to be built up or given back on every change). Rate-table transient
    enrichment corrects both at once by feel, which is why it wants retuning whenever anything else
    moves.
    """
    p = A.Page(title='MAP Prediction')
    y0 = p.head('While the throttle is MOVING, take manifold pressure from the predicted table instead '
                'of the sensor, and model the fuel film explicitly. Together these replace transient '
                'throttle — use one approach or the other, not both.', enable=FC + 'map_predict_enabled',
                enable_label='Prediction')
    on = '[#fuel_calculator.map_predict_enabled] == 1'

    cfg = p.panel(10, y0, 420, A.panel_h(2, A.ROW, top=12, bottom=6), 'Prediction')
    y = 12
    y = p.field(cfg, 10, y, 'Predicted MAP Time', FC + 'map_predict_hold_ms', 'configedit', 110, 'ms',
                enable=on, lbl_w=170)
    p.field(cfg, 10, y, 'Throttle Source', FC + 'tps_src', 'enum', 200, enable=on, lbl_w=170)
    p.note(10, y0 + A.panel_h(2, A.ROW, top=12, bottom=6) + 6,
           'The hold is how long prediction lasts after the movement that triggered it. Too short and '
           'it lets go before the sensor has caught up, which is the lean hole it exists to fill; a '
           'long or shared MAP hose wants more. Around 200 ms suits a typical install.', w=410)

    ww = p.panel(10, y0 + 210, 420, A.panel_h(3, A.ROW, top=18, bottom=6), 'Fuel Film')
    y = p.check(ww, 10, 16, 'Wall-film model enabled', FC + 'wallfilm_enabled')
    won = '[#fuel_calculator.wallfilm_enabled] == 1'
    # The SAME field appears on Fuel Setup, and carries the same gate there: an evaporation time of zero
    # switches the wall-film model off, so the fraction it deposits is a number nobody reads.
    y = p.field(ww, 10, y + 6, 'Film Pooling', FC + 'wallfilm_x_pct', 'configedit', 110, '%',
                enable=f'{won} and [#fuel_calculator.wallfilm_tau_ms] > 0', lbl_w=170)
    p.field(ww, 10, y, 'Evaporation Time', FC + 'wallfilm_tau_ms', 'configedit', 110, 'ms',
            enable=won, lbl_w=170)
    p.note(10, y0 + 210 + A.panel_h(3, A.ROW, top=18, bottom=6) + 6,
           'Pooling is how much of each injection lands on the port wall: 5-10% for a well matched port '
           'and injector on a warm engine, 25-30% where the inlet turns sharply or the spray misses the '
           'valve, around 50% cold. Evaporation is how long the film takes to give it back — about '
           '200 ms warm, up to 400 ms cold. Optimise injection TIMING first: a lean spike caused by '
           'closed-valve injection is not a pooling problem, and pooling used to hide it just makes the '
           'engine rich everywhere else.', w=410)

    live = p.panel(450, y0, 400, 260, 'Right Now')
    for i, (lbl, ch, fmt) in enumerate((('Manifold Pressure', 'map_est', '%.1f'),
                                        ('Measured', 'map', '%.1f'),
                                        ('Predicted', 'map_predicted', '%.1f'),
                                        ('Throttle Rate', 'tps_rate', '%.0f'),
                                        ('Film Correction', 'fuel_corr_film', '%.3f'),
                                        ('Injector PW', 'inj_pw', '%.2f'))):
        # PITCH 70, NOT 74. A panel gives its children h - titleBarH() - 4, so this 260-tall box has 234
        # of content; three rows at 74 end at 212 and the state row below them ran to 262 — 28px past
        # the box, clipped on screen and invisible to a checker that only measures panels against the
        # canvas. Six pixels a row is what buys the last row its place.
        p.readout(live, 16 + (i % 2) * 195, 14 + (i // 2) * 70, lbl, f'[${ch}]', fmt, w=180)
    # The state, in words: measured / predicted / predicted because the sensor failed. A number would
    # need a legend, and the legend is the whole content of the row.
    p.add(p._new('value', 16, 206, 370, 26,
                 {'signalName': '[$map_source]', 'format': '%s', 'fontName': A.FONT_LBL,
                  'align': 'Left', 'showUnit': '0'}), into=live)

    p.graph(450, y0 + 270, 820, 200, 'map_est')

    nxt = p.panel(870, y0, 400, A.panel_h(3, 28, top=12, bottom=6), 'Instead Of')
    y = 12
    y = p.switch(nxt, 10, y, 'Transient Throttle  (the classic one)', 'transient_throttle.enabled',
                 link=f'Configuration/Fuel Tuning/Transient Throttle', w=340, pitch=28)
    p.switch(nxt, 10, y, 'Predicted MAP Table', '', link='Configuration/Fuel Tuning/Predicted MAP',
             w=340, pitch=28)
    p.switch(nxt, 10, y + 28, 'Transient TPS Scaling', '',
             link='Configuration/Fuel Tuning/MAP Prediction/Transient TPS Scaling', w=340, pitch=28)
    return p
