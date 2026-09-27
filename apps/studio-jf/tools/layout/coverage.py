"""Which configuration settings have nowhere to be edited — the completeness check for the pages.

A page you forgot to build looks exactly like a page nobody has needed yet: nothing complains, and the
setting is simply unreachable. The one honest measure is the CONFIG itself — every field the schema
declares should be somewhere in the document, and the ones that are not are the work list.

Three kinds of "not on a page" are not gaps, and saying so is most of the value here:

  TABLE-OWNED   a table's own axis machinery — the breakpoint arrays, the live bin counts, the axis
                channel selectors and the optional-axis flags. The table editor's context menu owns
                these; putting them on the page as well is duplication, not coverage.
  STRUCTURAL    padding, and the parent WORD of a packed bit group (its named bits are bound
                individually, which is how a user edits it).
  DERIVED       fields written by a calibration routine rather than typed (an ETB's learned limits).
  RESERVED      declared and deliberately unused — a table's second axis that the model does not read.
                The schema says so in its own label and help, and a page for it would be a page for
                nothing.
  DIALOG-OWNED  edited together somewhere that is not a page, because they only mean anything as a set.
                A recording PROFILE is the example: channel mask, start and stop conditions, run and
                re-arm times. A page control per field would be a second place to change one thing.
  LEARNED       the ECU writes it, not the tuner — the schema puts it in the `learned` segment and says
                it is not part of the tune. An editor would be a control the next engine cycle
                overwrites. Worth a READOUT, which is a different question from coverage.

Everything else is either bound somewhere in dashboard.gui or a real hole.

    python3 coverage.py            # the summary and the holes
    python3 coverage.py --all      # …and every field, classified
"""
import json, re, sys, collections
from apply_fuel import DOC, load_meta

TABLE_SUFFIXES = ('_axis', '_axis_n', '_x_src', '_y_src', '_z_src', '_x_en', '_y_en', '_z_en',
                  '_x_axis', '_y_axis', '_z_axis', '_yaxis', '_xaxis', '_n')


def declared(meta):
    """Every editable config field the meta declares: (path -> kind)."""
    out = {}
    def walk(node, path=''):
        for k, v in (node or {}).items():
            if not isinstance(v, dict):
                continue
            p = f'{path}{k}'
            t = v.get('type')
            if 'fields' in v:                                   # array of structs
                for fk, fv in v['fields'].items():
                    out[f'{p}[*].{fk}'] = ('bits' if fv.get('bits') else 'elem')
                    for bk in (fv.get('bits') or {}):
                        out[f'{p}[*].{fk}.{bk}'] = 'elem'
                for tk in (v.get('tables') or {}):
                    out[f'{p}[*].{tk}'] = 'elemtable'
                for ak, av in (v.get('arrays') or {}).items():
                    for sk in av.get('fields', {}):
                        out[f'{p}[*].{ak}[*].{sk}'] = 'elemarr'
            elif t == 'table' or ('cols' in v and 'rows' in v):
                out[p] = 'table'
            elif t == 'scalar' or ('datatype' in v and 'offset' in v):
                out[p] = 'bits' if v.get('bits') else 'scalar'
                for bk in (v.get('bits') or {}):
                    out[f'{p}.{bk}'] = 'scalar'
            elif t == 'array':
                out[p] = 'array1d'
            else:
                walk(v, p + '.')
    walk(meta['config'])
    return out


def bound(doc):
    """Every config path the document names — bindings, sigils in conditions, and preset writes."""
    seen = set()
    def add(p):
        p = p.strip()
        if not p or '.' not in p:
            return
        # A COMPUTED SUBSCRIPT carries its own brackets — "engine.cyl[@firing_order[0].cyl - 1].bank" —
        # so the subscript has to be matched by DEPTH, not by the first ']'. A regex stopping at that
        # one turned the path into "cyl[*].cyl - 1].bank" and reported the field as unreachable.
        out, i, depth = '', 0, 0
        while i < len(p):
            c = p[i]
            if c == '[':
                if depth == 0:
                    out += '[*]'
                depth += 1
            elif c == ']':
                depth -= 1
            elif depth == 0:
                out += c
            i += 1
        seen.add(out)
    def walk(ws):
        for w in ws:
            pr = w.get('props', {})
            sig = pr.get('signalName', '')
            # The BINDING ITSELF, always — and any sigils inside it as well. A path whose subscript is
            # computed carries a sigil in the middle of it ("outputs.output[@[#pc.output_sel]].kind"),
            # and reading that as "an expression over pc.output_sel" credited the host variable and
            # missed the twelve fields the page actually edits.
            if sig:
                add(sig)
            # A WIDGET THAT EDITS A PAIR covers both halves. The CAN field picker binds a sensor's
            # `can_frame` and derives `can_bit` as its sibling — they are one choice (which frame, and
            # which field of it), so a single control writes both and only one of them carries the
            # binding this scan looks for.
            if w.get('type') == 'canfield' and sig.endswith('.can_frame'):
                add(sig[:-len('can_frame')] + 'can_bit')
            for r in re.findall(r'\[#((?:[^\[\]]|\[[^\]]*\])+)\]', sig):
                add(r)
            for key in ('condition', 'enableCondition', 'minExpr', 'maxExpr', 'ranges'):
                for r in re.findall(r'\[#((?:[^\[\]]|\[[^\]]*\])+)\]', pr.get(key, '')):
                    add(r)
            for pair in re.findall(r'([a-z0-9_]+(?:\[[^\]]*\])?(?:\.[a-z0-9_]+(?:\[[^\]]*\])?)+)\s*=', pr.get('presets', '')):
                add(pair)
            kids = pr.get('children')
            if kids:
                walk(json.loads(kids))
    for pg in doc['panelLibrary'].values():
        walk(pg.get('widgets', []))
    return seen


# Settings a DIALOG owns as a set. Listed by path prefix because the reason is about the group, not
# about any one field: see the module docstring.
DIALOG_OWNED = {
    'datalog.': 'the recording profile — Logging > Onboard Logging...',
    # The SCRIPT *is* on its page — the `script` element on Lua Scripting. It is excused here because
    # it carries no BINDING for this scan to find:
    # which config field holds the script differs per firmware (lua.source here, ts.luaScript on a
    # rusEFI ECU), so the editor asks the loaded definition instead of naming one in the document.
    'lua.source': 'the script editor on Lua Scripting — it resolves its own field, so it has no binding',
    # The generic CAN pool is a FRAME LIST, not a page of settings: 96 frames x 8 fields and 512
    # fields x 8, whose meaning is the bit layout they make together. A page of 4,864 spin boxes would
    # be a worse way to say it than the frame editor, which draws the layout the wire carries.
    'can.gc_frame': 'the frame list — Configuration > CAN Bus > CANn > Receive/Transmit',
    'can.gc_field': 'the frame list — Configuration > CAN Bus > CANn > Receive/Transmit',
}


def classify(path, kind, tables, meta=None):
    if '_align_pad' in path:
        return 'STRUCTURAL'
    # NOT PART OF THE TUNE. The schema says so itself, so this needs no list to maintain.
    if meta is not None:
        d = _field(meta, path)
        if isinstance(d, dict) and d.get('segment') == 'learned':
            return 'LEARNED'
    for pre in DIALOG_OWNED:
        # `enabled` and `rate_hz` ARE on the page; only what the dialog edits as a set is excused, and
        # a field already found on a page never reaches classify() anyway.
        if path.startswith(pre):
            return 'DIALOG-OWNED'
    if meta is not None:
        d = _field(meta, path)
        if isinstance(d, dict) and (str(d.get('label', '')).lower().endswith('(reserved)')
                                    or str(d.get('help', '')).startswith('Reserved')):
            return 'RESERVED'
    if kind == 'bits':
        return 'STRUCTURAL'                      # the packed word; its named bits are bound separately
    leaf = path.rsplit('.', 1)[-1]
    stem = re.sub(r'\[\*\]', '', path)
    for t in tables:                             # "<table>_x_src", "<table>_y_en", "<table>_x_axis" …
        if stem.startswith(t) and stem != t and stem[len(t):].startswith(('_', '')):
            if any(stem.endswith(s) for s in TABLE_SUFFIXES):
                return 'TABLE-OWNED'
    if leaf.endswith(('_axis', '_axis_n')) or leaf in ('cols_n', 'rows_n'):
        return 'TABLE-OWNED'
    return 'MISSING'


def _field(meta, path):
    node = meta['config']
    for part in re.sub(r'\[\*\]', '', path).split('.'):
        if not isinstance(node, dict):
            return None
        node = node.get(part) or (node.get('fields') or {}).get(part)
    return node


def main():
    meta, doc = load_meta(), json.load(open(DOC))
    fields, have = declared(meta), bound(doc)
    tables = {re.sub(r'\[\*\]', '', p) for p, k in fields.items() if k in ('table', 'elemtable')}
    rows = collections.defaultdict(list)
    for p, kind in sorted(fields.items()):
        norm = re.sub(r'\[[^\[\]]*\]', '[*]', p)
        state = 'covered' if norm in have else classify(p, kind, tables, meta)
        rows[state].append((p, kind))
    total = sum(len(v) for v in rows.values())
    print(f'{total} config fields: {len(rows["covered"])} on a page, '
          f'{len(rows["TABLE-OWNED"])} table-owned, {len(rows["STRUCTURAL"])} structural, '
          f'{len(rows["RESERVED"])} reserved, {len(rows["DIALOG-OWNED"])} dialog-owned, '
          f'{len(rows["LEARNED"])} learned, {len(rows["MISSING"])} MISSING\n')
    by_mod = collections.defaultdict(list)
    for p, kind in rows['MISSING']:
        by_mod[p.split('.')[0]].append((p, kind))
    for mod, items in sorted(by_mod.items(), key=lambda kv: -len(kv[1])):
        print(f'{mod}  ({len(items)})')
        for p, kind in items:
            print(f'    {kind:10} {p}')
    if '--all' in sys.argv:
        for state in ('TABLE-OWNED', 'STRUCTURAL', 'RESERVED', 'DIALOG-OWNED', 'LEARNED'):
            print(f'\n--- {state} ---')
            for p, kind in rows[state]:
                print(f'    {kind:10} {p}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
