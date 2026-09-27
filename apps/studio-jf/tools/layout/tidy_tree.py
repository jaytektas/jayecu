"""Nodes that name a C array rather than a job — drop them, or make them a page.

The tree was seeded from the schema, and a few of the seeds are the machine's names for a config array:
`cyl`, `output`, `cell_pool`, `protection_levels`. They have no page and no children, so clicking one
leaves the viewport on whatever was there before — which reads as a broken link rather than as "there is
nothing here". Their parents already present those arrays properly (Cylinders & Firing is a grid of the
cylinders; Outputs is the output list), so the node is not hiding anything: it is a leftover.

`streams` is the one that is really a branch — six trigger inputs, each with its own page — so it keeps
its place and gets a human name and a page listing what is under it.

Run after the installers; it is idempotent, and it only ever touches nodes it can name.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import top_pages as T
from apply_fuel import DOC

# Machine-named nodes with nothing behind them. Listed explicitly rather than detected: "a node with no
# page" is also true of a node whose page has not been written YET, and this file must never be the
# reason one goes missing.
DROP = ['cyl', 'cell_pool', 'cyl_bank', 'protection_levels', 'threshold_monitors', 'gear_ratio',
        'output']
RENAME = {'streams': 'Trigger Streams'}


def main():
    d = json.load(open(DOC))
    lib = d['panelLibrary']

    dropped, renamed = [], []

    def walk(node, pre):
        kids = node.get('children')
        if not kids:
            return
        keep = []
        for c in kids:
            path = f'{pre}/{c["name"]}'
            if c['name'] in DROP and not c.get('children') and path not in lib:
                dropped.append(path)
                continue
            keep.append(c)
            walk(c, path)
        node['children'] = keep

    for t in d['tree']:
        walk(t, t['name'])

    # THE RENAME, in every place a path is stored: the node, the pages keyed by it, the viewports that
    # point at them and any link that names one. A path is an identity here, not a label.
    def rename(node, pre):
        for c in node.get('children', []):
            old = f'{pre}/{c["name"]}'
            if c['name'] in RENAME:
                c['name'] = RENAME[c['name']]
                new = f'{pre}/{c["name"]}'
                renamed.append((old, new))
                for k in [k for k in lib if k == old or k.startswith(old + '/')]:
                    lib[new + k[len(old):]] = lib.pop(k)
                for s in d['surfaces']['pool']:
                    for w in s['model']['widgets']:
                        for prop in ('node', 'link'):
                            v = w['props'].get(prop, '')
                            if v == old or v.startswith(old + '/'):
                                w['props'][prop] = new + v[len(old):]
                rename(c, new)
            else:
                rename(c, old)

    for t in d['tree']:
        rename(t, t['name'])

    # …and the branch node's own page, now that it has a name worth showing.
    made = 0
    for _old, new in renamed:
        node = None
        def find(nodes, pre):
            for c in nodes:
                p = f'{pre}/{c["name"]}'
                if p == new:
                    return c
                got = find(c.get('children', []), p)
                if got:
                    return got
            return None
        for t in d['tree']:
            node = node or (t if t['name'] == new else find(t.get('children', []), t['name']))
        if node is None or new in lib:
            continue
        leaf = new.rsplit('/', 1)[-1]
        rows = [(c['name'], f'{new}/{c["name"]}') for c in node.get('children', [])]
        lib[new] = T.page_index(leaf, rows,
                                'Every trigger input this ECU reads, and what each one is wired to. A '
                                'stream with no signal assigned is not read at all.').to_json()
        made += 1

    # …and pages left keyed to a node that no longer exists. A page nothing can reach is not harmless:
    # it is carried in every save, it shows up in the library, and the next reader has to work out
    # whether it matters. ('Configuration/Fuel' was one — the fuel branch's own switchboard, installed
    # under the branch's old name, so the page existed and nothing could open it.)
    paths = set()
    def _paths(nodes, pre):
        for c in nodes:
            p = f'{pre}/{c["name"]}'
            paths.add(p)
            _paths(c.get('children', []), p)
    for t in d['tree']:
        paths.add(t['name'])
        _paths(t.get('children', []), t['name'])
    orphans = [k for k in lib if k not in paths]
    for k in orphans:
        del lib[k]
    for s in d['surfaces']['pool']:
        s['model']['widgets'] = [w for w in s['model']['widgets']
                                 if not (w['type'] == 'viewport' and w['props'].get('node') in orphans)]

    # A page with no viewport is a page nobody can open.
    main_model = d['surfaces']['pool'][0]['model']
    have = {w['props'].get('node') for w in main_model['widgets'] if w['type'] == 'viewport'}
    ref = next(w for w in main_model['widgets'] if w['type'] == 'viewport')
    nid = max(w['id'] for w in main_model['widgets']) + 1
    added = 0
    for path in lib:
        if path in have:
            continue
        vp = json.loads(json.dumps(ref))
        vp['id'] = nid; nid += 1
        vp['uid'] = str(uuid.uuid4())
        vp['props'] = dict(vp['props'])
        vp['props'].update({'node': path, 'title': path.rsplit('/', 1)[-1], 'labelText': '',
                            'signalName': ''})
        vp['x'], vp['y'], vp['w'], vp['h'] = 0, 92, A.CANVAS_W, A.CANVAS_H
        main_model['widgets'].append(vp)
        added += 1

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'tidy: dropped {len(dropped)} machine-named node(s) and {len(orphans)} unreachable page(s), '
          f'renamed {len(renamed)}, {made} index page(s), {added} new viewport(s)')
    for k in orphans:
        print(f'   x {k}')
    for p in dropped:
        print(f'   - {p}')
    for o, n in renamed:
        print(f'   ~ {o}  ->  {n}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
