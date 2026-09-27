"""Install the Engine Configuration branch, and give the trigger its Diagnostics node.

Same three-way agreement as every other installer — tree node, page keyed by the node's PATH, viewport on
Main — plus the reference check apply_top uses: every signal must name a real channel or config field, and
every link a node that exists. The pages here address twelve firing positions and four injection stages
through indexed bindings, which is exactly the kind of thing that is easy to get subtly wrong (the page
this replaces had all four stage rows pointing at stage one), so nothing is written until it all resolves.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import engine_pages as EP
from apply_fuel import DOC, load_meta, ROOT
from apply_top import validate, node_paths_of

ENG  = f'{ROOT}/Engine Configuration'
TRIG = f'{ENG}/Trigger System'


def main():
    meta = load_meta()
    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    eng = next((c for c in cfg['children'] if c['name'] == 'Engine Configuration'), None)
    if eng is None:
        print('no Engine Configuration node in the tree'); return 1

    # THE TRIGGER'S OWN DIAGNOSTICS NODE. Configuration and evidence are different jobs asked at different
    # times — the settings are typed once at installation, the live view is what you watch while cranking
    # a stubborn engine — and a page each lets both be read at a size that works under a bonnet.
    trig = next((c for c in eng.get('children', []) if c['name'] == 'Trigger System'), None)
    if trig is None:
        print('no Trigger System node in the tree'); return 1
    kids = trig.setdefault('children', [])
    if not any(c['name'] == 'Diagnostics' for c in kids):
        kids.insert(0, {'name': 'Diagnostics', 'expanded': False})
        print('tree: Trigger System / Diagnostics added')

    pages = {
        ENG:                        EP.page_top(),
        f'{ENG}/Cylinders & Firing': EP.page_cylinders(),
        f'{ENG}/Ignition System':    EP.page_ignition(),
        f'{ENG}/Fuel System':        EP.page_fuel_system(),
        TRIG:                        EP.page_trigger(),
        f'{TRIG}/Diagnostics':       EP.page_trigger_diagnostics(),
    }
    # THE STREAM PAGES, generated rather than cloned. They were hand-built and then copied from one
    # another, so they had drifted: different margins and a different diagram width on Crank Primary,
    # four pages carrying Cam Intake B1's enable label, two with no title. Same page, six elements.
    pages[f'{TRIG}/Trigger Streams'] = EP.page_streams_branch()
    for i, name in enumerate(EP.STREAM_NAMES):
        pages[f'{TRIG}/Trigger Streams/{name}'] = EP.page_stream(i, name)
    # THE TYPO IN THE TREE. The node was called "Crank Seconddary" — the schema's own label for slot 1
    # is "Crank Secondary" — and the page key, the viewport and every link agreed with the typo, which
    # is why it survived. Renamed in one place here: node, page, viewport and title together, so nothing
    # is left pointing at a name that no longer exists.
    streams_node = None
    for c in trig.get('children', []):
        if c['name'] == 'Trigger Streams':
            streams_node = c
    if streams_node:
        for c in streams_node.get('children', []):
            if c['name'] == 'Crank Seconddary':
                c['name'] = 'Crank Secondary'
                old_key = f'{TRIG}/Trigger Streams/Crank Seconddary'
                new_key = f'{TRIG}/Trigger Streams/Crank Secondary'
                if old_key in lib:
                    lib[new_key] = lib.pop(old_key)
                for w in main_model['widgets']:
                    if w['type'] == 'viewport' and w['props'].get('node') == old_key:
                        w['props']['node'] = new_key
                        w['props']['title'] = 'Crank Secondary'
                print('tree: "Crank Seconddary" renamed to "Crank Secondary"')

    pages = {k: v.to_json() for k, v in pages.items()}

    bad = validate(meta, pages, node_paths_of(d['tree']))
    if bad:
        print(f'REFUSING TO WRITE — {len(bad)} reference(s) name nothing:')
        for p, t, s, why in bad[:40]:
            print(f'   {p:<46} {t:<11} {s:<50} {why}')
        return 1

    lib.update(pages)

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
    print(f'engine: {len(pages)} pages installed, {added} new viewport(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
