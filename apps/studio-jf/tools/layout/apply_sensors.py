"""Install a page for EVERY sensor, and gate each one's node on its own enable.

118 of the 122 sensors had a tree node with nothing behind it — only the four hand-authored pages (the two
pedals, the two throttles) had anything to show, so the switchboard's links mostly led nowhere.

One page BODY serves them all: it is written with `[*]` bindings and the viewport that shows it carries the
element (`sensors.sensor[clt]`), which is what `[*]` resolves to. That is the shape the four hand-authored
pages already used — this generates the rest, and replaces those four so every sensor reads the same.

Gating matches the feature pages: a sensor's node appears in the tree only while the sensor is enabled, and
the switchboard is how you enable it. The page carries its own switch too, so a sensor found through search
while disabled can be turned on from the page that describes it.
"""
import json, sys, os, uuid, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import sensor_detail as SD
import sensor_page as SP
import sensor_group as SG
from apply_fuel import DOC, ROOT, load_meta

SENSORS = f'{ROOT}/Sensors'


def main():
    d = json.load(open(DOC))
    lib, pool = d['panelLibrary'], d['surfaces']['pool']
    main_model = pool[0]['model']

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    sensors_node = next((c for c in cfg['children'] if c['name'] == 'Sensors'), None)
    if sensors_node is None:
        print('no Sensors node in the tree'); return 1

    rows = [(i, n, g) for i, n, _t, g in SP.catalog_full()]
    fixed_type = {i: t for i, _n, t, _g in SP.catalog_full()}
    dtcs = SP.catalog_dtcs()
    meta = load_meta()
    tel = meta['telemetry']
    arr = meta['config']['sensors']['sensor']
    # id -> the channel that sensor publishes. Not always its id (boost_pressure publishes boost_kpa),
    # which is why the meta carries the mapping and nothing here guesses it.
    sig_of = dict(zip(arr.get('element_ids', []), arr.get('element_signals', [])))

    # THE TREE. Sensors -> GROUP -> the sensors in it. A hundred and twenty-two children on one node is a
    # list you scroll rather than a tree you navigate: nothing is ever more than a few rows from something
    # unrelated, and the tree cannot say what kind of thing you are looking at. The catalogue already
    # groups them — the firmware's own grouping, the one the switchboard prints as headings — so the tree
    # can use it, and each group earns a page of its own that has room for a live reading per input.
    kept = [c for c in sensors_node.get('children', []) if c['name'] not in ('sensor',)]
    by_name = {}
    def _collect(nodes):
        for c in nodes:
            by_name.setdefault(c['name'], c)
            _collect(c.get('children', []))
    _collect(kept)
    for k in [k for k in lib if k.startswith(SENSORS + '/')]:
        del lib[k]

    group_title = dict(SP.GROUP_TITLE)
    # ALPHABETICAL WITHIN EACH GROUP, through the one helper the switchboard uses, so the tree, the
    # group pages and the switchboard all put a name in the same place. (Natural order: the EGTs run
    # 1..12, not 1, 10, 11, 12, 2.)
    by_group = SP.group_sensors(rows)

    pages, made = {}, 0
    groups, group_of = [], {}
    for key, title in SP.GROUP_TITLE:
        if not by_group.get(key):
            continue                                 # a group the catalogue has no inputs for is no node
        groups.append((key, title))
        for sid, _n in by_group[key]:
            group_of[sid] = title
    gnodes = {title: {'name': title, 'expanded': False, 'children': []} for _k, title in groups}
    children = [gnodes[title] for _k, title in groups]

    # THE TREE FOLLOWS THE SAME ORDER, which it only does by walking the same lists. Walking `rows`
    # here left the tree in catalogue order while the pages beside it were alphabetical — the same
    # sensors, two orders, which is worse than either one on its own.
    for group, _title in [(k, t) for k, t in groups]:
      for sid, name in by_group[group]:
        title = group_title[group]
        node = by_name.get(name) or {'name': name, 'expanded': False}
        node.pop('children', None)                  # Diagnostics and Wiring are panels on the page now
        node['condition'] = f'sensors.sensor[{sid}].enabled'
        gnodes[title]['children'].append(node)

        # EACH PAGE IS BUILT FOR ITS OWN SENSOR. The shape is the same everywhere, but three things are
        # facts about this one: which checks it has a DTC for (a check that cannot report what it finds
        # is not worth offering — which is what keeps Detect Stuck off a throttle and leaves it on a
        # switch), what those codes are, and whether its type is fixed by the catalogue or the tune's to
        # choose. A single shared body could only ever have shown the union of all of them.
        codes = dtcs.get(sid, {})
        checks = [c for c in ('raw_min', 'raw_max', 'op_min', 'op_max', 'stuck', 'max_deriv')
                  if c in codes]
        type_label = '' if fixed_type[sid] == 'NONE' else fixed_type[sid].replace('_', ' ').title()
        base = f'{SENSORS}/{group_title[group]}/{name}'
        # THE CHECKS GET THEIR OWN NODE. A sensor is set up once from a data sheet and its checks are
        # tuned when something starts reporting a fault it should not — two jobs, at two different times,
        # that were sharing one page and each getting half of it.
        node['children'] = [{'name': 'Diagnostics', 'expanded': False}]
        # THE ELEMENT RIDES ON THE PAGE. One body serves all 128 sensors, written with `[*]`; the sensor
        # it is about used to be carried by the viewport that showed it, and a page opens in a window of
        # its own now. Written here, every `[*]` on the page resolves to THIS sensor however it is opened.
        pg = SD.page(checks=checks, dtc=codes, type_label=type_label,
                     diag_link=f'{base}/Diagnostics').to_json()
        pg['title'] = name
        pg['elementScope'] = f'sensors.sensor[{sid}]'
        pages[base] = pg
        dpg = SD.diag_page(checks=checks, dtc=codes).to_json()
        dpg['title'] = f'{name} — Diagnostics'
        dpg['elementScope'] = f'sensors.sensor[{sid}]'
        pages[f'{base}/Diagnostics'] = dpg
        made += 1

    sensors_node['children'] = children

    # THE GROUP PAGES. Built after the tree, because each row links to the sensor's own node and a link is
    # only worth writing when the node it names exists.
    node_at = {f'{SENSORS}/{group_of[sid]}/{name}' for sid, name, _g in rows}
    def link_of(sid, name):
        path = f'{SENSORS}/{group_of[sid]}/{name}'
        return path if path in node_at else ''
    for key, title in groups:
        gp = SG.page(title, by_group[key], link_of, lambda sid: sig_of.get(sid, ''), tel).to_json()
        gp['title'] = title
        pages[f'{SENSORS}/{title}'] = gp
    lib.update(pages)

    # THE VIEWPORTS. Each carries its sensor as the ELEMENT, which is what every `[*]` on the page reads.
    have = {w['props'].get('node'): w for w in main_model['widgets'] if w['type'] == 'viewport'}
    # PAGES OPEN IN WINDOWS NOW, so the Main surface carries no viewports to clone — this block used
    # to add one per page, and live_strip deletes every one of them again. With nothing to clone it
    # raised StopIteration and took the whole installer with it. Nothing to add is not a failure.
    ref = next((w for w in main_model['widgets'] if w['type'] == 'viewport'), None)
    viewports = ref is not None      # …and when there are none, the whole block below is skipped
    nid = max(w['id'] for w in main_model['widgets']) + 1
    added = 0
    for sid, name, _g in rows:
      for path in (f'{SENSORS}/{group_of[sid]}/{name}',
                   f'{SENSORS}/{group_of[sid]}/{name}/Diagnostics'):
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
        vp['props'].update({'node': path, 'labelText': '',
                            'title': name if path.endswith(name) else f'{name} — Diagnostics',
                            'signalName': f'sensors.sensor[{sid}]'})
        vp['x'], vp['y'], vp['w'], vp['h'] = 0, 92, A.CANVAS_W, A.CANVAS_H

    # …and drop viewports left pointing at pages that no longer exist (the old sub-branch).
    # A GROUP PAGE NEEDS A VIEWPORT TOO — it is a page like any other, and one without a viewport is a
    # tree node that opens nothing.
    for _key, title in groups:
        if not viewports:
            break
        path = f'{SENSORS}/{title}'
        vp = have.get(path)
        if vp is None:
            vp = json.loads(json.dumps(ref))
            vp['id'] = nid; nid += 1
            vp['uid'] = str(uuid.uuid4())
            vp['props'] = dict(vp['props'])
            main_model['widgets'].append(vp)
            added += 1
        vp['props'].update({'node': path, 'title': title, 'labelText': '', 'signalName': ''})
        vp['x'], vp['y'], vp['w'], vp['h'] = 0, 92, A.CANVAS_W, A.CANVAS_H

    live = {f'{SENSORS}/{group_of[sid]}/{name}' for sid, name, _g in rows}
    live |= {f'{SENSORS}/{group_of[sid]}/{name}/Diagnostics' for sid, name, _g in rows}
    live |= {f'{SENSORS}/{title}' for _k, title in groups}
    stale = [w for w in main_model['widgets']
             if w['type'] == 'viewport' and (w['props'].get('node') or '').startswith(SENSORS + '/')
             and w['props']['node'] not in live]
    for w in stale:
        main_model['widgets'].remove(w)

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'sensors: {made} pages built per sensor under {len(groups)} group node(s), '
          f'{added} new viewport(s), {len(stale)} stale dropped; every node gated on its own enable')
    return 0


if __name__ == '__main__':
    sys.exit(main())
