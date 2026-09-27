"""Install a page for EVERY physical output, named by its pin (IGN1, LS7, HS3).

outputs.output[i] IS pin i. Every pin's node is always in the tree, because its page is where what the
pin does is set: a coil and its cylinder, an injector with its cylinder and stage, or a generic output.
Its Frequency page appears only for a generic PWM output.

Outputs had one page with the slot chosen at the top — the same asymmetry the sensors had before
apply_sensors, and the same fix. One page BODY serves all forty-two: it is written with `[*]`
bindings and the viewport that shows it carries the element (`outputs.output[7]`), which is what
`[*]` resolves to.

WHY A PICKER WAS THE WRONG SHAPE. It is a second place to say which thing you are looking at, and it
disagrees with the tree that got you there — you can navigate to Output 7 and be shown Output 3. It
also made every neighbouring page depend on it: the Frequency node was gated on the SELECTED slot's
kind, so which pages existed changed as you moved a dropdown.

"""
import json, sys, os, uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import more_pages as MP
from apply_fuel import DOC, ROOT, load_meta

OUTPUTS = f'{ROOT}/Electrical/Outputs'


def main():
    d = json.load(open(DOC))
    lib, pool = d['panelLibrary'], d['surfaces']['pool']
    main_model = pool[0]['model']

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    elec = next((c for c in cfg['children'] if c['name'] == 'Electrical'), None)
    outs = next((c for c in (elec or {}).get('children', []) if c['name'] == 'Outputs'), None)
    if outs is None:
        print('no Electrical/Outputs node in the tree'); return 1

    count = load_meta()['config']['outputs']['output']['count']

    # THE OLD SHAPE GOES. "Output Setup" was the one page behind the picker and "Frequency" hung off
    # the same selection; both are per-output now and live under the output they belong to.
    for gone in (f'{OUTPUTS}/Output Setup', f'{OUTPUTS}/Frequency'):
        lib.pop(gone, None)
    outs['children'] = [c for c in outs.get('children', [])
                        if c['name'] not in ('Output Setup', 'Frequency')]
    # THE TACHO IS A GENERIC OUTPUT NOW (its template on any pin's wizard), so the page for the module that
    # used to drive it goes, node and all.
    elec['children'] = [c for c in elec.get('children', []) if c['name'] != 'Tacho Output']
    lib.pop(f'{ROOT}/Electrical/Tacho Output', None)

    # One body, reused. The library is keyed by node path so each node needs its own entry, but the
    # page is identical for every slot — that is the whole point of a template.
    body = MP.page_output_setup().to_json()
    freq = MP.page_freq_map().to_json()
    meta = load_meta()
    wiring = meta.get('wiring') or {}
    # The physical connectors, so a terminal can name the SHELL IT IS IN. "CN2-25" is only useful to
    # someone who already knows which connector CN2 is; the shell is colour-coded and the colour is
    # the thing you actually look for on the loom, so it belongs in the same breath as the number.
    conns = meta.get('connectors') or {}

    def where_wired(w):
        """"CN2-25" + "W" -> "CN2 (WHITE 25), wire W". Anything that is not a CNx-nn terminal is
        passed through untouched rather than guessed at."""
        pin, col = w.get('pin', ''), w.get('color', '')
        if pin and '-' in pin:
            shell, _, term = pin.partition('-')
            shell_col = (conns.get(shell) or {}).get('color', '')
            if shell_col and term:
                pin = f"{shell} ({shell_col.upper()} {term})"
        return ', '.join(x for x in (pin, f"wire {col}" if col else '') if x)

    # THE OLD NAMES GO: "Output N" was a slot number, and a slot number is not a pin.
    for i in range(count):
        for suffix in ('', '/Frequency'):
            lib.pop(f'{OUTPUTS}/Output {i + 1}{suffix}', None)
    names = MP.output_row_names(count)

    by_name = {c['name']: c for c in outs.get('children', [])}
    live, pages = set(), {}
    for i in range(count):
        name = names[i]
        node = by_name.get(name) or {'name': name, 'expanded': False}
        node.pop('condition', None)          # every pin is always reachable: its page sets what it does
        # The carrier page exists only for a generic PWM output: a coil, an injector and a plain level
        # have no carrier to set. Asked of THIS pin rather than of whatever a dropdown was pointing at.
        node['children'] = [{'name': 'Frequency', 'expanded': False,
                             'condition': f'[#outputs.output[{i}].function] == 3 && '
                                          f'[#outputs.output[{i}].kind] == 0'}]
        by_name[name] = node

        p = f'{OUTPUTS}/{name}'
        # THE ELEMENT RIDES ON THE PAGE — one body serves all 42 outputs, written with `[*]`, and this is
        # what those resolve to. It was the showing viewport's job while the Main surface had one per page.
        # THE PIN AND WHERE IT IS WIRED is a ROW now, not a title — a title is text and this is two
        # colours. The panel keeps its own generic name and the pinwire row draws the connector badge
        # and the wire, resolving both from the resource name substituted below.
        # %ROW% -> this page's output index. Every other binding on the shared body resolves through
        # elementScope, but a CLI command takes a NUMBER, and the Bench Test buttons send
        # `test <row> ...`. Substituted on the serialised body so it reaches panel children too, which
        # are themselves JSON strings.
        pages[p] = json.loads(json.dumps(body).replace(MP.PIN_RES, name).replace('%ROW%', str(i)))
        pages[p]['title'] = name
        pages[p]['elementScope'] = f'outputs.output[{i}]'
        pages[f'{p}/Frequency'] = json.loads(json.dumps(freq))
        pages[f'{p}/Frequency']['elementScope'] = f'outputs.output[{i}]'
        live.add(p); live.add(f'{p}/Frequency')

    outs['children'] = [by_name[names[i]] for i in range(count)]
    lib.update(pages)

    # THE VIEWPORTS. Each carries its output as the ELEMENT, which is what every `[*]` on the page
    # reads. Same mechanism the sensors use.
    have = {w['props'].get('node'): w for w in main_model['widgets'] if w['type'] == 'viewport'}
    # PAGES OPEN IN WINDOWS NOW, so the Main surface carries no viewports to clone — this block used
    # to add one per page, and live_strip deletes every one of them again. With nothing to clone it
    # raised StopIteration and took the whole installer with it. Nothing to add is not a failure.
    ref = next((w for w in main_model['widgets'] if w['type'] == 'viewport'), None)
    viewports = ref is not None      # …and when there are none, the whole block below is skipped
    nid = max(w['id'] for w in main_model['widgets']) + 1
    added = 0
    for i in range(count):
        name = names[i]
        for path, title in ((f'{OUTPUTS}/{name}', name),
                            (f'{OUTPUTS}/{name}/Frequency', f'{name} — Frequency')):
            if not viewports:
                continue
            vp = have.get(path)
            if vp is None:
                vp = json.loads(json.dumps(ref))
                vp['id'] = nid; nid += 1
                vp['uid'] = str(uuid.uuid4())
                vp['props'] = dict(vp['props'])
                main_model['widgets'].append(vp)
                added += 1
            vp['props'].update({'node': path, 'labelText': '', 'title': title,
                                'signalName': f'outputs.output[{i}]'})

    # Viewports for the shape that no longer exists.
    stale = [w for w in main_model['widgets']
             if w['type'] == 'viewport' and (w['props'].get('node') or '').startswith(OUTPUTS + '/')
             and w['props']['node'] not in live]
    for w in stale:
        main_model['widgets'].remove(w)

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'outputs: {count} pages (+{count} Frequency) from one template body, '
          f'{added} new viewport(s), {len(stale)} stale dropped; a node per pin')
    return 0


if __name__ == '__main__':
    sys.exit(main())
