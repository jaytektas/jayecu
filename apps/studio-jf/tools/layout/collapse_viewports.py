#!/usr/bin/env python3
"""Collapse each surface's per-node viewport stack to ONE unbound viewport.

A page is authored once and lives in the library, keyed by its tree path. A viewport is a window onto
whichever page the tree has selected — so one per surface is enough, and every surface shows the same
page at the same time.

That is not how the document grew. Every viewport carried a `node`, Surface::nodeHidden_ hid it unless
that node was the selection, and so a surface needed one viewport PER PAGE: 526 stacked on Main, 512 on
each of the other two, all at the same 0,92 1280x700 box. The cost was not the bloat — it was that a
page whose viewport had never been added to a given surface was UNREACHABLE there. Fifteen pages opened
on Main and nowhere else.

This removes the stack and leaves one viewport with no `node`, which the studio now reads as "show the
selection". A viewport that carries an explicit `page` is a pasted private copy and is left alone.
"""
import json, shutil, sys, pathlib

from apply_fuel import DOC   # one definition of where the shipped layout lives

def main(path=DOC):
    p = pathlib.Path(path)
    shutil.copy(p, str(p) + '.bak-before-viewport-collapse')
    d = json.loads(p.read_text())

    total_removed = 0
    for surf in d['surfaces']['pool']:
        m = surf['model']
        ws = m.get('widgets') or []
        vps = [w for w in ws if w['type'] == 'viewport' and not (w.get('props') or {}).get('page')]
        if not vps:
            continue
        # They are all the same box; keep the first as the survivor and strip its binding.
        # STRIP EVERY TRACE OF THE NODE IT USED TO BE. Keeping the first viewport wholesale keeps its
        # caption too, so all three surfaces end up titled after whichever node happened to be first
        # while showing whatever the tree has selected. A viewport with no caption of its own falls
        # back to the page's title, which is the one that is true.
        DROP = ('node', 'title', 'labelText')
        keep = dict(vps[0])
        keep['props'] = {k: v for k, v in (keep.get('props') or {}).items() if k not in DROP}
        others = [w for w in ws if w not in vps]
        m['widgets'] = others + [keep]
        total_removed += len(vps) - 1
        print(f"  {surf['name']:16s} {len(vps):4d} viewports -> 1   "
              f"({len(others)} other widgets kept)")

    p.write_text(json.dumps(d, indent=2))
    print(f"removed {total_removed} redundant viewports")

if __name__ == '__main__':
    main(*sys.argv[1:])
