#!/usr/bin/env python3
"""Make an ECU firmware KIT from the current build: firmware image + meta + dashboard + kit.json.

  python3 tools/make_kit.py [--board jaytek_v1] [--out DIR] [--version X.Y.Z] [--min-studio X.Y.Z]

A kit is what the studio flashes and ships (apps/studio-jf/src/model/FirmwareKits.h). The files are
named the way a release names them, so the same folder can be copied beside a studio build
(<studio>/firmware/) or its files attached to a release:

  <board>-<version>-kit.json       -> copied into the kit folder as kit.json
  <board>-<version>-firmware.bin   firmware/build/<board>/code.bin — the application image DFU writes
  <board>-<version>.meta           shared/tuneit-meta.json (JSON + CRC footer)
  <board>-<version>.gui            definition/boards/<board>.dashboard.gui (when there is one)

It REFUSES a mismatched set, because a kit only works as a set: the meta must be for this board, and
must be the layout the firmware was compiled against (generated/schema_meta.h). shared/ and generated/
hold whichever board codegen ran for last, so `make firmware BOARD=<board>` first.

--version overrides firmware/version.txt — for making a pretend-newer kit to test the studio's offer.
"""
import argparse, json, pathlib, re, shutil, sys

REPO = pathlib.Path(__file__).resolve().parent.parent

ap = argparse.ArgumentParser()
ap.add_argument('--board', default='jaytek_v1')
ap.add_argument('--out', help='parent folder for the kit (default firmware/build/<board>/kits)')
ap.add_argument('--version', help='default: firmware/version.txt')
ap.add_argument('--min-studio', help="default: the meta's own min_studio (firmware/min_studio.txt)")
a = ap.parse_args()

fw   = REPO / 'firmware' / 'build' / a.board / 'code.bin'
meta = REPO / 'shared' / 'tuneit-meta.json'
gui  = REPO / 'definition' / 'boards' / f'{a.board}.dashboard.gui'
for p in (fw, meta):
    if not p.exists():
        sys.exit(f'missing {p} — run `make firmware BOARD={a.board}` first')

raw = meta.read_bytes()
m = json.loads(raw[:-4])['meta']                      # strip the 4-byte CRC footer
if m['board'] != a.board:
    sys.exit(f'shared/tuneit-meta.json is for {m["board"]}, not {a.board} — run `make firmware BOARD={a.board}`')
compiled = re.search(r'JAYECU_LAYOUT_HASH_STR\s+"([0-9a-f]+)"', (REPO / 'generated' / 'schema_meta.h').read_text())
if not compiled or compiled.group(1) != m['layout_hash']:
    sys.exit(f'the meta ({m["layout_hash"]}) is not the layout generated/ was built for — rebuild the firmware')

version = a.version or (REPO / 'firmware' / 'version.txt').read_text().strip()
stem = f'{a.board}-{version}'
out = pathlib.Path(a.out) if a.out else REPO / 'firmware' / 'build' / a.board / 'kits'
kit_dir = out / stem
shutil.rmtree(kit_dir, ignore_errors=True)
kit_dir.mkdir(parents=True)

label = {
    'board': a.board, 'version': version, 'build': m.get('fw_build', ''),
    'layout_hash': m['layout_hash'], 'min_studio': a.min_studio or m.get('min_studio', '0.1.0'),
    'firmware': f'{stem}-firmware.bin', 'meta': f'{stem}.meta',
}
shutil.copy(fw, kit_dir / label['firmware'])
shutil.copy(meta, kit_dir / label['meta'])
if gui.exists():
    # Stamped, not copied: builtFor names this meta's layout and the studio it needs, and a release
    # refuses a dashboard with a binding that names nothing (tools/stamp_dashboard.py).
    import stamp_dashboard
    label['dashboard'] = f'{stem}.gui'
    stamp_dashboard.stamp_file(gui, kit_dir / label['dashboard'], meta, strict=True)
# WHAT CHANGED, in words, for every version up to this one (firmware/CHANGES.md): the studio shows the
# ones newer than the ECU runs, so someone skipping three versions reads all three. A kit whose own
# version has no notes is refused — the studio would offer an update it cannot explain.
import release_notes
label['notes'] = [n for n in release_notes.released()
                  if release_notes.vkey(n['version']) <= release_notes.vkey(version)]
if not a.version and not any(n['version'] == version for n in label['notes']):
    sys.exit(f'firmware/CHANGES.md has no "## {version}" section — make release writes it from Unreleased')
(kit_dir / 'kit.json').write_text(json.dumps(label, indent=2) + '\n')
print(f'  kit: {kit_dir}  ({a.board} {version}, layout {m["layout_hash"]}, build {label["build"]})')
