#!/usr/bin/env python3
"""The meta's FORMAT number, and the check that keeps it honest.

An older studio reads a meta by the structure it was written for. min_studio (firmware/min_studio.txt)
refuses a studio too old for this firmware, but only if someone remembers to raise it — and the change
that needs it (a new key the studio must understand, a new datatype, a new table-axis form) looks like any
other change. So the STRUCTURE is fingerprinted: every key path, with maps of named things (settings,
channels, signals, trouble codes) collapsed to one entry, every leaf's JSON type, and the values of the
keys that name a kind of thing (type, datatype, kind …). Adding a setting leaves the fingerprint alone;
adding a new kind of setting does not.

codegen emits meta.meta_format from codegen/meta_format.lock and refuses to build if the fingerprint has
moved without it. The studio declares the highest format it reads (MetaModel kMetaFormat) and refuses a
newer one with the "a newer studio is needed" message.

    python3 tools/meta_format.py check  [shared/tuneit-meta.json]   # what codegen runs; lists what moved
    python3 tools/meta_format.py bless  [shared/tuneit-meta.json]   # after an intended change: format + 1

bless insists that firmware/min_studio.txt has been raised first, to the first studio that reads the new
format — raising the format without it would let every older studio try.
"""
import json, pathlib, sys

REPO = pathlib.Path(__file__).resolve().parent.parent
LOCK = REPO / 'codegen' / 'meta_format.lock'
MIN_STUDIO = REPO / 'firmware' / 'min_studio.txt'

# Keys whose VALUES are part of the format: a new value is a new kind of thing an older studio does not
# know how to read.
KIND_KEYS = {'type', 'datatype', 'kind', 'field_type', 'role', 'encoding', 'format'}
# Written by this tool into the meta, so not part of what it fingerprints.
SELF = {('meta', 'meta_format')}


def _is_map(d):
    """A dict of NAMED THINGS (settings, channels, codes) rather than a record with fixed fields: all its
    values are containers of one kind, or it is a long run of scalars of one type."""
    vals = list(d.values())
    if not vals:
        return False
    kinds = {type(v) for v in vals}
    if len(kinds) != 1:
        return False
    k = kinds.pop()
    return k in (dict, list) or len(vals) >= 8


def entries(doc):
    out = set()

    def walk(v, path):
        if isinstance(v, dict):
            if _is_map(v):
                for x in v.values():
                    walk(x, path + ('*',))
            else:
                for key, x in v.items():
                    if (path + (key,))[:2] in SELF and len(path) == 1:
                        continue
                    if key in KIND_KEYS and isinstance(x, str):
                        out.add('.'.join(path + (key,)) + '=' + x)
                    walk(x, path + (key,))
        elif isinstance(v, list):
            for x in v:
                walk(x, path + ('[]',))
        else:
            # A number is a number to the studio: 3 and 3.5 are one type, or a new setting with a decimal
            # default would read as a change of format.
            t = 'null' if v is None else 'bool' if isinstance(v, bool) else 'num' if isinstance(v, (int, float)) \
                else type(v).__name__
            out.add('.'.join(path) + ':' + t)

    walk(doc, ())
    return sorted(out)


def load_meta(path):
    raw = pathlib.Path(path).read_bytes()
    raw = raw[:raw.rfind(b'}') + 1]            # the file carries a CRC footer after the JSON
    return json.loads(raw.decode('utf-8'))


def read_lock():
    return json.loads(LOCK.read_text()) if LOCK.exists() else {'format': 0, 'min_studio': '0.0.0', 'entries': []}


def check(doc):
    """Returns (format, None) when the structure matches the lock, else (format, message)."""
    lock = read_lock()
    have, want = set(entries(doc)), set(lock['entries'])
    if have == want:
        return lock['format'], None
    added, gone = sorted(have - want), sorted(want - have)
    lines = ['The meta\'s STRUCTURE changed, and an older studio may misread it:']
    lines += ['  + ' + e for e in added[:40]] + (['  … %d more added' % (len(added) - 40)] if len(added) > 40 else [])
    lines += ['  - ' + e for e in gone[:40]] + (['  … %d more gone' % (len(gone) - 40)] if len(gone) > 40 else [])
    lines += ['',
              'If an older studio reads the new structure correctly (it only ignores what it does not know),',
              'bless it as the same format:   python3 tools/meta_format.py bless --same',
              'Otherwise raise firmware/min_studio.txt to the first studio that reads it, teach the studio',
              '(MetaModel kMetaFormat), and bless a new format:   python3 tools/meta_format.py bless']
    return lock['format'], '\n'.join(lines)


def _ver(s):
    return tuple(int(x) for x in s.strip().split('.'))


def bless(doc, same):
    lock = read_lock()
    min_studio = MIN_STUDIO.read_text().strip() if MIN_STUDIO.exists() else '0.1.0'
    fmt = lock['format'] if same else lock['format'] + 1
    if not same and lock['entries'] and _ver(min_studio) <= _ver(lock['min_studio']):
        sys.exit('meta_format: a new format needs firmware/min_studio.txt raised above %s first '
                 '(it is %s) — otherwise every older studio would still try to read it.'
                 % (lock['min_studio'], min_studio))
    LOCK.write_text(json.dumps({'format': max(fmt, 1), 'min_studio': min_studio if not same else lock['min_studio'] or min_studio,
                                'entries': entries(doc)}, indent=1) + '\n')
    print('meta_format: format %d blessed (%d structure entries, min_studio %s)'
          % (max(fmt, 1), len(entries(doc)), min_studio))


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args or args[0] not in ('check', 'bless'):
        sys.exit(__doc__)
    doc = load_meta(args[1] if len(args) > 1 else REPO / 'shared' / 'tuneit-meta.json')
    if args[0] == 'check':
        fmt, msg = check(doc)
        print(msg or 'meta_format: structure matches format %d' % fmt)
        sys.exit(1 if msg else 0)
    bless(doc, same='--same' in sys.argv)
