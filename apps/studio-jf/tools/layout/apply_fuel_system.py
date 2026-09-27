"""Gather the fuel HARDWARE pages under Engine Configuration/Fuel System.

Fuel Setup describes the injectors themselves — how many and what they flow.
That is the same kind of thing Ignition System holds for the coils, and
it is not tuning: it does not change with a map, it changes when the engine is built. They sat under
Fuel Tuning next to the VE table, which is where you go to change fuelling, not to find out what is
bolted on.

So it moves to Fuel System, matching Ignition System on the other side.

RUN THIS LAST. apply_fuel owns the Fuel Tuning branch and rebuilds it wholesale, so a re-run puts Fuel
Setup back where it was; this pass moves it out again. That is why it is a mover rather than an edit
to apply_fuel: the two branches have different owners, and a page that must end up in one while being
built by the other needs somebody to carry it across every time.

A move is three things, and missing any one leaves a page that opens blank:
  * the tree node,
  * the panelLibrary entry, which is keyed by the node's PATH,
  * every `link` prop that names the old path — five of them pointed at Fuel Setup.

    python3 tools/layout/apply_fuel_system.py
"""
import json, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_fuel import DOC, ROOT

HOME = f'{ROOT}/Engine Configuration/Fuel System'
MOVE = ['Fuel Setup']                             # in the order they should appear under it
FROM = f'{ROOT}/Fuel Tuning'


def main():
    raw = open(DOC, encoding='utf-8', errors='replace').read()
    d = json.loads(raw)
    lib = d['panelLibrary']

    cfg = next(n for n in d['tree'] if n['name'] == ROOT)

    def child(node, name):
        for c in node.get('children') or []:
            if c['name'] == name:
                return c
        return None

    eng = child(cfg, 'Engine Configuration')
    fsys = child(eng, 'Fuel System') if eng else None
    fuel = child(cfg, 'Fuel Tuning')
    if fsys is None or fuel is None:
        print('need both Engine Configuration/Fuel System and Fuel Tuning'); return 1

    kids = fsys.setdefault('children', [])
    moved, relinked = [], 0
    for name in MOVE:
        node = child(fuel, name)
        here = any(c['name'] == name for c in kids)
        if node is None and not here:
            print(f'  {name}: not under Fuel Tuning and not under Fuel System — skipped')
            continue
        # THE PAGE MOVES EVERY RUN, EVEN WHEN THE NODE ALREADY HAS. This used to `continue` the moment the
        # node was found under Fuel System — "already here, nothing to do" — which was true of the TREE and
        # false of the LIBRARY: apply_fuel rebuilds the page at the OLD key every run, and leaving it there
        # meant the destination kept whatever was installed the day the node first moved. The blanket path
        # rewrite below then renamed the fresh page's key onto the stale one's, and the stale one won.
        # Fuel Setup had been frozen since that day: it did not follow a single layout change.
        if node is not None:
            fuel['children'] = [c for c in fuel['children'] if c is not node]
            if not here:
                kids.append(node)
        # The page travels with the node, and so does every page BELOW it: the library is keyed by path,
        # so a child page keeps pointing at a node that no longer exists unless its key moves too.
        old, new = f'{FROM}/{name}', f'{HOME}/{name}'
        for key in [k for k in lib if k == old or k.startswith(old + '/')]:
            lib[new + key[len(old):]] = lib.pop(key)
        moved.append(f'{name}: {old} -> {new}')

    # Every reference to a moved path, wherever it is — a label's `link`, a viewport's `node`. Done on
    # the SERIALISED document because those live inside props and inside the children-as-a-string blobs,
    # and a structural walk would have to know every place a path can hide.
    out = json.dumps(d, indent=2)
    for name in MOVE:
        old, new = f'{FROM}/{name}', f'{HOME}/{name}'
        relinked += out.count(old)
        out = out.replace(old, new)
    open(DOC, 'w').write(out)

    for m in moved:
        print(f'  {m}')
    print(f'fuel system: {len(moved)} page(s) moved, {relinked} reference(s) rewritten')
    return 0


if __name__ == '__main__':
    sys.exit(main())
