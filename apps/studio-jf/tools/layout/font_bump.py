"""Raise any page text still below the app's own size — a document sweep, not a rebuild.

Most pages are generated, so a font change in author.py reaches them the next time their installer runs.
The rest are FILLED IN once (apply_more clones a template where a page is missing) or were authored by
hand, and those kept whatever size they were written at — 12px body text inside an app whose own text is
15, which is what made them unreadable.

Only ever RAISES, and only below the floor: run it twice and the second run does nothing. A wrapped label
grows with its text, because the same words at a bigger size need more lines and a label that clips is
how this went unnoticed in the first place — the box is what the checker measures.
"""
import json, re, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_fuel import DOC

RAISE = {10: 14, 11: 14, 12: 14, 13: 15}     # anything else is already at or above the floor


def main():
    d = json.load(open(DOC))
    bumped = grown = 0

    def walk(widgets):
        nonlocal bumped, grown
        for w in widgets:
            f = w['props'].get('fontName', '')
            m = re.match(r'^\|(\d+)\|(\d)\|(\d)$', f)
            if m and int(m.group(1)) in RAISE:
                old = int(m.group(1))
                new = RAISE[old]
                w['props']['fontName'] = f'|{new}|{m.group(2)}|{m.group(3)}'
                bumped += 1
                # A wrapped paragraph needs the room the bigger glyphs take, or the last line goes
                # missing — silently, because nothing measures the TEXT.
                if w['props'].get('wrap') == '1' and w.get('h'):
                    w['h'] = int(round(w['h'] * new / old))
                    grown += 1
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
    print(f'fonts: {bumped} raised to the floor ({grown} wrapped label(s) grown to match)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
