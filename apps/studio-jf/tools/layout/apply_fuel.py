"""Install the Fuel branch — tree, pages, and the viewports that make them reachable — into dashboard.gui.

Three things have to agree or the branch is invisible: the tree node, the panelLibrary page keyed by that
node's PATH, and a viewport on Main pointed at the node (a page no viewport points at is deleted at save,
SurfaceTabs::pruneUnplacedPages). Doing all three here is what makes the result openable.

Every binding is checked against the real meta before anything is written: a page of controls bound to
paths that do not exist looks exactly like a page that works until you click it.
"""
import json, sys, uuid, copy, os, pathlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import fuel_pages as F

# Paths come from paths.py — one definition, resolved from the repo and keyed by board.
# Re-exported because eight scripts already do `from apply_fuel import DOC`.
from paths import DOC, META, BOARD, check_board   # noqa: F401

ROOT = 'Configuration'
BRANCH = 'Fuel Tuning'

# ---- the branch, in the order the work is actually done -----------------------------------------
def build():
    """The branch, at one node per table — the granularity the reference tree uses."""
    import fuel_tree as FT
    FC, ENG = F.FC, F.ENG
    pages, tree = {}, []

    def add(path, name, page, cond=None, children=None):
        pages[path] = page
        n = {'name': name, 'expanded': False}
        if cond: n['condition'] = cond
        if children: n['children'] = children
        return n

    B = f'{ROOT}/{BRANCH}'
    # Setup first, then the two maps everything else corrects, then starting, in the order the work is done.
    tree.append(add(f'{B}/Fuel Setup', 'Fuel Setup', F.page_setup()))
    # The fuel twin of the ignition ledger: the charge, then every correction that scales it, then the
    # commanded pulse. The corrections have had their own channels since the module was written.
    import top_pages as _T
    tree.append(add(f'{B}/Fuel Breakdown', 'Fuel Breakdown', _T.page_fuel_breakdown()))
    tree.append(add(f'{B}/VE Table', 'VE Table', F.page_ve()))
    tree.append(add(f'{B}/Target Lambda', 'Target Lambda', F.page_target_lambda()))
    # THE LEARNED TRIM SITS NEXT TO THE MAP IT CORRECTS, because it is the same grid: it borrows
    # ve_table's axes outright, so cell for cell it says what the VE table is missing here. That is
    # what makes "Apply to Base Table" a copy rather than a resample.
    tree.append(add(f'{B}/Long Term Fuel Trim', 'Long Term Fuel Trim',
                    FT.table_page('Long Term Fuel Trim', FC + 'lambda_ltft',
                                  [('LTFT', 'ltft_pct', '%.1f'), ('STFT', 'stft_pct', '%.1f'),
                                   ('Lambda', 'lambda_1', '%.2f'), ('Target', 'lambda_target', '%.2f')],
                                  'What closed loop has learned the VE table is missing, on the VE '
                                  'table\'s own grid. It is applied from the moment the engine starts — '
                                  'before a wideband has warmed up, which is where its value is. When '
                                  'the surface stops moving, fold it into the VE table and reset it: '
                                  'the trim is meant to end up in the map, not to live here forever. '
                                  'If cells sit pinned at the authority limit the base map is wrong, '
                                  'and raising the limit hides that rather than fixing it.',
                                  learned='apply+reset'),
                    cond='[#lambda.ltft_enabled] == 1'))
    tree.append(add(f'{B}/Bank Trim', 'Bank Trim',
                    FT.table_page('Bank Trim', FC + 'lambda_bank_trim',
                                  [('Bank 1', 'ltft_bank_1_pct', '%.1f'),
                                   ('Bank 2', 'ltft_bank_2_pct', '%.1f'),
                                   ('LTFT', 'ltft_pct', '%.1f')],
                                  'How far each bank sits from the engine-wide trim — two numbers, not '
                                  'a second map, because what separates one bank from the other is '
                                  'mostly injector flow and manifold bias rather than something that '
                                  'varies with RPM and load. A single-bank engine uses the first and '
                                  'ignores the second. Multi-point injection fires every injector on '
                                  'one pulse and cannot deliver a difference between banks, so there '
                                  'the two are averaged. With bank 2 disabled there is one loop for the '
                                  'whole engine, nothing is learned here, and nothing stored here is '
                                  'applied.',
                                  learned='reset'),      # two scalars: no base map to fold them into
                    cond='[#lambda.ltft_enabled] == 1 and [#engine.cylinder_count] > 0'))
    # THE BLEND MODEL'S LOW-RPM MAP: VE against RPM x throttle, whose air (at baro) crossfades by RPM into
    # the VE table's air on measured MAP. Appears when Blend is the air model — nothing else reads it.
    tree.append(add(f'{B}/Alpha-N VE Table', 'Alpha-N VE Table', ALPHA_VE_PAGE(),
                    cond='[#fuel_calculator.fuel_model] == 3'))
    # The WHOLE start sequence on one page — prime, crank, flood clear, post-start, warmup — in the
    # order it happens. The per-table nodes below are for tuning one curve; this is for understanding
    # (and for the flood-clear box, which belongs beside the cranking threshold and nowhere else).
    # It was written and never installed: a page that exists only in a builder is a page nobody can open.
    tree.append(add(f'{B}/Start & Warmup', 'Start & Warmup', F.page_start_warmup()))
    tree.append(add(f'{B}/Fuel Prime Pulse', 'Fuel Prime Pulse',
                    FT.table_page('Fuel Prime Pulse', FC + 'prime_fuel_table',
                                  [('Prime PW', 'prime_pw', '%.0f'), ('Coolant', 'clt', '%.0f')],
                                  'Fired once per power-up, at first sync while cranking.',
                                  extra_fields=[('Prime Mode', FC + 'prime_mode', 'enum', '')],
                                  enable='[#fuel_calculator.prime_enable] == 1')))
    tree.append(add(f'{B}/Cranking', 'Cranking',
                    FT.table_page('Cranking', FC + 'cranking_fuel_table',
                                  [('Cranking Corr', 'fuel_corr_cranking', '%.3f'),
                                   ('Engine RPM', 'rpm', '%.0f'), ('Coolant', 'clt', '%.0f')],
                                  'Applies below the cranking threshold, until the engine catches.',
                                  extra_fields=[('Cranking Threshold', ENG + 'cranking_rpm', 'configedit', 'RPM')],
                                  enable_path=FC + 'enable_cranking')))

    # Corrections: every multiplier that sits on top of the VE table, per-cylinder trims included. Each
    # carries its own enable now, so the page it lives on shows the tick and the folder gets a real
    # switchboard rather than a list of names.
    # THE TREE SHOWS WHAT IS ON. A correction that is switched off is not in the calculation, so it is
    # not in the list of things to tune either — it is turned back on from the Corrections switchboard,
    # which lists every one of them with its tick whether or not it is running. Same rule the feature
    # nodes already follow; the difference is only that corrections had not been given it.
    corr = [add(f'{B}/Corrections/{name}', name,
                FT.table_page(name, table, live, note, extra, enable_path=flag),
                cond=f'[#{flag}] == 1')
            for name, table, live, note, extra, flag in FT.CORRECTIONS]
    for n in range(1, 13):
        corr.append(add(f'{B}/Corrections/Cylinder {n}', f'Cylinder {n}',
                        FT.table_page(f'Cylinder {n}', f'{FC}cyl{n}_fuel_corr_table',
                                      [(f'Lambda {n}', f'lambda_{n}', '%.2f'),
                                       ('Target', 'lambda_target', '%.2f'),
                                       ('LTFT', 'ltft_pct', '%.1f')],
                                      'Per-cylinder trim, set by hand — closed loop corrects the '
                                      'engine as a whole, not one cylinder. Watch this cylinder\'s '
                                      'own wideband against the target and trim until they agree. '
                                      'Delivered in sequential injection only.'),
                        cond=f'[#engine.cylinder_count] >= {n}'))
    pages[f'{B}/Corrections'] = FT.page_corrections_branch()
    tree.append({'name': 'Corrections', 'expanded': False, 'children': corr})

    # A stage is a group of tables about one set of injectors, so each table is a leaf under it.
    for st in (1, 2, 3, 4):
        kids = [add(f'{B}/Stage {st}/{name}', name,
                    FT.table_page(f'Stage {st} — {name}', f'{FC}stage{st}_{suffix}', live, note))
                for name, suffix, live, note in FT.STAGE_TABLES]
        kids.insert(0, add(f'{B}/Stage {st}/Setup', 'Setup', F.page_injector_stage(st)))
        # THE STAGE'S FUEL — its settings, density and composition trim, on one page (fuel_pages).
        kids.insert(1, add(f'{B}/Stage {st}/Fuel', 'Fuel', F.page_stage_fuel(st)))
        tree.append({'name': f'Stage {st}', 'expanded': False,
                     'condition': f'[#engine.num_inj_stages] >= {st}', 'children': kids})

    # THE TRANSIENT STRATEGY THAT REPLACES CLASSIC TRANSIENT FUEL: MAP prediction + the fuel film, and
    # every table of both, under one node. Appears when either half is on.
    MPF = f'{B}/MAP Prediction & Fuel Film'
    PRED = '[#fuel_calculator.map_predict_enabled] == 1'
    FILM = '[#fuel_calculator.wallfilm_enabled] == 1'
    mpf_kids = [
        add(f'{MPF}/Predicted MAP', 'Predicted MAP', PREDICTED_MAP_PAGE(), cond=PRED),
        add(f'{MPF}/Transient TPS Scaling', 'Transient TPS Scaling',
            FT.table_page('Transient TPS Scaling', FC + 'map_predict_scale_table',
                          [('Throttle Rate', 'tps_rate', '%.0f'), ('Throttle', 'tps', '%.1f'),
                           ('MAP Source', 'map_source', '%s'), ('MAP (est)', 'map_est', '%.1f')],
                          'The throttle RATE at which the full predicted MAP is used, per operating point. '
                          'Below it, rate / cell of the way from measured to predicted (half the rate, half '
                          'way); under a tenth of the cell is noise and ignored. Watch Throttle '
                          'Rate with your foot still — whatever it wanders by is noise, and this wants to be '
                          'about ten times that, or the noise triggers prediction at constant throttle.'),
            cond=PRED),
        add(f'{MPF}/Film Pooling Percentage', 'Film Pooling Percentage',
            FT.table_page('Film Pooling Percentage', FC + 'film_pool_table',
                          [('Film Correction', 'fuel_corr_film', '%.3f'), ('Coolant', 'clt', '%.0f'),
                           ('MAP', 'map', '%.1f')],
                          'How much of each injection stays on the port wall, against coolant and manifold '
                          'pressure: 5-10 % warm with a well-matched injector and port, 25-30 % for sharp inlet '
                          'turns, around 50 % cold. As little as gives a clean transient.'),
            cond=FILM),
        add(f'{MPF}/Film Evaporation Time Constant', 'Film Evaporation Time Constant',
            FT.table_page('Film Evaporation Time Constant', FC + 'film_evap_table',
                          [('Film Correction', 'fuel_corr_film', '%.3f'), ('Engine RPM', 'rpm', '%.0f'),
                           ('Coolant', 'clt', '%.0f')],
                          'How fast the film comes off the wall, as a time constant: about 98 % of it has gone '
                          'in four times this. Around 200 ms warm and at speed, up to 400 ms cold or at low RPM.'),
            cond=FILM),
    ]
    tree.append(add(MPF, 'MAP Prediction & Fuel Film', F.page_map_prediction(),
                    cond=f'{PRED} || {FILM}', children=mpf_kids))
    tt = add(f'{B}/Transient Throttle', 'Transient Throttle', F.page_transient())
    # A transient table may name the flag the firmware gates it on as a fifth element; the node carries
    # it too, so a curve nothing reads is out of the tree rather than merely greyed.
    tt['children'] = [add(f'{B}/Transient Throttle/{name}', name,
                          FT.table_page(name, table, live, note, enable_path=(e[0] if e else '')),
                          cond=(f'[#{e[0]}] == 1' if e else ''))
                      for name, table, live, note, *e in FT.TRANSIENT_TABLES]
    tree.append(tt)
    return {'name': BRANCH, 'expanded': True, 'children': tree}, pages


# ---- validation ---------------------------------------------------------------------------------
def load_meta():
    s = open(META, encoding='utf-8', errors='replace').read()
    return json.JSONDecoder().raw_decode(s, 0)[0]


def _pc_declared(name):
    """A HOST variable ("pc.<name>"): declared in the schema's pc_vars and carried in the meta's
    "pcVars", which is where the studio gets them too."""
    try:
        return any(v.get('name') == name for v in load_meta().get('pcVars', []))
    except Exception:
        return False


def resolve_config(meta, path):
    """'fuel_calculator.ve_table' / 'engine.inj_stage[0].mode' / 'pc.diag_view' -> is it declared?"""
    import re
    if path.startswith('pc.'):
        return _pc_declared(path[3:])
    parts = path.split('.')
    cur = meta['config']
    for i, part in enumerate(parts):
        m = re.match(r'^([a-z0-9_]+)\[([^\]]*)\]$', part)
        key = m.group(1) if m else part
        if not isinstance(cur, dict) or key not in cur:
            return False
        cur = cur[key]
        if m:                                    # an element: descend into its fields — AND its tables.
            # An array-of-structs declares per-element TABLES beside its fields ("etb[1].ff_table"), and
            # looking only at `fields` refused a binding that resolves perfectly well. It went unnoticed
            # because the page that used it wrote "etb[*]" — which failed the same way, and was never
            # checked, because the template pages predate this validator.
            fields = cur.get('fields', {})
            tables = cur.get('tables', {})
            # …AND its nested ARRAYS. A stream's pattern cells are one ("trigger.streams[0].cell[].v"):
            # a run of little structs inside each element. Without them the only page that edits a
            # pattern could not be validated at all — every cell binding read as "no such config field",
            # which is why the stream pages were built by hand outside this checker in the first place.
            arrays = cur.get('arrays', {})
            cur = {**tables, **fields, **arrays} if (fields or tables or arrays) else cur
    return True


def validate(meta, pages, node_paths=None):
    tel = meta['telemetry']
    channels = set(tel.keys() if isinstance(tel, dict) else [t['id'] for t in tel])
    bad = []
    import re as _re
    def check_expr(page, w, key):
        """Every [#path] a condition names must exist. A rule pointing at a field that is not there is
        not an error anywhere — it simply evaluates false for ever, so the control it gates is greyed
        (or hidden) permanently and nothing says why."""
        expr = w['props'].get(key, '')
        # Depth-aware: a path carries its own subscript ("engine.inj_stage[0].mode"), so stopping at the
        # first ']' reads the reference as "engine.inj_stage[0" and calls a perfectly good rule broken.
        for ref in _re.findall(r'\[#((?:[^\[\]]|\[[^\]]*\])+)\]', expr):
            if not resolve_config(meta, ref):
                bad.append((page, w['type'], f'{key}: {ref}', 'no such config field'))

    def walk(ws, page):
        for w in ws:
            for k in ('condition', 'enableCondition'):
                if w['props'].get(k): check_expr(page, w, k)
            sig = w['props'].get('signalName', '')
            if sig:
                s = sig.strip()
                if s.startswith('[$') and s.endswith(']'):
                    if s[2:-1] not in channels: bad.append((page, w['type'], sig, 'no such channel'))
                elif '.' in s:
                    if not resolve_config(meta, s): bad.append((page, w['type'], sig, 'no such config field'))
                elif s not in channels:
                    bad.append((page, w['type'], sig, 'no such channel'))
            link = w['props'].get('link', '')
            if link and node_paths is not None and link not in node_paths:
                bad.append((page, w['type'], f'link: {link}', 'no such tree node'))
            kids = w['props'].get('children')
            if kids: walk(json.loads(kids), page)
    for path, pg in pages.items():
        walk(pg['widgets'], path)
    return bad


# ---- install ------------------------------------------------------------------------------------
def ALPHA_VE_PAGE():
    import fuel_tree as FT
    return FT.table_page('Alpha-N VE Table', F.FC + 'alpha_ve_table',
                         [('Alpha-N VE', 've_alpha', '%.1f'), ('Throttle', 'tps', '%.1f'),
                          ('Engine RPM', 'rpm', '%.0f'), ('Alpha-N Share', 'blend_alpha_share', '%.0f'),
                          ('Charge Load', 'charge_load', '%.1f')],
                         'The Blend air model\'s low-RPM map: VE against RPM and throttle, its air taken at '
                         'atmosphere. Between Blend Start and End RPM its air crossfades into the VE table\'s '
                         '(on measured MAP); Alpha-N Share says how much of the charge is still this map\'s. Each range has its own cells, so tune '
                         'this one below the crossover and the VE table above it.')


def PREDICTED_MAP_PAGE():
    import fuel_tree as FT
    return FT.table_page('Predicted MAP', F.FC + 'predicted_map_table',
                         [('MAP', 'map', '%.1f'), ('Throttle', 'tps', '%.1f'), ('Engine RPM', 'rpm', '%.0f')],
                         'What the manifold WOULD read at this RPM and throttle, for MAP prediction: while the '
                         'throttle moves fast the averaged MAP reading lags the plenum, and fuelling takes the '
                         'higher of this and the measured value until the intake settles (the lower on a fast '
                         'lift, with Predict Tip-Out). Used only during a transient. Auto Tune learns it from '
                         'steady running (Tune: Predicted MAP).')


def main():
    meta = load_meta()
    branch, pages = build()
    pages = {k: v.to_json() for k, v in pages.items()}

    # Check links against the tree AS IT WILL BE — this branch's own nodes included, since the pages
    # being installed link to each other.
    d_preview = json.load(open(DOC))
    cfg_prev = next(n for n in d_preview['tree'] if n['name'] == ROOT)
    cfg_prev['children'] = [c for c in cfg_prev['children'] if c['name'] not in ('Fuel Tuning', BRANCH)]
    cfg_prev['children'].append(branch)
    paths = set()
    def _walk(ns, pre=''):
        for n in ns:
            q = f'{pre}/{n["name"]}' if pre else n['name']
            paths.add(q); _walk(n.get('children', []), q)
    _walk(d_preview['tree'])
    bad = validate(meta, pages, paths)
    if bad:
        print(f'REFUSING TO WRITE — {len(bad)} binding(s) name nothing:')
        for p, t, s, why in bad[:40]:
            print(f'   {p:<52} {t:<11} {s:<48} {why}')
        return 1

    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']

    # Out with the seeded "Fuel Tuning" branch: its 40 flat nodes are one per table, machine-named and
    # machine-ordered, and exactly one of them had a page (a single table widget) which the new Stage 2
    # page carries properly.
    olds = ('Configuration/Fuel Tuning', 'Configuration/Fuel')
    dropped_pages = [k for k in lib if any(k == o or k.startswith(o + '/') for o in olds)]
    for k in dropped_pages: del lib[k]
    before_vp = len(main_model['widgets'])
    main_model['widgets'] = [w for w in main_model['widgets']
                             if not (w['type'] == 'viewport'
                                     and any(w['props'].get('node', '') == o
                                             or w['props'].get('node', '').startswith(o + '/') for o in olds))]
    dropped_vps = before_vp - len(main_model['widgets'])

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    # ADOPT WHAT WAS MOVED IN. Rebuilding a branch replaces its children, so anything another pass put
    # here — O2 Control, which is the Lambda module living inside Fuel Tuning — would be dropped along
    # with its pages. Keep every child this build does not own.
    existing = next((c for c in cfg['children'] if c['name'] in ('Fuel', BRANCH)), None)
    if existing:
        mine = {c['name'] for c in branch['children']}
        adopted = [c for c in existing.get('children', []) if c['name'] not in mine]
        if adopted:
            branch['children'] = adopted + branch['children']
            print(f'adopted {len(adopted)} child branch(es) already here: '
                  + ', '.join(c['name'] for c in adopted))
    cfg['children'] = [c for c in cfg['children'] if c['name'] not in ('Fuel', BRANCH)]
    # Where the old node was: the branch keeps its place in the tree, so nothing else moves this pass.
    cfg['children'].insert(min(2, len(cfg['children'])), branch)

    for path, pg in pages.items():
        lib[path] = pg

    # NO VIEWPORT PER PAGE. A viewport is a window onto whichever page the tree has selected, so
    # the one a surface carries reaches all of them (see collapse_viewports.py). Creating one per
    # page here rebuilds the stack a document was migrated out of -- 189 of them, last time these
    # ran after the migration.

    json.dump(d, open(DOC, 'w'), indent=2)
    widgets = sum(len(p['widgets']) for p in pages.values())
    print(f'installed {len(pages)} pages ({widgets} top-level widgets)')
    print(f'dropped {len(dropped_pages)} old page(s), {dropped_vps} old viewport(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
