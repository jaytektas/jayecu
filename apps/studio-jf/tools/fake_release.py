#!/usr/bin/env python3
"""Serve a pretend GitHub release on localhost, to test the studio's updates before the repos are public.

  STUDIO release:
  python3 tools/fake_release.py --version 0.2.0 --file path/to/new-studio.AppImage   (or a -setup.exe)
  python3 tools/fake_release.py --version 0.2.0 --file x --bad-sum                   (checksum must be refused)

  FIRMWARE release, from kit folders made by jayecu's tools/make_kit.py (start the studio with
  JAYECU_FIRMWARE_URL instead):
  python3 tools/fake_release.py --version 0.2.0 --kit path/to/jaytek_v1-0.2.0 --port 8766

It writes the release JSON GitHub's API would return, a SHA256SUMS, and the file under the name the
studio looks for, then serves them. Start the studio with the JAYECU_UPDATE_URL it prints. On Linux,
also set APPIMAGE to a throw-away file standing in for the running AppImage — that file is what the
update replaces:

  APPIMAGE=/tmp/test.AppImage JAYECU_UPDATE_URL=http://127.0.0.1:8765/latest.json ./build/studio
"""
import argparse, hashlib, http.server, json, pathlib, shutil, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--version', required=True)
ap.add_argument('--file', help='the new studio: an AppImage (Linux) or a setup.exe (Windows)')
ap.add_argument('--kit', action='append', default=[], help='a firmware kit folder (repeat for several boards)')
ap.add_argument('--port', type=int, default=8765)
ap.add_argument('--bad-sum', action='store_true', help='publish a wrong checksum')
args = ap.parse_args()

if bool(args.file) == bool(args.kit):
    ap.error('give --file (a studio release) or --kit (a firmware release), not both')
base = f'http://127.0.0.1:{args.port}'
d = pathlib.Path(tempfile.mkdtemp(prefix='fake_release_'))

names = []
if args.file:
    src = pathlib.Path(args.file)
    suffix = '-setup.exe' if src.name.lower().endswith('.exe') else '-x86_64.AppImage'
    names.append(f'jayecu-studio-{args.version}{suffix}')
    shutil.copy(src, d / names[0])
for k in args.kit:
    kd = pathlib.Path(k)
    label = json.loads((kd / 'kit.json').read_text())
    stem = f"{label['board']}-{label['version']}"
    names.append(f'{stem}-kit.json')                 # a release names the label after its kit
    shutil.copy(kd / 'kit.json', d / names[-1])
    for key in ('firmware', 'meta', 'dashboard'):
        if label.get(key):
            names.append(label[key])
            shutil.copy(kd / label[key], d / label[key])

sums = ''
for i, n in enumerate(names):
    digest = hashlib.sha256((d / n).read_bytes()).hexdigest()
    if args.bad_sum and i == len(names) - 1:
        digest = '0' * 64                            # the last file published is the damaged one
    sums += f'{digest}  {n}\n'
(d / 'SHA256SUMS').write_text(sums)
names.append('SHA256SUMS')
(d / 'latest.json').write_text(json.dumps({
    'tag_name': f'v{args.version}',
    'html_url': f'{base}/',
    'assets': [{'name': n, 'size': (d / n).stat().st_size, 'browser_download_url': f'{base}/{n}'} for n in names],
}, indent=2))

print(f'serving {d}')
print(f"  {'JAYECU_FIRMWARE_URL' if args.kit else 'JAYECU_UPDATE_URL'}={base}/latest.json")
handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(*a, directory=str(d), **k)
http.server.ThreadingHTTPServer(('127.0.0.1', args.port), handler).serve_forever()
