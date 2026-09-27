"""Page notes, and the paragraphs beside panels, wrap INSIDE the visible page instead of running off it.

A page's note (the grey line under its title bar, author.Page.head) was given the whole page width:
1134 of the 1280-wide page. The page is drawn at the interface scale, so a note that full ends beyond
the visible area and its last words run off the right-hand side. A note now ends at NOTE_RIGHT at most
(author.NOTE_RIGHT, the same limit head() applies to new pages) and wraps within it — shorter lines are
easier to read anyway.

The same holds for any other wrapped paragraph placed on the page itself (the explanation columns beside
the panels, author.Page.wrapped): it ends at TEXT_RIGHT at most.

A paragraph that wraps to more lines than before gets the height it needs (the ruler that breaks text
exactly as LabelWidget does), and what sits below it moves down by the same amount, with the page's own
height, so nothing ends up under the text: for a note, everything below it; for a side paragraph, what
lies beneath it in the columns it spans.

    python3 apply_note_wrap.py            # report
    python3 apply_note_wrap.py --write    # apply to definition/boards/jaytek_v1.dashboard.gui

Writes the document back in the studio's own compact form (no indent, UTF-8 as-is), so the diff is only
the notes that changed.
"""
import json, sys
import author as A
from paths import DOC

NOTE_Y = A.BAND + 2         # where head() puts a note


def is_note(e):
    p = e.get('props', {})
    return (e.get('type') == 'label' and e.get('groupId', 0) == 0 and e.get('y') == NOTE_Y
            and p.get('wrap') == '1' and p.get('labelText'))


def font_px(e):
    try:
        return int(e.get('props', {}).get('fontName', '').split('|')[1])
    except Exception:
        return 13


def rewrap(page, e, right_limit, is_page_note):
    """Cap one label at right_limit and make room for its new height. Returns a report tuple or None."""
    widgets = page.get('widgets') or []
    right = e['x'] + e['w']
    if right <= right_limit:
        return None
    old_h, x0 = e['h'], e['x']
    new_w = right_limit - x0
    text = e['props']['labelText']
    need = A.Page.note_h(text, new_w) if is_page_note else A.Page.wrap_h(text, new_w, font_px(e))
    new_h = max(old_h, need)
    delta = new_h - old_h
    bottom = e['y'] + old_h
    e['w'], e['h'] = new_w, new_h
    if delta > 0:
        for o in widgets:
            if o is e or o.get('y', 0) < bottom - 2:
                continue
            # A note spans the page, so everything below it moves. A side paragraph only pushes what is
            # beneath it — anything overlapping its original columns.
            if is_page_note or (o['x'] < right and o['x'] + o.get('w', 0) > x0):
                o['y'] += delta
        for key in ('canvasHeight', 'minHeight'):
            if isinstance(page.get(key), (int, float)) and page[key] > 0:
                page[key] += delta
    return (right, new_w, old_h, new_h, delta)


def fix_page(page):
    out = []
    for e in list(page.get('widgets') or []):
        p = e.get('props', {})
        if e.get('type') != 'label' or e.get('groupId', 0) != 0 or p.get('wrap') != '1' or not p.get('labelText'):
            continue
        if is_note(e):
            r = rewrap(page, e, A.NOTE_RIGHT, True)
        else:
            r = rewrap(page, e, A.TEXT_RIGHT, False)
        if r:
            out.append(r)
    return out


def main():
    write = '--write' in sys.argv
    raw = open(DOC, encoding='utf-8').read()
    doc = json.loads(raw)
    changed = 0
    for node, page in (doc.get('panelLibrary') or {}).items():
        for right, w, oh, nh, d in fix_page(page):
            changed += 1
            print(f'  {node}: paragraph ended at {right}, now {w} wide; height {oh} -> {nh}' + (f', content down {d}' if d else ''))
    print(f'{changed} paragraph(s) re-wrapped')
    if write and changed:
        open(DOC, 'w', encoding='utf-8').write(json.dumps(doc, separators=(',', ':'), ensure_ascii=False))


if __name__ == '__main__':
    main()
