#!/usr/bin/env python3
"""Give each workspace tab the page it is named for.

A tab carries its own page, so "Idle Control" ought to open on idle control rather than on whatever the
tree happened to restore. `at` on a pool entry is where a tab was last left; seeding it is the same
thing as having left it there — and since a tab remembers where you move it, these really are defaults.

Only tabs that HOST a viewport get one: a tab with none (Diagnostics is instruments only) has nowhere to
show a page, and a setting with no effect is worse than none.
"""
import json, shutil, sys, pathlib

from apply_fuel import DOC   # one definition of where the shipped layout lives

# The page you actually want in front of you on that tab — the TARGET table, not the settings branch.
# A workspace named for a control loop opens on the thing you tune in it.
DEFAULTS = {
    'Idle Control':  'Configuration/Engine Functions/Idle Control/Target RPM',
    'Boost Control': 'Configuration/Engine Functions/Boost Control/Boost Target',
    'Main':          'Configuration',
}

def main(path=DOC):
    p = pathlib.Path(path)
    shutil.copy(p, str(p) + '.bak-before-tab-defaults')
    d = json.loads(p.read_text())
    lib = d['panelLibrary']

    for entry in d['surfaces'].get('pool', []):
        want = DEFAULTS.get(entry.get('name'))
        if not want:
            continue
        model = entry.get('model') or {}
        if not any(w['type'] == 'viewport' for w in (model.get('widgets') or [])):
            print(f"  {entry['name']:16s} no viewport — skipped")
            continue
        if want not in lib:
            print(f"  {entry['name']:16s} SKIPPED, no such page: {want}")
            continue
        entry['at'] = want
        print(f"  {entry['name']:16s} -> {want}")

    p.write_text(json.dumps(d, indent=2))
    print('done')

if __name__ == '__main__':
    main(*sys.argv[1:])
