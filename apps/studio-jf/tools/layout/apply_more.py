"""Fill in every tree node that had no page: the table branches, and the folders.

A node that opens onto nothing is the one thing a navigation tree must not do — and 60-odd of them did,
including whole branches (O2 trim, cam control, torque, knock) whose tables had never been reachable.

Two passes, in this order:
  1. TABLE PAGES for the child nodes of the table branches (matched to the schema by label).
  2. BRANCH PAGES for every remaining node that has children and no page — built from the tree itself, so
     this covers the folders nobody has got to yet and keeps covering them as the tree grows.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import more_pages as MP
from apply_fuel import DOC, load_meta, ROOT

VIEW = (0, 92, A.CANVAS_W, A.CANVAS_H)


def tree_index(d):
    """{path: node} for the whole tree, and {path: [child names]}."""
    nodes, kids = {}, {}
    def walk(n, pre):
        for c in n.get('children', []):
            p = f'{pre}/{c["name"]}'
            nodes[p] = c
            kids.setdefault(pre, []).append(c['name'])
            walk(c, p)
    for t in d['tree']:
        nodes[t['name']] = t
        walk(t, t['name'])
    return nodes, kids


def clone_elements(d, lib, main_model, nodes):
    """Element pages that exist for one element and not its siblings.

    Three of the six trigger streams had a page and three did not, and Half Bridge A had one while B did
    not — the same asymmetry the sensors had, and the same fix: a page written with [*] bindings is a
    TEMPLATE, so the sibling gets a copy of it and a viewport carrying its own element.
    """
    import copy as _copy
    JOBS = [
        # (template page, [(sibling node, element)] )
        # The trigger streams are NOT here any more: all six are generated from one function
        # (engine_pages.page_stream), which is what stopped them drifting apart. Cloning a page and
        # binding it to another element is for a page written by hand, and there is one left.
        (f'{ROOT}/Electrical/Half Bridges/Half Bridge A',
         [('Half Bridge B', 'h_bridge.bridge[1]')]),
    ]
    made = 0
    for tmpl_path, siblings in JOBS:
        tmpl = lib.get(tmpl_path)
        if not isinstance(tmpl, dict):
            print(f'  ! no template at {tmpl_path}')
            continue
        parent = tmpl_path.rsplit('/', 1)[0]
        for name, element in siblings:
            path = f'{parent}/{name}'
            if path not in nodes or path in lib:
                continue
            page = _copy.deepcopy(tmpl)
            page['uid'] = str(uuid.uuid4())
            page['title'] = name
            for w in page['widgets']:                    # fresh uids, as always — see apply_sensors
                w['uid'] = str(uuid.uuid4())
                kids = w['props'].get('children')
                if kids:
                    ks = json.loads(kids)
                    for k in ks:
                        k['uid'] = str(uuid.uuid4())
                    w['props']['children'] = json.dumps(ks, separators=(',', ':'))
            lib[path] = page
            nid = max(w['id'] for w in main_model['widgets']) + 1
            ref = next(w for w in main_model['widgets'] if w['type'] == 'viewport')
            vp = json.loads(json.dumps(ref))
            vp.update({'id': nid, 'uid': str(uuid.uuid4()),
                       'x': VIEW[0], 'y': VIEW[1], 'w': VIEW[2], 'h': VIEW[3]})
            vp['props'] = dict(vp['props'])
            vp['props'].update({'node': path, 'title': name, 'labelText': '', 'signalName': element})
            main_model['widgets'].append(vp)
            made += 1
    return made


def main():
    meta = load_meta()
    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']
    nodes, kids = tree_index(d)

    # A TABLE WITH NO NODE IS AS WRONG AS A NODE WITH NO TABLE, and only the second was ever reported.
    # table_pages() gives a page to any CHILD NODE whose name matches a table's label, so a table added
    # to a module that already has a table branch stays invisible: no node, no page, and coverage.py is
    # the only thing that ever says so. Create the node and let the existing machinery do the rest —
    # which is also why the gain surfaces below appear without a line of page code.
    made_nodes = 0
    for branch, mod, _live, _note in MP.TABLE_BRANCHES:
        node = nodes.get(branch)
        if not node:
            continue
        have = {c['name'] for c in node.get('children', [])}
        for label in sorted(MP.tables_of(meta, mod)):
            if label in have:
                continue
            node.setdefault('children', []).append({'name': label, 'children': []})
            made_nodes += 1
    if made_nodes:
        nodes, kids = tree_index(d)          # the index is stale the moment the tree grows
        print(f'  + {made_nodes} table node(s) that had none')

    pages, missing = MP.table_pages(meta, kids)
    made_tables = len(pages)
    if missing:
        print(f'  ! {len(missing)} node(s) in a table branch match no table: {missing[:3]}')

    # …then every folder that still has no page of its own. Skipped: the ARRAY nodes (`cyl`, `streams`,
    # `output`), which are a data shape rather than a place — their pages are the element pages that live
    # under them, and a switchboard listing "output" once would say nothing.
    ARRAY_NODES = {'cyl', 'streams', 'cell_pool', 'gear_ratio', 'output', 'cyl_bank', 'wb',
                   'protection_levels', 'threshold_monitors', 'sensor'}
    made_branches = 0
    for path, node in sorted(nodes.items()):
        name = path.rsplit('/', 1)[-1]
        if name in ARRAY_NODES or path in lib or path in pages:
            continue
        children = [c['name'] for c in node.get('children', []) if c['name'] not in ARRAY_NODES]
        if not children:
            continue
        rows = [(c, f'{path}/{c}') for c in children]
        pages[path] = MP.branch_page(name, rows,
                                     MP.BRANCH_BLURB.get(path, f'What {name} contains.'))
        made_branches += 1

    # …and the pages that have no module switch of their own: an engine has a cylinder count whether you
    # like it or not.
    #
    # Cylinders & Firing and Ignition System moved to engine_pages/apply_engine, which owns that whole
    # branch — the firing order and the per-cylinder bank/TDC were split across two pages there, which is
    # one table cut in half. They are not installed from here any more: two installers writing the same
    # key means whichever ran last wins, which is not a thing to leave lying around.
    HAND = {f'{ROOT}/Engine Configuration/Vehicle Identity':      MP.page_vehicle_identity,
            f'{ROOT}/Electrical/Outputs':                        MP.page_outputs}
    for path, fn in HAND.items():
        if path in nodes:
            pages[path] = fn()

    # …and the ones whose NODE this installer makes, because the seeded tree never had them. Output
    # Setup is the other half of the outputs switchboard: one slot in full, chosen at the top.
    # Output Setup and Frequency are NOT here any more: apply_outputs installs one of each PER
    # OUTPUT, from a template body, with the node gated on that output's own enable. Two installers
    # writing the same key means whichever ran last wins, which is not a thing to leave lying around.
    MADE = {# The generic pool. Its own branch rather than a corner of Outputs: an output is one of
            # the things that READS a table, and an expression is another.
            f'{ROOT}/Generic Tables': MP.page_generic_tables}
    for _i in range(1, 9):
        MADE[f'{ROOT}/Generic Tables/Generic Table {_i}'] = (lambda i: lambda: MP.page_generic_table(i))(_i)
    MADE_COND = {}
    for path, fn in MADE.items():
        parent_path, leaf = path.rsplit('/', 1)
        parent = nodes.get(parent_path)
        if parent is None:
            print(f'  ! {path}: no parent node — skipped')
            continue
        if path not in nodes:
            parent.setdefault('children', []).append({'name': leaf, 'expanded': False})
            nodes[path] = parent['children'][-1]
            print(f'tree: {path} added')
        if path in MADE_COND:
            nodes[path]['condition'] = MADE_COND[path]
        pages[path] = fn()

    # Validate before writing: every link must name a node that exists.
    bad = []
    for path, pg in pages.items():
        for w in pg.to_json()['widgets']:
            for k in json.loads(w['props'].get('children', '[]') or '[]'):
                link = k['props'].get('link', '')
                if link and link not in nodes:
                    bad.append((path, link))
    if bad:
        print(f'REFUSING TO WRITE — {len(bad)} link(s) name no node:')
        for p, l in bad[:10]:
            print(f'   {p:<52} -> {l}')
        return 1

    for path, pg in pages.items():
        lib[path] = pg.to_json()

    # A PAGE NEEDS NO VIEWPORT OF ITS OWN. The line that was here said "a page is only reachable
    # through a viewport that names it", which was true and is the thing that changed: a viewport is a
    # window onto whichever page the tree has selected, so the one a surface carries reaches all of
    # them. Creating one per page rebuilds the stack a document was migrated out of.
    added = 0

    cloned = clone_elements(d, lib, main_model, nodes)

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'filled in: {made_tables} table page(s), {made_branches} branch page(s), '
          f'{cloned} element page(s) cloned, {added + cloned} viewport(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
