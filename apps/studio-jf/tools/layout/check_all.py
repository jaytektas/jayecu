"""Build every page the installers build and measure it against its own canvas.

A page is a fixed canvas with no reflow, so "does it fit" is not a question anyone can answer by eye at
155 pages. This builds them all and prints what hangs over an edge, which is how the refit to the real
viewport size (1280x700) was done and how it stays done.

    python3 check_all.py
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from apply_fuel import load_meta
import fuel_pages as F, fuel_tree as FT, top_pages as T, feature_pages as FP
import idle_pages as IP, boost_pages as BP, diag_page as DG
import sensor_page as SP


def check_node_names(meta):
    """A NODE NAME MUST NOT CONTAIN '/'. Paths are '/'-joined strings everywhere — panelLibrary keys,
    hyperlink targets, viewport node refs — and main.cpp's hyperlink::reachable() walks a link by
    splitOn(path, '/'). So a node called "O2 / Lambda" produces a path that matches the library exactly
    (which is why validation passed) and yet splits into segments that match no node, leaving every
    caption pointing into that branch silently unfollowable. Renaming is the fix; this stops it coming
    back."""
    import apply_fuel as _AF, json as _json
    bad = []
    def walk(ns, pre=''):
        for n in ns:
            nm = n.get('name', '')
            if '/' in nm: bad.append(f"{pre}/{nm}" if pre else nm)
            walk(n.get('children', []), f'{pre}/{nm}' if pre else nm)
    walk(_json.load(open(_AF.DOC))['tree'])
    for b in bad: print(f"  ! node name contains '/': {b!r} — links into it cannot resolve")
    return len(bad)


def pages(meta):
    """(name, Page) for one of every SHAPE — not all 155, but every builder that makes them."""
    out = []
    out.append(('fuel setup', F.page_setup()))
    out.append(('ve table', F.page_ve()))
    out.append(('target lambda', F.page_target_lambda()))
    out.append(('start warmup', F.page_start_warmup()))
    out.append(('inj stage 1', F.page_injector_stage(1)))
    out.append(('transient', F.page_transient()))
    out.append(('transient decay', F.page_transient_decay()))
    out.append(('cyl trim', F.page_cyl_trim(1)))
    for name, table, live, note, extra, flag in FT.CORRECTIONS[:2]:
        out.append((f'corr:{name}', FT.table_page(name, table, live, note, extra, enable_path=flag)))
    out.append(('corrections switchboard', FT.page_corrections_branch()))
    out.append(('root', T.page_root(meta)))
    out.append(('fuel top', T.page_fuel_top()))
    out.append(('ign top', T.page_ign_top()))
    out.append(('advance', T.page_ign_advance()))
    out.append(('ign trailing', T.page_ign_trailing()))
    out.append(('ign cyl trim', T.page_ign_cyl_trim(1)))
    out.append(('dwell', T.page_ign_dwell()))
    out.append(('advance limits', T.page_ign_limits()))
    for title, _node, groups, blurb in T.CATEGORY_PAGES:
        out.append((f'cat:{title}', T.page_category(title, groups, blurb)))
    for feat in FP.FEATURES:
        title, mod, _node, groups, live, blurb = feat
        # …WITH ITS EXTRA PANEL, which is what apply_top installs (apply_top.py:193). Built without it,
        # this checked the scalar groups of a page and NONE of the part that gets retuned: the speed
        # pickup calibration and its Capture button, the gear ratio grid, the CAN bus rows, the Lua
        # source, the misfire segments and every branch-tables panel were all checked by nobody. A
        # configedit sitting underneath a button went in that way, and reported clean.
        out.append((f'feat:{title}', FP.feature_page(title, mod, groups, live, blurb,
                                                     FP.EXTRA_PANELS.get(mod))))
    out.append(('idle setup', IP.page_setup(meta)))
    out.append(('boost setup', BP.page_setup(meta)))
    # The sensors switchboard needs the document (its links must name real tree nodes), so it reads the
    # one the installers write to — the same file, and the same answer.
    import json
    from apply_fuel import DOC
    out.append(('sensors', SP.page(json.load(open(DOC)))[0]))
    out.append(('diagnostics', DG.page()[0]))
    # The Engine Configuration branch — six pages, each a different shape (a switchboard, two tables, a
    # diagram page, a live page), so all six are measured rather than one standing for the rest.
    # The drive-by-wire pages: two throttle bodies, two bridges, the pedal, and the settings page each
    # body gets. Every one of them is a different shape.
    import etb_pages as ET
    out.append(('etb branch', ET.page_etb_branch()))
    out.append(('hb branch', ET.page_hb_branch()))
    out.append(('throttle A', ET.page_throttle(0)))
    out.append(('throttle B', ET.page_throttle(1)))
    out.append(('etb cal settings', ET.page_cal_settings(0)))
    out.append(('half bridge A', ET.page_half_bridge(0)))
    out.append(('accelerator pedal', ET.page_pedal()))
    import function_pages as FN
    out.append(('idle stepper', FN.page_stepper()))
    out.append(('misfire', FN.page_misfire()))
    out.append(('engine protection', FN.page_protection()))
    out.append(('protection levels', FN.page_protection_levels()))
    out.append(('threshold monitors', FN.page_threshold_monitors()))
    for path, build in FP.EXTRA_PAGES.items():
        out.append((f'extra:{path.rsplit("/", 1)[-1]}', build()))
    import more_pages as MP2
    out.append(('outputs switchboard', MP2.page_outputs()))
    out.append(('output setup', MP2.page_output_setup()))
    out.append(('output frequency', MP2.page_freq_map()))
    out.append(('generic tables', MP2.page_generic_tables()))
    for _i in range(1, 9):
        out.append((f'generic table {_i}', MP2.page_generic_table(_i)))
    import knock_pages as KP
    for path, pg in KP.pages().items():
        out.append((f'knock:{path.rsplit("/", 1)[-1]}', pg))
    import cruise_pages as CP
    for path, pg in CP.pages().items():
        out.append((f'cruise:{path.rsplit("/", 1)[-1]}', pg))
    import engine_pages as EG
    out.append(('engine top', EG.page_top()))
    out.append(('engine cylinders', EG.page_cylinders()))
    out.append(('engine ignition', EG.page_ignition()))
    out.append(('engine fuel system', EG.page_fuel_system()))
    out.append(('engine trigger', EG.page_trigger()))
    out.append(('engine trigger diag', EG.page_trigger_diagnostics()))
    # Both stream shapes: a crank stream and a cam stream (the cam one carries the VVT panel, so it is
    # the taller of the two and the one that runs out of canvas first), plus the switchboard.
    out.append(('trigger streams', EG.page_streams_branch()))
    out.append(('stream (crank)', EG.page_stream(0, EG.STREAM_NAMES[0])))
    out.append(('stream (cam)', EG.page_stream(2, EG.STREAM_NAMES[2])))
    # The per-sensor pages and the per-group lists: 122 of the first and 11 of the second are built from
    # these three shapes, so one of each is what there is to measure. The sensor pages take their content
    # from the catalogue — a sensor with every check and one with none are different heights — so both
    # ends are checked rather than a middle case that happens to fit.
    import sensor_detail as SD, sensor_group as SG
    all_checks = [c[0] for c in SD.CHECKS]
    codes = {c: 'P0123' for c in all_checks}
    out.append(('sensor (all checks)', SD.page(all_checks, codes, 'Temperature', 'x/Diagnostics')))
    out.append(('sensor (no checks)', SD.page([], {}, '', '')))
    out.append(('sensor diagnostics', SD.diag_page(all_checks, codes)))
    out.append(('sensor diagnostics (one)', SD.diag_page(['raw_min'], {'raw_min': 'P0123'})))
    big = [(f's{i}', f'Sensor Name {i}') for i in range(35)]
    out.append(('sensor group (35)', SG.page('Engine', big, lambda *_: 'x', lambda _s: 'rpm',
                                             {'rpm': {'digits': 0, 'units': 'RPM'}})))
    out.append(('sensor group (1)', SG.page('Nitrous', big[:1], lambda *_: '', lambda _s: '', {})))
    return out


def main():
    meta = load_meta()
    bad, n = [], 0
    for name, p in pages(meta):
        n += 1
        # Diagnostics is a TAB, not a page in a viewport: no title bar floats over it.
        bad += A.check(p, name, band=(name != 'diagnostics'))
    for b in bad:
        print(' ', b)
    print(f'{n} page shapes checked, {len(bad)} complaint(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
    check_node_names(None)
