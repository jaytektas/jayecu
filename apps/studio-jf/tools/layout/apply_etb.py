"""Install the electronic-throttle pages.

Throttle body B and both half bridges by default — A is left exactly as it was, so the two can be read
side by side and the new one only replaces the old when it has earned it:

    python3 apply_etb.py            # branch pages, body B (+ its settings), half bridges A and B
    python3 apply_etb.py --with-a   # …and body A, once B has been looked at

Same three-way agreement as every other installer, and the same reference check: every signal names a
real channel or config field, every link a node that exists.
"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import etb_pages as E
import function_pages as FN
from apply_fuel import DOC, load_meta, ROOT
from apply_top import validate, node_paths_of

ETB = f'{ROOT}/Engine Functions/Electronic throttle'
# Beside Outputs under Electrical, not under the throttle — see etb_pages.HB. The tree node is created
# here (apply_etb owns these pages) and the old one under Electronic throttle is dropped.
HB  = 'Configuration/Electrical/Half Bridges'


def main(with_a=False):
    meta = load_meta()
    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']

    # THE TREE. Each body gains a Calibration Settings child — eleven numbers that are read once at
    # installation and then never again, which is a page, not a panel on the page you work in.
    PEDAL = f'{ROOT}/Engine Functions/Accelerator Pedal'

    def node_at(path):
        cur = {'children': d['tree']}
        for seg in path.split('/'):
            nxt = next((c for c in cur.get('children', []) if c['name'] == seg), None)
            if nxt is None:
                return None
            cur = nxt
        return cur

    # ---- THE BRIDGES MOVE OUT OF THE THROTTLE BRANCH -------------------------------------------
    # They are general output hardware — the board calls them "usable as ETB servo / stepper / BAC idle
    # valve / DC-PWM" — and the only bidirectional drivers the ECU has. Filed under Electronic throttle,
    # the driver a coil-driven stepper idle valve needs lived inside the throttle branch, where nobody
    # setting one up would look. They belong beside Outputs, which is the other output hardware.
    elec = node_at(f'{ROOT}/Electrical')
    old_hb = node_at(f'{ETB}/Half bridge')
    if elec is not None:
        kids = elec.setdefault('children', [])
        if not any(c['name'] == 'Half Bridges' for c in kids):
            # Ahead of Outputs: two fixed drivers read before forty-four generic slots.
            kids.insert(0, {'name': 'Half Bridges', 'expanded': False,
                            'children': [{'name': f'Half Bridge {E.NAME[i]}', 'expanded': False}
                                         for i in (0, 1)]})
            print('tree: Half Bridges moved to Electrical — they are output hardware, not a throttle part')
    if old_hb is not None:
        etb_node = node_at(ETB)
        if etb_node is not None:
            etb_node['children'] = [c for c in etb_node.get('children', []) if c is not old_hb]
        for k in [k for k in d['panelLibrary']
                  if k == f'{ETB}/Half bridge' or k.startswith(f'{ETB}/Half bridge/')]:
            del d['panelLibrary'][k]

    made_nodes = 0
    bodies = [1] + ([0] if with_a else [])
    for i in bodies:
        body = node_at(f'{ETB}/Throttle Body {E.NAME[i]}') or node_at(f'{ETB}/Throttle body {E.NAME[i]}')
        if body is None:
            print(f'  ! no tree node for throttle body {E.NAME[i]} — skipped'); continue
        kids = body.setdefault('children', [])
        if not any(c['name'] == 'Calibration Settings' for c in kids):
            kids.append({'name': 'Calibration Settings', 'expanded': False})
            made_nodes += 1

    # The node for body A is spelled "Throttle body A" and B's "Throttle Body B" — the tree's own casing,
    # which is what the page keys and every link must match.
    def body_path(i):
        for spelling in (f'Throttle Body {E.NAME[i]}', f'Throttle body {E.NAME[i]}'):
            if node_at(f'{ETB}/{spelling}'):
                return f'{ETB}/{spelling}'
        return f'{ETB}/Throttle Body {E.NAME[i]}'

    # The pedal NODE follows the module, like every other function in the tree. It was left gated on
    # sensors.sensor[app_1].enabled — a leftover, and one that made the tree, the switchboard and the
    # page's own switch disagree about what turning "Accelerator Pedal" off meant.
    pedal_node = node_at(PEDAL)
    if pedal_node is not None and pedal_node.get('condition') != 'app.enabled':
        pedal_node['condition'] = 'app.enabled'
        print('tree: Accelerator Pedal now appears with its MODULE, not with one of its sensors')

    # The pedal map is a 16x16 table over four gear planes and had a 430x160 box on the pedal page; it
    # gets a node of its own, the way a throttle body's Calibration Settings does.
    if pedal_node is not None:
        kids = pedal_node.setdefault('children', [])
        if not any(c['name'] == 'Pedal to Throttle' for c in kids):
            kids.append({'name': 'Pedal to Throttle', 'expanded': False})
            made_nodes += 1

    prot = node_at(f'{ROOT}/Protection/Engine Protection')
    if prot is not None:
        kids = prot.setdefault('children', [])
        for name in ('Protection Levels', 'Threshold Monitors'):
            if not any(c['name'] == name for c in kids):
                kids.append({'name': name, 'expanded': False})
                made_nodes += 1

    pages = {ETB: E.page_etb_branch(), HB: E.page_hb_branch(), PEDAL: E.page_pedal(),
             f'{PEDAL}/Pedal to Throttle': E.page_pedal_map(),
             f'{ROOT}/Engine Functions/Idle Stepper': FN.page_stepper(),
             f'{ROOT}/Protection/Misfire Detection': FN.page_misfire(),
             FN.PROT: FN.page_protection(),
             f'{FN.PROT}/Protection Levels': FN.page_protection_levels(),
             f'{FN.PROT}/Threshold Monitors': FN.page_threshold_monitors()}
    for i in (0, 1):
        pages[f'{HB}/Half Bridge {E.NAME[i]}'] = E.page_half_bridge(i)
    for i in bodies:
        pages[body_path(i)] = E.page_throttle(i)
        pages[f'{body_path(i)}/Calibration Settings'] = E.page_cal_settings(i)
    pages = {k: v.to_json() for k, v in pages.items()}

    bad = validate(meta, pages, node_paths_of(d['tree']))
    if bad:
        print(f'REFUSING TO WRITE — {len(bad)} reference(s) name nothing:')
        for p, t, s, why in bad[:30]:
            print(f'   {p:<52} {t:<11} {s:<44} {why}')
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
    print(f'etb: {len(pages)} pages installed ({"A and B" if with_a else "B only — A untouched"}), '
          f'{made_nodes} new node(s), {added} new viewport(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main('--with-a' in sys.argv))
