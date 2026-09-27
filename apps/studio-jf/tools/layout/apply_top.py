"""Install the category pages (Configuration root, Fuel, Ignition) and the Ignition branch.

Same three-way agreement as apply_fuel: tree node, page keyed by the node's PATH, viewport on Main.
Plus one more check that only matters for a switchboard — every `link` must name a node that EXISTS.
A link to a node nobody made is a name that does nothing when clicked, and looks identical to one that
works until it is tried.
"""
import json, sys, uuid, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
import top_pages as T
import feature_pages as FP

# Pages whose HOME CHANGED. Keyed old -> new; the new path is the one FEATURES (or pages) states, and this
# only cleans up after it. Lua Scripting was filed under Communications, which is where a bus goes: a
# script reads any channel and publishes its own, and the features downstream consume what it writes.
MOVED_PAGES = {
    'Configuration/Communications/Lua Scripting': 'Configuration/Lua Scripting',
    # …and the rest of that category with it. "Communications" held one real branch and two leaves,
    # so every one of its children was reached through a heading that said nothing they did not.
    # CAN Bus carries its own switchboard and stands on its own; datalogging and Lua were never
    # about a bus and are not filed under one.
    'Configuration/Communications/CAN Bus':     'Configuration/CAN Bus',
    'Configuration/Communications/Datalogging': 'Configuration/Datalogging',
}
import sensor_page as SP
from apply_fuel import DOC, load_meta, resolve_config, ROOT

IGN = f'{ROOT}/Ignition Tuning'


def build():
    pages = {}
    tree = []

    def add(path, name, page, cond=None):
        pages[path] = page
        return {'name': name, 'expanded': False, **({'condition': cond} if cond else {})}

    tree.append(add(f'{IGN}/Advance Table', 'Advance Table', T.page_ign_advance()))
    tree.append(add(f'{IGN}/Dwell Time', 'Dwell Time', T.page_ign_dwell()))
    # CRANKING ADVANCE — a conditional base table, so it belongs beside the map it replaces rather than
    # among the corrections, which only add to it.
    import fuel_tree as _FT
    tree.append(add(f'{IGN}/Cranking Advance', 'Cranking Advance',
                    _FT.table_page('Cranking Advance', 'ignition.cranking_ign_table',
                                   [('Advance', 'advance', '%.1f'), ('Coolant', 'clt', '%.0f'),
                                    ('Engine RPM', 'rpm', '%.0f'), ('Base Table', 'ign_base_kind', '%.0f')],
                                   'Held while the engine is CRANKING, in place of the advance map — not '
                                   'added to it. The map is tuned for an engine that is running; at 200 '
                                   'rpm it hands out whatever its lowest RPM bin holds. A few degrees, '
                                   'more when cold because the burn is slower, keeps the kickback torque '
                                   'off the starter. It replaces the slow corrections too: the coolant '
                                   'advance correction reads the same coolant this table does, and '
                                   'applying both would count it twice.',
                                   enable_path='ignition.cranking_ign_enable')))
    tree.append(add(f'{IGN}/Advance Limits', 'Advance Limits', T.page_ign_limits()))
    # WHERE THE NUMBER CAME FROM. Every term of the advance sum is a published channel now, so this is a
    # ledger of the firmware's own arithmetic rather than a second opinion about it.
    tree.append(add(f'{IGN}/Timing Breakdown', 'Timing Breakdown', T.page_ign_breakdown()))
    # Same rule as the fuel corrections: the tree lists the ones that are in the sum, and the folder's
    # own page is the switchboard that turns them on again.
    corr = [add(f'{IGN}/Corrections/{e[0]}', e[0], T.page_ign_correction(e), cond=f'[#{e[3]}] == 1')
            for e in T.IGN_CORRECTIONS]
    tree.append({'name': 'Corrections', 'expanded': False, 'children': corr})
    trims = [add(f'{IGN}/Cylinder Trims/Cylinder {n}', f'Cylinder {n}', T.page_ign_cyl_trim(n),
                 cond=f'[#engine.cylinder_count] >= {n}') for n in range(1, 13)]
    tree.append({'name': 'Cylinder Trims', 'expanded': False, 'children': trims})
    # …and the two branch nodes themselves, which grouped pages without being one. A node with no page
    # leaves the viewport on whatever was there before, which reads as a broken link.
    # A REAL SWITCHBOARD, not an index. With the tree showing only the corrections that are on, this is
    # the one place a switched-off one can be switched back on — an index of links to pages the tree has
    # hidden would be a dead end.
    pages[f'{IGN}/Corrections'] = T.page_ign_corrections_branch()
    pages[f'{IGN}/Cylinder Trims'] = T.page_index(
        'Cylinder Trims', [(f'Cylinder {n}', f'{IGN}/Cylinder Trims/Cylinder {n}') for n in range(1, 13)],
        'Per-cylinder advance offsets, for an engine that does not run every cylinder the same. Only the '
        'cylinders this engine has appear in the tree.')
    # Rotary only: a two-plug engine is the only one with a trailing spark to split.
    tree.append(add(f'{IGN}/Trailing Split', 'Trailing Split', T.page_ign_trailing(),
                    cond='[#engine.cycle_type] == 2'))

    ign_node = {'name': 'Ignition Tuning', 'expanded': True, 'children': tree}
    pages[IGN] = T.page_ign_top()
    return ign_node, pages


def validate(meta, pages, node_paths):
    tel = meta['telemetry']
    channels = set(tel.keys() if isinstance(tel, dict) else [t['id'] for t in tel])
    bad = []
    import re

    # "name[@<expr>]" is an INDEXED binding: the subscript is computed from config at resolve time
    # (firing_order[0].cyl - 1 -> whichever cylinder fires first). Which element it lands on is a runtime
    # question; whether the FIELD exists is not, and that is what this checks — the same trick the studio
    # uses to find a binding's metadata, pinning every computed subscript to element 0.
    def pin_indices(path):
        out, i = '', 0
        while i < len(path):
            if path.startswith('[@', i):
                depth = 0
                while i < len(path):
                    if path[i] == '[': depth += 1
                    elif path[i] == ']':
                        depth -= 1
                        if depth == 0: break
                    i += 1
                out += '[0]'
                i += 1
            else:
                out += path[i]; i += 1
        return out

    def check_ref(page, kind, whole, ref, sigil):
        """One [$channel] or [#config.path] reference — or a bare one, which a binding may also be."""
        ref = pin_indices(ref.strip())
        if sigil == '$':
            if ref not in channels: bad.append((page, kind, whole, 'no such channel'))
        elif '.' in ref:
            if not resolve_config(meta, ref): bad.append((page, kind, whole, 'no such config field'))
        elif ref not in channels:
            bad.append((page, kind, whole, 'no such channel'))

    def check_signal(page, kind, sig):
        """A signalName is either a plain binding or an EXPRESSION over several.

        An indicator says "[$sync_level] == 2" and a value can be arithmetic; treating the whole string as
        one name refused pages that work. Anything carrying a sigil is read as an expression and every
        reference in it is checked; anything else is the plain binding it looks like.
        """
        s = sig.strip()
        refs = re.findall(r"\[([\$#])((?:[^\[\]]|\[[^\]]*\])+)\]", s)
        if refs:
            for sigil, ref in refs:
                check_ref(page, kind, sig, ref, sigil)
        else:
            check_ref(page, kind, sig, s, '')

    def walk(ws, page):
        for w in ws:
            sig = w['props'].get('signalName', '')
            if sig:
                check_signal(page, w['type'], sig)
            for k in ('condition', 'enableCondition'):
                for ref in re.findall(r'\[#((?:[^\[\]]|\[[^\]]*\])+)\]', w['props'].get(k, '')):
                    if not resolve_config(meta, pin_indices(ref)):
                        bad.append((page, w['type'], f'{k}: {ref}', 'no such config field'))
            link = w['props'].get('link', '')
            if link and link not in node_paths:
                bad.append((page, w['type'], f'link: {link}', 'no such tree node'))
            kids = w['props'].get('children')
            if kids: walk(json.loads(kids), page)
    for path, pg in pages.items():
        walk(pg['widgets'], path)
    return bad


def node_paths_of(tree):
    out = set()
    def walk(nodes, prefix=''):
        for n in nodes:
            p = f'{prefix}/{n["name"]}' if prefix else n['name']
            out.add(p)
            walk(n.get('children', []), p)
    walk(tree)
    return out


def main():
    meta = load_meta()
    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']
    cfg = next(n for n in d['tree'] if n['name'] == ROOT)

    # The tables panel on a branch page links to the branch's CHILD NODES, so it has to know what they
    # are called — the schema's labels are only sometimes the same words.
    def _kids(node, pre):
        for c in node.get('children', []):
            path = f'{pre}/{c["name"]}'
            FP.TREE_CHILDREN.setdefault(pre, []).append(c['name'])
            _kids(c, path)
    for _t in d['tree']:
        _kids(_t, _t['name'])

    ign_node, pages = build()
    pages[ROOT] = T.page_root()
    for title, path, groups, blurb in T.CATEGORY_PAGES:
        pages[path] = T.page_category(title, groups, blurb)
    # The fuel branch's own page is the SWITCHBOARD — what the fuel side is made of, what is switched on
    # and what it is doing — not a list of its children, which is what the tree beside it already shows.
    # It was installed under 'Configuration/Fuel', the branch's old name, so nothing could open it.
    pages[f'{ROOT}/Fuel Tuning'] = T.page_fuel_top()
    # THE SENSORS SWITCHBOARD, built from the catalogue rather than by hand. The page that was here listed
    # twenty of the 122 inputs, in one panel, with no links — so the other hundred could only be reached
    # through the tree, and the page could not answer what a switchboard is for: what is this car wired
    # for? Generated, so a sensor added to the schema appears here without anyone remembering to.
    sensors_page, s_linked, s_unlinked = SP.page(d)
    pages[f'{ROOT}/Sensors'] = sensors_page
    print(f'sensors: {s_linked + s_unlinked} inputs on the switchboard '
          f'({s_unlinked} with no page of their own to link to)')
    # A page found through the search must be able to turn its own feature on, so every module page the
    # switchboard points at carries that module's enable — the shape the Throttle Body A page uses.
    for title, mod, path, groups, live, blurb in FP.FEATURES:
        pages[path] = FP.feature_page(title, mod, groups, live, blurb, FP.EXTRA_PANELS.get(mod))

    # …and the pages that belong to a feature's branch without being its own page (an array of fifteen
    # widebands needs a page, not a panel).
    for path, mk in FP.EXTRA_PAGES.items():   # not `build` — that is this module's own builder
        pages[path] = mk()

    # Knock has a family rather than a page: the module, its two decision tables, and the learned noise
    # floor it builds while it listens.
    import knock_pages as KP
    pages.update(KP.pages())

    # Cruise is a family too, and it cannot be a generic feature page: feature_page() emits its live
    # strip only when it has no extra panel, and cruise needs both the strip that says WHY it is not
    # engaging and the links to its three gain curves.
    import cruise_pages as CP
    pages.update(CP.pages())

    # THE TREE LISTS WHAT THE ENGINE IS RUNNING. A feature that is switched off leaves the tree, the way
    # the reference software does it — otherwise every car's tree is the same list of everything the ECU
    # could ever do, and the six things this one actually uses are lost in it.
    #
    # Safe only because the switchboard exists and every page carries its own switch: Configuration lists
    # every function whether on or off, so a disabled feature is always one click away and can be turned
    # on from the page that describes it. Without those two, this would hide settings with no way back.
    # Walk the WHOLE branch: a feature node lives inside its category now, not beside it.
    # INDEXED BY PATH, not by name. A bare name is not unique in this tree — there is a sensor called
    # "Vehicle Speed" under Sensors and a module called "Vehicle Speed" under Vehicle Functions — so a
    # name lookup found whichever came first and gated the wrong node.
    by_path = {}
    def _index(nodes, prefix):
        for n in nodes:
            path = f"{prefix}/{n['name']}"
            by_path[path] = n
            _index(n.get('children', []), path)
    # THE ROOT IS A PARENT TOO. Indexing only its children meant a page directly under Configuration had
    # "nothing in the tree to hang it on" — parent_and_leaf looks for the longest node path the page path
    # starts with, and ROOT itself was not in the index. Lua Scripting is the first page to sit there.
    by_path[ROOT] = cfg
    _index(cfg['children'], ROOT)

    # Malformed nodes from an earlier install, removed BEFORE anything is looked up — their joined
    # path is the one the correctly nested node needs, so leaving them makes ensure_path believe the
    # work is already done and the real nesting is never built.
    dropped = 0
    for parent_path, bad_name in getattr(FP, 'RETIRED_NODE_NAMES', []):
        parent = by_path.get(parent_path)
        if not parent or not parent.get('children'):
            continue
        before = len(parent['children'])
        parent['children'] = [c for c in parent['children'] if c.get('name') != bad_name]
        if len(parent['children']) != before:
            by_path.pop(f'{parent_path}/{bad_name}', None)
            dropped += 1
    if dropped:
        by_path.clear()
        by_path[ROOT] = cfg
        _index(cfg['children'], ROOT)
        print(f'tree: dropped {dropped} malformed node(s)')

    # The parent of a page path, found by the LONGEST existing node path it starts with. Splitting on the
    # last '/' cannot work here: "Water & Methanol" is one node whose name contains a slash, and that is
    # allowed — the tree stores names, and only this function has to care.
    def parent_and_leaf(path):
        best = ''
        for np in by_path:
            if path.startswith(np + '/') and len(np) > len(best):
                best = np
        return (by_path.get(best), path[len(best) + 1:]) if best else (None, path)

    def ensure_path(path):
        """The node at `path`, creating EVERY missing level on the way.

        parent_and_leaf finds the longest ancestor that exists and hands back the whole remainder as
        one leaf name — so a path two levels deeper than anything installed became a single node
        literally called "CAN1/Transmit", sitting flat under its grandparent with a slash in its
        name. It looked nested in any listing that joins names, and was not.
        """
        if path in by_path:
            return by_path[path], 0
        parts = path.split('/')
        made = 0
        cur = ''
        node = None
        for seg in parts:
            nxt = f'{cur}/{seg}' if cur else seg
            if nxt not in by_path:
                parent = by_path.get(cur)
                if parent is None:
                    return None, made               # nothing to hang it off
                parent.setdefault('children', []).append({'name': seg, 'expanded': False})
                by_path[nxt] = parent['children'][-1]
                made += 1
            node = by_path[nxt]
            cur = nxt
        return node, made
    # A feature the tree has never heard of gets its node made, under the category its path names. The
    # gating below only ever ADJUSTED nodes that already existed, so adding a feature installed its page
    # and its link and left both pointing at nothing — which is exactly what the link check then refused.
    created = 0
    for title, mod, path, groups, live, blurb in FP.FEATURES:
        if path in by_path:
            continue
        parent, leaf = parent_and_leaf(path)
        if parent is None:
            print(f'  ! {path}: nothing in the tree to hang it on — skipped')
            continue
        node = {'name': leaf, 'expanded': False}
        parent.setdefault('children', []).append(node)
        by_path[path] = node
        created += 1
    # …and the same for a page family: knock's three children hang off the Knock Control node, which is
    # itself adopted from the seeded tree. A page keyed at a path with no node is a page with no way in.
    for path in sorted(pages):
        if path in by_path:
            continue
        _, made = ensure_path(path)
        created += made
    if created:
        print(f'tree: {created} new node(s) created')

    # …and put a branch's children in the order it declares, for the branches where alphabetical is
    # not what the page says. Unlisted children keep their relative order behind the listed ones.
    for parent_path, order in getattr(FP, 'CHILD_ORDER', {}).items():
        parent = by_path.get(parent_path)
        kids = (parent or {}).get('children')
        if not kids:
            continue
        rank = {name: i for i, name in enumerate(order)}
        parent['children'] = sorted(kids, key=lambda c: (rank.get(c.get('name'), len(rank)),))

    gated = 0
    for title, mod, path, groups, live, blurb in FP.FEATURES:
        node = by_path.get(path)
        if node is None:
            continue
        # A module's switch is `enabled`, and every feature follows that. What this DOES still guard
        # against is a module that has lost one: the old code tested for the field and, finding none,
        # simply skipped — leaving whatever condition was there before, which by then named nothing.
        if 'enabled' in meta['config'].get(mod, {}):
            node['condition'] = f'{mod}.enabled'
            gated += 1
        elif node.get('condition'):
            del node['condition']          # named a field that is gone — an ungated node beats a dead gate
            print(f'tree: cleared a condition naming nothing on {path}')
    # Nodes this installer used to make and no longer does. Removed by PATH, from the declared list —
    # see FP.RETIRED_PAGES for why it is a list rather than a rule.
    pruned = 0
    for path in FP.RETIRED_PAGES:
        # The PAGE and the NODE are pruned INDEPENDENTLY. Doing it only when the node is still there
        # leaves the page body behind on the second run — a page with no node, which is invisible and
        # still in the file, and exactly what check_doc calls it.
        if pages.pop(path, None) is not None or lib.pop(path, None) is not None:
            pruned += 1
        node = by_path.get(path)
        parent = by_path.get(path.rsplit('/', 1)[0]) if node is not None else None
        if node is not None and parent is not None and parent.get('children') is not None:
            parent['children'] = [c for c in parent['children'] if c is not node]
            pruned += 1
    if pruned:
        print(f'tree: pruned {pruned} retired node(s)')

    # …and the pages that are not a feature's own. They have no module to derive a switch from, so
    # feature_pages declares each one's condition; a page listed in EXTRA_PAGES without one is a
    # KeyError here rather than a node that quietly shows for ever, which is how the Generic CAN
    # pages shipped ungated.
    for path in FP.EXTRA_PAGES:
        node = by_path.get(path)
        if node is None:
            continue
        cond = FP.EXTRA_PAGE_CONDS[path]          # deliberately not .get(): an absent one is a bug
        if cond:
            node['condition'] = cond
            gated += 1
        elif node.get('condition'):
            del node['condition']
    print(f'tree: {gated} nodes now appear only when what they configure is on')

    # Ignition's branch replaces the seeded one (23 flat table nodes, machine-named).
    old = f'{ROOT}/Ignition Tuning'
    dropped = [k for k in lib if k == old or k.startswith(old + '/')]
    existing = next((c for c in cfg['children'] if c['name'] in ('Ignition', 'Ignition Tuning')), None)
    if existing:                                    # keep what was moved in (Knock Control) and its pages
        mine = {c['name'] for c in ign_node['children']}
        adopted = [c for c in existing.get('children', []) if c['name'] not in mine]
        if adopted:
            ign_node['children'] = adopted + ign_node['children']
            kept = {f"{old}/{c['name']}" for c in adopted}
            dropped = [k for k in dropped if not any(k == p or k.startswith(p + '/') for p in kept)]
            print('adopted: ' + ', '.join(c['name'] for c in adopted))
    cfg['children'] = [c for c in cfg['children'] if c['name'] not in ('Ignition', 'Ignition Tuning')]
    fuel_at = next((i for i, c in enumerate(cfg['children']) if c['name'] == 'Fuel Tuning'), len(cfg['children']))
    cfg['children'].insert(fuel_at + 1, ign_node)

    pages = {k: (v if isinstance(v, dict) else v.to_json()) for k, v in
             ((k, v.to_json() if hasattr(v, 'to_json') else v) for k, v in pages.items())}

    bad = validate(meta, pages, node_paths_of(d['tree']))
    if bad:
        print(f'REFUSING TO WRITE — {len(bad)} reference(s) name nothing:')
        for p, t, s, why in bad[:40]:
            print(f'   {p:<44} {t:<11} {s:<58} {why}')
        return 1

    for k in dropped: del lib[k]
    before = len(main_model['widgets'])
    main_model['widgets'] = [w for w in main_model['widgets']
                             if not (w['type'] == 'viewport'
                                     and (w['props'].get('node', '') == old
                                          or w['props'].get('node', '').startswith(old + '/')))]
    for path, pg in pages.items():
        lib[path] = pg

    have = {w['props'].get('node') for w in main_model['widgets'] if w['type'] == 'viewport'}
    # NO VIEWPORT PER PAGE. A viewport is a window onto whichever page the tree has selected, so
    # the one a surface carries reaches all of them (see collapse_viewports.py). Creating one per
    # page here rebuilds the stack a document was migrated out of -- 189 of them, last time these
    # ran after the migration.
    # A PAGE THAT MOVED LEAVES AN ORPHAN. FEATURES names the new path, so the page installs there and the
    # node is created there — and the OLD node stays in the tree with the old page still under it, opening
    # onto a copy that nothing updates again. Both go, and every reference is rewritten, so a document
    # built before the move heals on the next run instead of showing the feature twice.
    retired = []
    for old_path, new_path in MOVED_PAGES.items():
        parent_path, _, leaf = old_path.rpartition('/')
        parent = by_path.get(parent_path)
        if parent and any(c['name'] == leaf for c in parent.get('children') or []):
            parent['children'] = [c for c in parent['children'] if c['name'] != leaf]
            retired.append(f'{old_path} -> {new_path}')
        for k in [k for k in lib if k == old_path or k.startswith(old_path + '/')]:
            del lib[k]

    out = json.dumps(d, indent=2)
    for old_path, new_path in MOVED_PAGES.items():
        out = out.replace(old_path, new_path)      # links and viewport nodes live inside prop strings
    open(DOC, 'w').write(out)
    print(f'installed {len(pages)} pages, dropped {len(dropped)} old ignition page(s)')
    for m in retired:
        print(f'  moved: {m}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
