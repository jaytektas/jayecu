"""Remove the Coil Wiring page — which coil fires which cylinder is now each IGN pin's own page.

The page used to be a read-only table of the firmware's coil convention. The firmware no longer has one:
outputs.output[i] IS pin i, a coil row names the cylinder it fires, and the studio lays the rows out when
the cylinder count or ignition mode changes (src/model/EngineOutputLayout). A table re-deriving a
convention would now disagree with the rows the moment one is edited by hand, so it goes; the rows are
seen and changed on Electrical/Outputs/IGN1..12.

Kept as an installer, in its place in the order, so a document that still carries the page loses it.

    python3 tools/layout/apply_ignition_wiring.py
"""
import json, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_fuel import DOC, ROOT

PAGE = f'{ROOT}/Engine Configuration/Ignition System/Coil Wiring'


def main():
    d = json.load(open(DOC, encoding='utf-8', errors='replace'))
    cfg = next(n for n in d['tree'] if n['name'] == ROOT)

    def child(node, name):
        for c in node.get('children') or []:
            if c['name'] == name: return c
        return None

    eng = child(cfg, 'Engine Configuration')
    ign = child(eng, 'Ignition System') if eng else None
    removed = 0
    if ign is not None:
        before = len(ign.get('children') or [])
        ign['children'] = [c for c in ign.get('children') or [] if c['name'] != 'Coil Wiring']
        removed = before - len(ign['children'])
    had = d['panelLibrary'].pop(PAGE, None) is not None
    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'coil wiring: {removed} tree node(s) and {int(had)} page(s) removed — coils are set on each IGN pin\'s page')
    return 0


if __name__ == '__main__':
    sys.exit(main())
