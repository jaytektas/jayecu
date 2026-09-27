"""Audit the INSTALLED document — every page, not just the ones a builder list remembers.

check_all.py builds pages from the installers and measures them, which is the right check to run
before writing. It cannot be the only one: it covers the 98 shapes its own list names, and the
document holds 569. MAP Prediction was clipping a row 28px past its panel's content box and nothing
said so for exactly that reason — the page is authored by a function that is not in the list.

So this reads dashboard.gui and asks the questions that only make sense of the finished thing:

  GEOMETRY   widgets inside the canvas, children inside their panel's CONTENT box (a panel gives its
             children h - titleBar - 4, so a child measured against the panel's own height is measured
             against a box 26px taller than the one it is in).
  BINDINGS   every config path exists in the meta, every [$channel] is a channel, every pc.* is a
             declared host variable. A binding to something that does not exist renders as a raw
             number or an empty control — it looks like a setting.
  LINKS      every link names a tree node that exists.
  REACHABLE  every page has a node and every node has a page. A page no node points at is invisible;
             a node with no page opens onto nothing.
  IDENTITY   uids unique within a page. Across pages they are shared by design — a template body is
             deep-copied per element — and only a collision inside one page can confuse the editor.
  CONTROLS   a tick box is the size a tick box is. JCheckBox draws a box of its rect's height, so an
             off-size rect is off-size ink. Configuration/Sensors is the one exception, at its row pitch.
  UNUSED     host variables nobody references.
  TEXT       the WORDS inside their box, measured with the app's own font (ruler.py). The box fitting
             its panel says nothing about the text fitting the box: a caption is clipped to its widget's
             rect and drawn with no maxWidth, so one too wide loses its tail mid-word and a wrapped one
             too short loses its first and last rows — with no ellipsis and no complaint anywhere, since
             every geometry check passes. All six trigger-stream pages read "…the pattern is right when
             sy" for exactly that reason.

    python3 tools/layout/check_doc.py
"""
import collections, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import author as A
from apply_fuel import DOC, load_meta

PANEL_TITLE, PAD = 22, 4


def _kids(w):
    ch = (w.get('props') or {}).get('children')
    return json.loads(ch) if ch else []


def _flat(ws):
    for w in ws:
        yield w
        yield from _flat(_kids(w))


def config_paths(meta):
    """Every config path the meta declares, with each subscript normalised to [*]."""
    out = set()

    def walk(node, path=''):
        for k, v in (node or {}).items():
            if not isinstance(v, dict):
                continue
            p = f'{path}{k}'
            if 'fields' in v:                                    # array of structs
                for fk, fv in v['fields'].items():
                    out.add(f'{p}[*].{fk}')
                    for bk in (fv.get('bits') or {}):
                        out.add(f'{p}[*].{fk}.{bk}')
                for tk in (v.get('tables') or {}):
                    out.add(f'{p}[*].{tk}')
                for ak in (v.get('arrays') or {}):
                    out.add(f'{p}[*].{ak}[*].v')
            elif 'offset' in v or 'datatype' in v or 'type' in v:
                out.add(p)
                for bk in (v.get('bits') or {}):
                    out.add(f'{p}.{bk}')
            if not ('fields' in v or 'offset' in v):
                walk(v, p + '.')
    walk(meta['config'])
    # A viewport's Data Source is an ELEMENT, not a field — "sensors.sensor[battery]" is what a
    # template page's "[*]" resolves against. Every array is therefore a legal path in its own right.
    for p2 in list(out):
        if '[*].' in p2:
            out.add(p2.split('[*].')[0] + '[*]')
    return out


def main():
    d = json.load(open(DOC, encoding='utf-8', errors='replace'))
    meta = load_meta()
    lib = d['panelLibrary']
    cfg = config_paths(meta)
    tel = set(meta.get('telemetry') or {})
    pcv = {v.get('name') for v in meta.get('pcVars', [])}     # the schema's pc_vars, via the meta

    # A LIST, then a set — because the interesting fault is a path that appears TWICE.
    #
    # This was a set, and a set is exactly the wrong shape: two nodes at one path collapse into one
    # entry, every check downstream passes, and the tree shows the branch twice with both copies
    # opening the same page. One sat in this document for weeks — "Water & Methanol" under Engine
    # Functions, once in its proper place and once appended after Electronic throttle — and check_doc
    # reported 0 complaints the whole time. The installers do not create it (dropping one and
    # reinstalling all twelve does not bring it back), so it is stale state, which is precisely the
    # kind this file exists to find: the document is not in any repo and nothing else looks at it.
    node_list = []

    def walk_tree(n, path=''):
        nm = n.get('name', '')
        pp = (path + '/' + nm if nm else path).lstrip('/')
        if nm:
            node_list.append(pp)
        for c in (n.get('children') or []):
            walk_tree(c, pp)
    for r in d['tree']:
        walk_tree(r)
    nodes = set(node_list)

    bad = collections.defaultdict(list)

    for pth, n in sorted(collections.Counter(node_list).items()):
        if n > 1:
            bad['tree node appears more than once'].append(f'{pth} x{n}')

    # ---- reachability ---------------------------------------------------------------------------
    # A '%' key is reached by the CHROME, not by the tree: "%Status" is the status-lamp grid, hosted in
    # the Status Lamps dock. A page path is built from menu labels, so no tree node can ever name one —
    # which is what makes the prefix safe, and why "no tree node" is its normal state rather than a fault.
    for k in sorted(k for k in set(lib) - nodes if not k.startswith('%')):
        bad['page with no tree node (invisible)'].append(k)
    for k in sorted(nodes - set(lib)):
        bad['tree node with no page (opens onto nothing)'].append(k)

    pc_used = collections.Counter()
    widgets = 0
    # SURFACES CARRY WIDGETS TOO. A tab (Main, Diagnostics, the workspaces) is a canvas of its own, not
    # a library page, and skipping them reported pc.diag_view — the Diagnostics sub-tab selector, used
    # eight times on that surface — as a host variable nobody references.
    surfaces = {f'surface:{e.get("name") or i}': (e.get('model') or {})
                for i, e in enumerate(d.get('surfaces', {}).get('pool') or [])}
    for name, pan in list(lib.items()) + list(surfaces.items()):
        uids = collections.Counter()
        # ---- geometry ---------------------------------------------------------------------------
        cw, chh = pan.get('canvasWidth', 1280), pan.get('canvasHeight', 700)

        def geom(ws, box_w, box_h, owner):
            for w in ws:
                x, y, ww, hh = w.get('x', 0), w.get('y', 0), w.get('w', 0), w.get('h', 0)
                if x < 0 or y < 0:
                    bad['off the top/left'].append(f'{name}: {w["type"]} at ({x},{y})')
                if x + ww > box_w:
                    bad['past the right edge'].append(
                        f'{name}: {owner} {w["type"]} right {x + ww} > {box_w}')
                if y + hh > box_h:
                    bad['past the bottom edge'].append(
                        f'{name}: {owner} {w["type"]} bottom {y + hh} > {box_h}')
                kids = _kids(w)
                if kids:
                    # A panel hands its children h - titleBar - 4; measuring them against the panel's
                    # own height is measuring against a box 26px taller than the one they are in.
                    titled = bool((w.get('props') or {}).get('labelText'))
                    geom(kids, ww - PAD, hh - (PANEL_TITLE if titled else 0) - PAD,
                         (w.get('props') or {}).get('labelText') or 'panel')
        geom(pan.get('widgets') or [], cw, chh, 'canvas')

        # ---- text -------------------------------------------------------------------------------
        # MEASURED, with the app's own font, by the same code the builders' checker uses (ruler.py via
        # author.caption_complaints). What was here was a character-count estimate that only ever looked
        # at WRAPPED labels and only at their height — so a one-line caption too wide for its box, which
        # is the commonest way a caption is lost, was invisible to it.
        for c in A.caption_complaints(name, pan.get('widgets') or [], _kids):
            bad['caption clipped by its own box'].append(c)

        # …AND THE READING, when the reading is a WORD. A readout bound to an enum-valued channel shows
        # its label, not the index — "Closed Loop", not 5 — so a cell sized for a number clips it, with
        # no ellipsis and nothing to say so. This is the same measurement the captions get, applied to
        # the one text on a page that is not authored anywhere: it comes from the meta at paint time.
        for w in _flat(pan.get('widgets') or []):
            if w.get('type') != 'value':
                continue
            pr = w.get('props') or {}
            ch = pr.get('signalName') or ''
            labels = A.Page.chan_enum_labels(ch)
            if not labels:
                continue
            from ruler import ruler, font_px
            px = font_px(pr.get('fontName') or '', 32)
            need = max(int(ruler().width(l, px)) for l in labels) + 8
            # MEASURED AT THE FLOOR THE WIDGET WILL ACTUALLY USE. ValueWidget shrinks a reading that
            # does not fit rather than guillotining it, down to 10px — so a cell narrower than the
            # label at its authored size is not yet a fault, it is a smaller word. What IS a fault is
            # a cell too narrow to show the label even shrunk, because there the letters do go.
            floor = max(int(ruler().width(l, 10)) for l in labels) + 8    # ValueWidget's own floor
            cell = int(w.get('w') or 0)
            if floor > cell:
                bad['state reading clipped by its own box'].append(
                    f'{name}: {ch} needs {floor} even shrunk (authored {need}) > {cell}')

        for w in _flat(pan.get('widgets') or []):
            widgets += 1
            uids[w.get('uid')] += 1
            pr = w.get('props') or {}
            # ---- links --------------------------------------------------------------------------
            lk = pr.get('link')
            if lk and lk not in nodes:
                bad['link naming no node'].append(f'{name}: -> {lk}')
            # ---- bindings -----------------------------------------------------------------------
            for key in ('signalName', 'condition', 'enableCondition', 'expr', 'target', 'rowCount'):
                v = pr.get(key)
                if not isinstance(v, str) or not v:
                    continue
                for c in re.findall(r'\[\$([A-Za-z0-9_]+)\]', v):
                    if tel and c not in tel:
                        bad['unknown telemetry channel'].append(f'{name}: [${c}]')
                for hp in re.findall(r'pc\.([A-Za-z0-9_]+)', v):
                    pc_used[hp] += 1
                    if pcv and hp not in pcv:
                        bad['undeclared host variable'].append(f'{name}: pc.{hp}')
            sig = (pr.get('signalName') or '').strip()
            mm = re.fullmatch(r'\[#([^\]]+)\]|([a-z_]+\.[A-Za-z0-9_\[\]\.]+)', sig)
            if mm:
                path = mm.group(1) or mm.group(2)
                if not path.startswith('pc.'):
                    norm = re.sub(r'\[[^\[\]]*\]', '[*]', path)
                    if norm not in cfg:
                        bad['binding naming no config field'].append(f'{name}: {sig}')
        for u, c in uids.items():
            if c > 1:
                bad['duplicate uid within one page'].append(f'{name}: {u} x{c}')

    for n in sorted(pcv - set(pc_used)):
        bad['host variable nobody references'].append(f'pc.{n}')

    # SWITCHED OFF SHOULD MEAN OUT OF THE TREE, and the check is an ABSENCE — which is why nothing
    # caught it for as long as it was wrong. Every other audit here asks whether what is present is
    # correct; this asks whether something that should exist does. Idle Control and Boost Control sat
    # in Engine Functions while switched off, alone among thirteen features, because the installer
    # that owns those two branches set the node condition only when it CREATED the node and both are
    # always moved into place by an earlier one. A branch of tables for a module that is not running
    # is a list of things to tune that do nothing.
    #
    # The invariant is deliberately the weakest one that catches it: SOME node is gated on the
    # module's enable. Which node is the installers' business, and a feature whose branch lives in two
    # places (a workspace tab and a tree branch) is still correct by this test.
    node_conds = []
    def _conds(n):
        if n.get('condition'):
            node_conds.append(n['condition'])
        for k in n.get('children') or []:
            _conds(k)
    for n in d['tree']:
        _conds(n)
    joined = ' ;; '.join(node_conds)
    for mod, fields in sorted(meta['config'].items()):
        if not isinstance(fields.get('enabled'), dict):
            continue                                  # no feature switch: nothing to hide it by
        if f'{mod}.enabled' not in joined:
            bad['feature stays in the tree while switched off'].append(
                f'{mod}: no tree node is gated on {mod}.enabled')

    # CONTROLS. A tick box is the same size on every page or it is not the same control. JCheckBox draws
    # a box of its RECT'S HEIGHT (JCheckBox.h: `float boxSz = b.height`), so an off-size rect is off-size
    # ink, not a spacing detail — one document once held boxes at 20, 18 and 16 (author.py's checkbox
    # comment). The catalogue on Configuration/Sensors is the ONE exception and it is load-bearing: its
    # rows are on a 20px pitch because that is what fits 134 inputs into six columns of a 1280px page,
    # and a 22px box on a 20px pitch overlaps its neighbour's hit rect by two pixels — the lower element
    # wins a hit test, so the bottom of every box toggled the sensor BELOW it.
    CHECK_EXEMPT = {'Configuration/Sensors': 20}
    for name, pan in sorted(lib.items()):
        want = CHECK_EXEMPT.get(name, A.CHECK_H)
        for w in _flat(pan.get('widgets') or []):
            if w.get('type') != 'checkbox':
                continue
            h, bw = w.get('h'), w.get('w')
            if h != want or bw != want:
                bad['tick box is not the size a tick box is'].append(
                    f'{name}: {bw}x{h}, expected {want}x{want}'
                    f'{" (page pitch)" if name in CHECK_EXEMPT else ""}')

    total = sum(len(v) for v in bad.values())
    print(f'{len(lib)} pages, {len(surfaces)} surfaces, {len(nodes)} tree nodes, {widgets} widgets')
    for kind in sorted(bad):
        items = bad[kind]
        print(f'\n{kind}  ({len(items)})')
        for i in items[:40]:
            print(f'    {i}')
        if len(items) > 40:
            print(f'    … and {len(items) - 40} more')
    print(f'\n{total} complaint(s)')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
