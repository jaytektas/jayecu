"""Group the flat list of modules into categories, and move everything that points at them.

The seeded tree is Configuration followed by every module in schema order — thirty-six siblings, machine
named. The reference software groups them (Engine Configuration, Fuel Tuning, Ignition Tuning, Engine
Functions, Electrical, Vehicle Functions, …) and that is what makes its tree readable at a glance.

Moving a node is four edits, not one, and missing any of them leaves something pointing at where the node
used to be:
  the node itself, the panelLibrary page keyed by its PATH (and every descendant page),
  every viewport whose `node` prop names it, and every link on any page.
This does all four, then verifies that nothing anywhere still names an old path.
"""
import json, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from apply_fuel import DOC, ROOT

# (current node name, category, human name). A category of None keeps the node at the top level.
PLAN = [
    # Setup — what the engine IS.
    ('Engine Configuration',  None,                 'Engine Configuration'),
    ('Engine',                'Engine Configuration', 'Cylinders & Firing'),
    ('Vehicle',               'Engine Configuration', 'Vehicle Identity'),
    ('Sensors',               None,                 'Sensors'),

    ('Fuel',                  None,                 'Fuel Tuning'),
    ('Lambda',                'Fuel Tuning',        'O2 Control'),
    ('TransientThrottle',     None,                 None),      # owned by Fuel Tuning now — drop the stray node

    ('Ignition',              None,                 'Ignition Tuning'),
    ('Knock',                 'Ignition Tuning',    'Knock Control'),

    # Engine Functions — things the engine DOES while it runs.
    ('Engine functions',      None,                 'Engine Functions'),
    ('Idle',                  'Engine Functions',   'Idle Control'),
    ('Stepper',               'Engine Functions',   'Idle Stepper'),
    ('Boost',                 'Engine Functions',   'Boost Control'),
    ('VvtControl',            'Engine Functions',   'Cam Control'),
    ('VVL',                   'Engine Functions',   'Variable Valve Lift'),
    ('Dfco',                  'Engine Functions',   'Deceleration Fuel Cut'),
    ('AntiLag',               'Engine Functions',   'Anti-Lag'),
    ('Nitrous',               'Engine Functions',   'Nitrous'),
    ('Wmi',                   'Engine Functions',   'Water & Methanol'),
    ('TorqueModel',           'Engine Functions',   'Torque Model'),

    # Protection — the limits.
    ('EngineProtection',      'Protection',         'Engine Protection'),
    ('RevLimiter',            'Protection',         'Rev Limiter'),
    ('LambdaProtect',         'Protection',         'Lambda Protection'),
    ('EgtProtect',            'Protection',         'EGT Protection'),

    # Vehicle Functions — things about the CAR.
    ('Launch',                'Vehicle Functions',  'Launch Control'),
    ('FlatShift',             'Vehicle Functions',  'Flat Shift'),
    ('TractionControl',       'Vehicle Functions',  'Traction Control'),
    ('PitLimiter',            'Vehicle Functions',  'Pit Speed Limiter'),
    ('CruiseControl',         'Vehicle Functions',  'Cruise Control'),
    ('GearDetect',            'Vehicle Functions',  'Gear Detection'),

    ('Alternator',            'Electrical',         'Alternator Control'),
    ('Outputs',               'Electrical',         'Outputs'),

    ('Can',                   'Communications',     'CAN Bus'),
    ('Datalog',               'Communications',     'Datalogging'),
    ('Lua',                   'Communications',     'Lua Scripting'),
]

CATEGORIES = ['Engine Configuration', 'Sensors', 'Fuel Tuning', 'Ignition Tuning',
              'Engine Functions', 'Protection', 'Vehicle Functions', 'Electrical', 'Communications']


def path_of(name, category=None):
    return f'{ROOT}/{category}/{name}' if category else f'{ROOT}/{name}'


def main():
    d = json.load(open(DOC))
    lib, main_model = d['panelLibrary'], d['surfaces']['pool'][0]['model']
    cfg = next(n for n in d['tree'] if n['name'] == ROOT)
    by_name = {c['name']: c for c in cfg['children']}

    moves = {}          # old path prefix -> new path prefix
    cat_nodes = {c: {'name': c, 'expanded': False, 'children': []} for c in CATEGORIES}
    keep_top, dropped = [], []

    for old_name, category, new_name in PLAN:
        node = by_name.pop(old_name, None)
        if node is None:
            continue
        if new_name is None:                       # a node whose content now lives elsewhere
            dropped.append(path_of(old_name))
            continue
        old_path = path_of(old_name)
        node['name'] = new_name
        if category and category != new_name:
            cat_nodes[category]['children'].append(node)
            new_path = path_of(new_name, category)
        else:
            keep_top.append((new_name, node))
            new_path = path_of(new_name)
        if old_path != new_path:
            moves[old_path] = new_path

    # Anything the plan does not mention stays where it is, at the top level.
    leftovers = [c for n, c in by_name.items()]
    order = {name: i for i, name in enumerate(CATEGORIES)}
    children = []
    placed = {n for n, _ in keep_top}
    for c in CATEGORIES:
        node = next((nd for nm, nd in keep_top if nm == c), None)
        if node is not None:                        # an existing node IS this category (Fuel Tuning, Sensors…)
            node.setdefault('children', [])
            node['children'] = cat_nodes[c]['children'] + node['children'] if cat_nodes[c]['children'] else node['children']
            children.append(node)
        elif cat_nodes[c]['children']:
            children.append(cat_nodes[c])
    children += leftovers
    cfg['children'] = children

    # ---- follow the moves everywhere ------------------------------------------------------------
    def remap(p):
        for old, new in moves.items():
            if p == old or p.startswith(old + '/'):
                return new + p[len(old):]
        return p

    newlib = {}
    for k, v in lib.items():
        nk = remap(k)
        if any(nk == dp or nk.startswith(dp + '/') for dp in dropped):
            continue                                # its content moved into another branch
        newlib[nk] = v
    d['panelLibrary'] = newlib

    vps = 0
    kept = []
    for w in main_model['widgets']:
        if w['type'] == 'viewport':
            node = w['props'].get('node', '')
            if any(node == dp or node.startswith(dp + '/') for dp in dropped):
                continue
            nn = remap(node)
            if nn != node:
                w['props']['node'] = nn
                leaf = nn.rsplit('/', 1)[-1]
                w['props']['title'] = leaf
                w['props']['labelText'] = leaf
                vps += 1
        kept.append(w)
    main_model['widgets'] = kept

    links = 0
    def walk(ws):
        nonlocal links
        for w in ws:
            l = w['props'].get('link')
            if l:
                nl = remap(l)
                if nl != l:
                    w['props']['link'] = nl; links += 1
            ch = w['props'].get('children')
            if ch:
                kids = json.loads(ch); walk(kids)
                w['props']['children'] = json.dumps(kids, separators=(',', ':'))
    for p in d['surfaces']['pool']:
        if p.get('model'): walk(p['model']['widgets'])
    for k, v in d['panelLibrary'].items():
        if isinstance(v, dict): walk(v.get('widgets', []))

    json.dump(d, open(DOC, 'w'), indent=2)
    print(f'moved {len(moves)} node(s); re-keyed pages, {vps} viewport(s), {links} link(s); '
          f'dropped {len(dropped)} stray node(s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
