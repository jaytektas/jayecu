#!/usr/bin/env python3
"""Stamp a dashboard's builtFor from the meta it ships with — and only claim what was checked.

A studio reads builtFor for two things: the layout_hash a dashboard was drawn against (when it differs
from the ECU's, the studio walks every binding and names the ones that no longer resolve) and the
studio it needs (older studios say "some parts may not show" instead of silently drawing less). The
authored file in definition/boards is written by layout scripts and by hand, and nothing kept its
builtFor current: it said 6dc604ca three layouts after the fact, so every connect ran the stale-layout
check for nothing, and it named no studio at all.

So every copy that SHIPS a dashboard (make_kit, make studio-meta, push_meta) goes through here:

  * every binding is checked against the meta — config paths, [$channels], pc.* host variables, the
    same rules as the studio's layout checker (apps/studio-jf/tools/layout/check_doc.py);
  * all resolve: builtFor = {layout_hash, telemetry_size} of this meta, studio = the meta's min_studio
    (or the newer studio that last saved it);
  * any does not: the layout_hash is LEFT as it was, so the studio still runs its check and names them
    — stamping the new hash over a broken binding would hide exactly what that check exists to show.

    python3 tools/stamp_dashboard.py <in.gui> <out.gui> [shared/tuneit-meta.json] [--strict]

--strict (a release) fails on an unresolved binding instead of shipping it with a warning.
"""
import json, pathlib, re, sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def _ver(s):
    try:
        return tuple(int(x) for x in str(s).strip().split('.'))
    except ValueError:
        return (0,)


def _config_paths(meta):
    """Every config path the meta declares, subscripts normalised to [*] (check_doc.config_paths)."""
    out = set()

    def walk(node, path=''):
        for k, v in (node or {}).items():
            if not isinstance(v, dict):
                continue
            p = f'{path}{k}'
            if 'fields' in v:
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
    for p in list(out):
        if '[*].' in p:
            out.add(p.split('[*].')[0] + '[*]')
    return out


def _widgets(ws):
    for w in ws or []:
        yield w
        kids = (w.get('props') or {}).get('children')
        if kids:
            yield from _widgets(json.loads(kids) if isinstance(kids, str) else kids)


def unresolved(doc, meta):
    """Every binding in the dashboard that names nothing in this meta."""
    cfg, tel = _config_paths(meta), set(meta.get('telemetry') or {})
    pcv = {v.get('name') for v in meta.get('pcVars', [])}
    bad = []
    canvases = list((doc.get('panelLibrary') or {}).items())
    canvases += [(f'surface:{e.get("name") or i}', e.get('model') or {})
                 for i, e in enumerate((doc.get('surfaces') or {}).get('pool') or [])]
    for name, page in canvases:
        for w in _widgets(page.get('widgets')):
            pr = w.get('props') or {}
            for key in ('signalName', 'condition', 'enableCondition', 'expr', 'target', 'rowCount'):
                v = pr.get(key)
                if not isinstance(v, str) or not v:
                    continue
                bad += [f'{name}: [${c}]' for c in re.findall(r'\[\$([A-Za-z0-9_]+)\]', v) if tel and c not in tel]
                bad += [f'{name}: pc.{h}' for h in re.findall(r'pc\.([A-Za-z0-9_]+)', v) if pcv and h not in pcv]
            sig = (pr.get('signalName') or '').strip()
            m = re.fullmatch(r'\[#([^\]]+)\]|([a-z_]+\.[A-Za-z0-9_\[\]\.]+)', sig)
            if m:
                path = m.group(1) or m.group(2)
                if not path.startswith('pc.') and re.sub(r'\[[^\[\]]*\]', '[*]', path) not in cfg:
                    bad.append(f'{name}: {sig}')
    return bad


def stamp(gui_bytes, meta_doc):
    """(stamped JSON bytes, unresolved bindings)."""
    doc = json.loads(gui_bytes)
    m = meta_doc['meta']
    bad = unresolved(doc, meta_doc)
    built = dict(doc.get('builtFor') or {})
    if not bad:
        built['layout_hash'] = m['layout_hash']
        built['telemetry_size'] = m['telemetry_size']
    need = m.get('min_studio', '0.1.0')
    if _ver(built.get('studio', '0')) < _ver(need):
        built['studio'] = need
    doc['builtFor'] = built
    return json.dumps(doc, ensure_ascii=False, separators=(',', ':')).encode('utf-8'), bad


def load_meta(path):
    raw = pathlib.Path(path).read_bytes()
    return json.loads(raw[:raw.rfind(b'}') + 1].decode('utf-8'))


def stamp_file(src, dst, meta_path=REPO / 'shared' / 'tuneit-meta.json', strict=False):
    out, bad = stamp(pathlib.Path(src).read_bytes(), load_meta(meta_path))
    if bad:
        print(f'  dashboard: {len(bad)} binding(s) name nothing in this meta — layout_hash left as it was, '
              'so the studio names them on connect:', file=sys.stderr)
        for b in bad[:20]:
            print('    ' + b, file=sys.stderr)
        if strict:
            sys.exit('stamp_dashboard: refusing to ship a dashboard with unresolved bindings (--strict)')
    pathlib.Path(dst).write_bytes(out)
    return bad


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    if len(a) < 2:
        sys.exit(__doc__)
    bad = stamp_file(a[0], a[1], a[2] if len(a) > 2 else REPO / 'shared' / 'tuneit-meta.json',
                     strict='--strict' in sys.argv)
    print(f'  dashboard -> {a[1]}' + ('' if not bad else f'  ({len(bad)} unresolved)'))
