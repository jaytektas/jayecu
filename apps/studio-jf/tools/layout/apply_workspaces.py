"""Install a branch of table pages plus the WORKSPACE tab that watches it.

A tab is a workspace, not a page: it has its own viewport geometry and its own static instrumentation
around it, and neither changes as the tree moves. Main is the general one — full-width viewport under the
live strip. This one gives the viewport the top-left corner (idle tables are small) and spends the rest on
the four numbers idle tuning is actually about, two graphs, and a watch list.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import fuel_tree as FT
import idle_pages as IP
import boost_pages as BP
import diag_page as DG
import live_strip as LS      # the one place the canvas size is stated
from apply_fuel import DOC, ROOT, load_meta

EF = f'{ROOT}/Engine Functions'
# (branch path, module, the module that draws it, tab name). Adding a workspace is adding a line here.
WORKSPACES = [
    (f'{EF}/Idle Control',  'idle',  IP, 'Idle Control'),
    (f'{EF}/Boost Control', 'boost', BP, 'Boost Control'),
]


def install(d, meta, branch_path, module, pages_mod, tab_name):
    lib, pool = d['panelLibrary'], d['surfaces']['pool']
    main_model = pool[0]['model']

    pages = {branch_path: pages_mod.page_setup(meta).to_json()}
    kids = []
    # A TABLE MAY CARRY ITS OWN ENABLE as a fifth element — the flag the firmware actually consults
    # before evaluating it. A table whose feature is switched off is not a table full of zeroes, it is a
    # table nothing reads, and a page that cannot say so invites tuning cells that do nothing.
    # …and a SIXTH for the settings that belong to that table and nowhere else. A correction slot's
    # channel is the clearest case: the curve means nothing without knowing what it is read against,
    # so picking it on the branch page and drawing it here would split one decision across two screens.
    # THE NODE CARRIES THE GATE TOO, not just the widgets on the page. Greying a table inside a page
    # still leaves the page in the tree, so a mode with three dead gain maps listed them alongside the
    # ones it uses and the tuner had no way to tell which was which — which is the whole point of
    # gating. apply_fuel's corrections have done this from the start (cond=); these two branches had
    # not, so all thirteen idle pages staged in open loop.
    for entry in pages_mod.TABLES:
        name, table, live, note = entry[:4]
        flag  = entry[4] if len(entry) > 4 else ''
        extra = entry[5] if len(entry) > 5 else None
        path = f'{branch_path}/{name}'
        # A flag NAME is the module's own field; anything containing `[#` is taken as a whole
        # expression, for a table the firmware gates on more than one thing.
        kw = {}
        if flag.startswith('['):
            kw['enable'] = flag
        elif flag:
            kw['enable_path'] = f'{module}.{flag}'
        pages[path] = FT.table_page(name, f'{module}.{table}', live, note, extra, **kw).to_json()
        node = {'name': name, 'expanded': False}
        if flag:
            node['condition'] = flag if flag.startswith('[') else f'[#{module}.{flag}] == 1'
        kids.append(node)
    # …and the branch's non-table children: a module whose settings include an ARRAY needs a page for
    # it, which a list of tables cannot be. (Idle Up: six external loads, seven fields each.)
    # A builder may ask for the meta — panel rows take their captions and kinds from it, the same way
    # the branch page does. Builders that do not want it keep the old no-argument shape.
    for name, spec in getattr(pages_mod, 'EXTRA_PAGES', {}).items():
        # …and its gate, the same way a table entry carries one. A page whose whole subject is off —
        # the motorised-gate servo on a car with a solenoid — belongs out of the tree, not in it
        # greyed. This loop appended a bare node, which is why those pages went on staging after the
        # table nodes were fixed.
        build, cond = spec if isinstance(spec, tuple) else (spec, '')
        try:
            page = build(meta)
        except TypeError:
            page = build()
        pages[f'{branch_path}/{name}'] = page.to_json()
        node = {'name': name, 'expanded': False}
        if cond:
            node['condition'] = cond
        kids.append(node)

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    ef = next(c for c in cfg['children'] if c['name'] == 'Engine Functions')
    leaf = branch_path.rsplit('/', 1)[-1]
    node = next((c for c in ef['children'] if c['name'] == leaf), None)
    if node is None:
        node = {'name': leaf, 'expanded': False}
        ef['children'].insert(0, node)
    # THE TREE SHOWS WHAT IS ON, and it is set whether or not this installer created the node. It was
    # only set on creation, and these two branches are always MOVED into place by an earlier installer
    # rather than made here — so Idle Control and Boost Control were the only two features in Engine
    # Functions that stayed in the tree while switched off. A branch of tables for a module that is not
    # running is a list of things to tune that do nothing; the feature switchboard is where it goes
    # back on, and it lists every feature with its tick whether or not it is running.
    node['condition'] = f'{module}.enabled'
    node['children'] = kids

    for k in [k for k in lib if k == branch_path or k.startswith(branch_path + '/')]:
        del lib[k]
    lib.update(pages)

    # NO CANVAS STAMPING. This copied the first page's canvas onto every page it had just built, which
    # threw away the size each one measured for itself (author.Page.to_json) — and a page window opens at
    # the size the page declares, so that made every one of them open at some other page's size.

    have = {w['props'].get('node') for w in main_model['widgets'] if w['type'] == 'viewport'}
    # PAGES OPEN IN WINDOWS NOW, so the Main surface carries no viewports to clone — this block used
    # to add one per page, and live_strip deletes every one of them again. With nothing to clone it
    # raised StopIteration and took the whole installer with it. Nothing to add is not a failure.
    ref = next((w for w in main_model['widgets'] if w['type'] == 'viewport'), None)
    if ref is None:
        have = set(pages)          # every page counts as handled, so the loop below adds none
    nid = max(w['id'] for w in main_model['widgets']) + 1
    added = 0
    for path in pages:
        if path in have: continue
        leaf = path.rsplit('/', 1)[-1]
        vp = json.loads(json.dumps(ref)); vp['id'] = nid; vp['uid'] = str(uuid.uuid4()); nid += 1
        vp['props'] = dict(vp['props']); vp['props'].update({'node': path, 'title': leaf, 'labelText': ''})
        main_model['widgets'].append(vp); added += 1

    ws = next((p for p in pool if p.get('name') == tab_name), None)
    if ws is None:
        ws = {'name': tab_name, 'model': json.loads(json.dumps(main_model))}
        ws['model']['uid'] = str(uuid.uuid4())
        pool.append(ws)
    wm = ws['model']
    furniture, wid = pages_mod.workspace_widgets(1, lambda: str(uuid.uuid4()))
    vps = []
    for w in main_model['widgets']:
        if w['type'] != 'viewport': continue
        v = json.loads(json.dumps(w)); v['uid'] = str(uuid.uuid4()); v['id'] = wid; wid += 1
        v['x'], v['y'], v['w'], v['h'] = pages_mod.VIEW
        vps.append(v)
    wm['widgets'] = furniture + vps
    # …and on a workspace tab it is the VIEW that workspace publishes: the rect its instruments leave
    # clear. On the idle tab that is (10,92,900,480) — the readouts start at x=920 and the lower graph at
    # y=582 — which is exactly the free corner, stated once by the module that laid them out.
    vx, vy, vw, vh = pages_mod.VIEW
    wm['pageArea'] = [int(vx), int(vy), int(vw), int(vh)]
    wm['canvasWidth'], wm['canvasHeight'] = LS.CANVAS_W, LS.CANVAS_H

    idx = pool.index(ws)
    if idx not in d['surfaces'].get('open', []):
        d['surfaces'].setdefault('open', []).append(idx)

    print(f'{tab_name}: {len(pages)} pages, {added} viewport(s) on Main; '
          f'tab = {len(furniture)} instruments + {len(vps)} viewports')


def check_pcvars(meta):
    """The host variables the dashboard binds to are DECLARED IN THE SCHEMA (pc_vars), and arrive in the
    meta. This used to write them into a pcvars.json beside the dashboard — which only ever existed in
    the developer's folder, so a user's studio had the pages and not the variables, and every chooser on
    them showed a bare 0. Now this only checks that the schema's option lists still match the pages
    built from them, so a view added here cannot silently go missing from its chooser.
    """
    import knock_pages as KP
    decl = {v['name']: v for v in meta.get('pcVars', [])}
    want = {
        'diag_view':         [nm for nm, _ in DG.VIEWS],
        'knock_noise_cyl':   [f'Cyl {i + 1}' for i in range(KP.CYLS)],
        'test_count':        None, 'test_on_ms': None, 'test_off_ms': None,
        'etb_autotune_rule': None,
    }
    bad = []
    for name, opts in want.items():
        v = decl.get(name)
        if v is None:
            bad.append(f'{name}: not declared (add it to pc_vars in ecu.schema.yaml, then make codegen)')
        elif opts is not None and v.get('options') != opts:
            bad.append(f'{name}: options {v.get("options")} != the pages\' {opts}')
    if bad:
        raise SystemExit('pc_vars out of step with the pages:\n  ' + '\n  '.join(bad))
    print('pcvars: ' + ', '.join(sorted(want)) + ' declared in the meta')


def install_diagnostics(d):
    """The Diagnostics tab: a chooser and one page per subject, no viewport.

    No viewport deliberately, and it cannot have one: a viewport paints only while ITS node is the tree
    selection, so a scrolling diagnostics page would go blank the moment the user touched the tree. The
    sub-tabs are panels stacked on a host variable instead — see Page.subtabs.
    """
    pool = d['surfaces']['pool']
    ws = next((p for p in pool if p.get('name') == 'Diagnostics'), None)
    if ws is None:
        ws = {'name': 'Diagnostics',
              'model': {'uid': str(uuid.uuid4()), 'canvasWidth': LS.CANVAS_W, 'canvasHeight': LS.CANVAS_H,
                        'canvasStatic': 0, 'canvasAnchor': 0, 'guideW': 0, 'guideH': 0, 'title': '',
                        'layout': 0, 'gridColumns': 2, 'focusIndex': 0, 'borderColor': '',
                        'borderWidth': 0, 'borderStyle': 1, 'borderRadius': 4, 'titleFont': '',
                        'titleColor': '', 'titlePadding': 4, 'titleStyle': 1, 'titlePlace': 0,
                        'titleEdge': 0, 'titleAlign': 0, 'widgets': []}}
        pool.append(ws)
    pg, placed, want = DG.page()
    ws['model']['widgets'] = pg.to_json()['widgets']
    # Set the canvas EVERY run, not just when the tab is first built: a tab that already existed kept the
    # size it was created at, which is how Diagnostics stayed 1280x720 on a 1600x800 surface.
    ws['model']['canvasWidth'], ws['model']['canvasHeight'] = LS.CANVAS_W, LS.CANVAS_H
    # NO PAGE WINDOWS ON THIS TAB. It is all instrument — the channel table fills the surface — so there is
    # no corner a page could sit in without covering the very thing the tab exists to show. A ZERO page
    # area says that, and means something different from declaring none at all (which reserves nothing and
    # lets a window have the whole area). A page opened while this tab is in front moves to one that takes
    # windows rather than doing nothing.
    ws['model']['pageArea'] = [0, 0, 0, 0]
    # A tab this run replaced must not leave its old half behind.
    for stale in [p for p in pool if p.get('name') == 'Sensors Live']:
        idx = pool.index(stale)
        d['surfaces']['open'] = [i if i < idx else i - 1 for i in d['surfaces'].get('open', []) if i != idx]
        pool.remove(stale)
    idx = pool.index(ws)
    if idx not in d['surfaces'].get('open', []):
        d['surfaces'].setdefault('open', []).append(idx)
    print(f'Diagnostics: {len(DG.VIEWS)} views, {placed} of {want} channels, nothing dropped'
          if placed == want else f'Diagnostics: {want - placed} channels DROPPED')


def main():
    meta = load_meta()
    d = json.load(open(DOC))
    for branch_path, module, pages_mod, tab_name in WORKSPACES:
        install(d, meta, branch_path, module, pages_mod, tab_name)
    check_pcvars(meta)
    install_diagnostics(d)
    json.dump(d, open(DOC, 'w'), indent=2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
