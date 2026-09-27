"""Every live readout prints to its CHANNEL's precision, not to a number somebody typed.

The telemetry descriptor decides `digits` beside the scale it transmits at: a frame counter is an
integer, a cam angle arrives in tenths of a degree, a lambda ratio in thousandths. Hand-written
formats claimed precision the wire never carried: "%.2f" on a 46-million-frame counter, three decimals
on an angle transmitted in tenths. The descriptor is a CEILING here, not an equals: a page that shows
coolant as a whole number chose that, and rounding a reading for a big dashboard number is a display
decision — inventing digits is not. So this only ever REMOVES decimals.

Two shapes cover every page: the ('Label', 'channel', '%.Nf') triples the builders carry, and a
'format' property sitting beside its own 'signalName'.
"""
import glob, re, sys
import author as A

SKIP = {'fmt_sweep.py', 'author.py'}
TRIPLE = re.compile(r"""\('([^']*)',\s*'([a-z0-9_]+)',\s*'%\.(\d)f'\)""")
PROP   = re.compile(r"""'signalName':\s*'([a-z0-9_]+)'((?:(?!signalName).){0,200}?)'format':\s*'%\.(\d)f'""", re.S)

def known(ch):
    t = A.Page._meta().get('telemetry') or {}
    if not isinstance(t, dict): t = {x['id']: x for x in t}
    return ch in t

def sweep(write):
    hits = 0
    for f in sorted(glob.glob('*.py')):
        if f in SKIP: continue
        s = orig = open(f).read()
        def fix_triple(m):
            global hits
            lbl, ch, n = m.group(1), m.group(2), int(m.group(3))
            if not known(ch): return m.group(0)
            d = A.Page.chan_digits(ch)
            if n <= d: return m.group(0)
            print(f'  {f}: {ch:24} %.{n}f -> %.{d}f   ({lbl})')
            return f"('{lbl}', '{ch}', '%.{d}f')"
        def fix_prop(m):
            ch, mid, n = m.group(1), m.group(2), int(m.group(3))
            if not known(ch): return m.group(0)
            d = A.Page.chan_digits(ch)
            if n <= d: return m.group(0)
            print(f'  {f}: {ch:24} %.{n}f -> %.{d}f   (property)')
            return f"'signalName': '{ch}'{mid}'format': '%.{d}f'"
        s = TRIPLE.sub(fix_triple, s)
        s = PROP.sub(fix_prop, s)
        if s != orig:
            hits += 1
            if write: open(f, 'w').write(s)
    return hits

if __name__ == '__main__':
    w = '--write' in sys.argv
    n = sweep(w)
    print(f'{n} file(s) {"rewritten" if w else "would change"}')
