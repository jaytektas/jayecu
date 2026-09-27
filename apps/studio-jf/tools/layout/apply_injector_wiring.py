"""Remove the Injector Wiring page, and grey the count that does nothing.

The page used to be a read-only table of the firmware's injector convention. The firmware no longer has
one: outputs.output[i] IS pin i, an injector row names its cylinder and stage, and the studio lays the
rows out when the stages, a stage's mode or its injector count change (src/model/EngineOutputLayout). A
table re-deriving a convention would disagree with the rows the moment one is edited by hand, so it goes;
the rows are seen and changed on Electrical/Outputs/LS1..22.

`num_outputs` IS STILL ONLY READ FOR A GROUPED STAGE. The studio lays out one injector per cylinder for
Sequential / Semi-Sequential / Sequential-Any-Sync, and `num_outputs` for Multi-Point and Bank, so the
control is greyed wherever it is dead.

    python3 tools/layout/apply_injector_wiring.py
"""
import json, re, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_fuel import DOC, ROOT

PAGE = f'{ROOT}/Engine Configuration/Fuel System/Injector Wiring'
OLD_PAGE = f'{ROOT}/Fuel Tuning/Injector Wiring'


def grey_dead_counts(lib):
    """Grey `num_outputs` wherever the mode makes it dead — the control and the label naming it."""
    touched = 0

    # THE WHOLE BINDING, not a substring of one. A widget that merely MENTIONS num_outputs in an
    # expression — this installer's own readout computes with it — is not a control on that field, and
    # matching loosely greyed 122 of them.
    BOUND = re.compile(r'^\[?#?engine\.inj_stage\[(\d)\]\.num_outputs\]?$')

    def walk(widgets):
        nonlocal touched
        # Which groups hold a dead-in-per-cylinder-mode count, and for which stage.
        gate = {}
        for w in widgets:
            m = BOUND.match((w.get('props') or {}).get('signalName', ''))
            if not m:
                continue
            gate[w.get('groupId') or 0] = m.group(1)
        for w in widgets:
            g = w.get('groupId') or 0
            # groupId 0 is "ungrouped" and shared by everything, so it can only ever gate the control
            # itself — never its neighbours.
            sig = (w.get('props') or {}).get('signalName', '')
            own = bool(BOUND.match(sig))
            # ITS OWN LABEL, AND NOTHING ELSE. Greying "every widget in the group" took 146 widgets:
            # groupIds repeat within a panel's child list, so a group that holds one dead count also
            # holds whatever else happened to be numbered the same. A LABEL is unbound; another
            # control has a binding of its own and is nobody's caption.
            if own or (g and g in gate and not sig):
                s = gate.get(g) or BOUND.match(sig).group(1)
                cond = f'[#engine.inj_stage[{s}].mode] == 2 || [#engine.inj_stage[{s}].mode] == 3'
                pr = dict(w.get('props') or {})
                prev = pr.get('enableCondition', '')
                # IDEMPOTENT. Appending unconditionally compounds the rule on every re-run —
                # ((cond) && (cond)) && (cond) — which still evaluates the same and is unreadable
                # forever after. An installer gets run more than once by definition.
                if cond not in prev:
                    pr['enableCondition'] = f'({prev}) && ({cond})' if prev else cond
                    w['props'] = pr
                    touched += 1        # count what CHANGED, not what was looked at
            kids = (w.get('props') or {}).get('children')
            if kids:
                w['props'] = dict(w['props'])
                w['props']['children'] = json.dumps(walk(json.loads(kids)), separators=(',', ':'))
        return widgets

    for pan in lib.values():
        pan['widgets'] = walk(pan.get('widgets') or [])
    return touched


def main():
    d = json.load(open(DOC, encoding='utf-8', errors='replace'))
    lib = d['panelLibrary']
    cfg = next(n for n in d['tree'] if n['name'] == ROOT)

    removed = 0

    def drop(node):
        nonlocal removed
        kids = node.get('children') or []
        keep = [c for c in kids if c['name'] != 'Injector Wiring']
        removed += len(kids) - len(keep)
        node['children'] = keep
        for c in keep:
            drop(c)

    drop(cfg)
    pages = sum(lib.pop(p, None) is not None for p in (PAGE, OLD_PAGE))
    greyed = grey_dead_counts(lib)

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'injector wiring: {removed} tree node(s) and {pages} page(s) removed — injectors are set on each '
          f'LS pin\'s page; {greyed} widget(s) now grey where num_outputs is not read')
    return 0


if __name__ == '__main__':
    sys.exit(main())
