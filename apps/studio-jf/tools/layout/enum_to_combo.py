"""A plain enum is a DROPDOWN — swap the picker for a combo box wherever the definition allows.

The enum picker is a searchable chooser built for the fields that need one: a signal selector offers 328
channels, a pin picker's list changes with the interface. Pointed at a three-option field like Engine
Cycle it is the wrong instrument — a "…" button where a dropdown arrow belongs, and two clicks for a job
that should take one.

Which is which comes from the DEFINITION (author.Page.enum_control), not from a list kept here: a field
that grows a picker later stops being a dropdown by itself. This sweeps the pages that already exist;
newly generated ones get it from A.field.

Only ever picker → combo, and only where the meta says the field is a plain enum, so a second run does
nothing and no field that needs its picker loses one.
"""
import json, sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from apply_fuel import DOC


def main():
    d = json.load(open(DOC))
    swapped, kept = 0, collections.Counter()

    def walk(widgets):
        nonlocal swapped
        for w in widgets:
            if w['type'] == 'enum':
                bind = w['props'].get('signalName', '')
                if bind and A.Page.enum_control(bind) == 'combobox':
                    w['type'] = 'combobox'
                    swapped += 1
                else:
                    kept[bind or '(unbound)'] += 1
            kids = w['props'].get('children')
            if kids:
                inner = json.loads(kids)
                walk(inner)
                w['props']['children'] = json.dumps(inner)

    for page in d['panelLibrary'].values():
        walk(page['widgets'])
    for s in d['surfaces']['pool']:
        walk(s['model']['widgets'])

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'enums: {swapped} now combo boxes; {sum(kept.values())} kept their picker')
    for b, n in kept.most_common(8):
        print(f'   {n:4d}  {b}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
