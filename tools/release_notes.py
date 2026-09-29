#!/usr/bin/env python3
"""The firmware's change notes: firmware/CHANGES.md, what the studio shows before a firmware update.

  python3 tools/release_notes.py check            is the Unreleased section in step with the code? (read-only)
  python3 tools/release_notes.py prepare --board B   make release's first step (see below)

THE NOTES ARE WRITTEN WITH THE WORK, NOT AT RELEASE TIME. A commit that changes something a user would
notice adds a line under "## Unreleased" in the same commit (tools/hooks/pre-commit reminds). Nothing
about a release is then left to remember:

  prepare   Asks GitHub which firmware build the newest published kit for BOARD is. If the firmware
            has not changed since, there is nothing to release and the version stays. If it has:
              * Unreleased must say what changed — refused otherwise, with the commits to write it from;
              * firmware/version.txt goes up a patch, unless it was already raised by hand (0.5.0);
              * Unreleased becomes that version's section, and a fresh empty Unreleased goes on top;
              * the two files are committed as "firmware <version>", so the build is that commit.

The file:

    ## Unreleased
    - Plain words for a user, one change a line.

    ## 0.4.1
    - ...

make_kit.py puts every version's section into kit.json; the studio shows the ones newer than the ECU runs.
"""
import argparse, json, pathlib, re, subprocess, sys, urllib.request

REPO = pathlib.Path(__file__).resolve().parent.parent
NOTES = REPO / 'firmware' / 'CHANGES.md'
VERSION = REPO / 'firmware' / 'version.txt'
RELEASES = "https://api.github.com/repos/jaytektas/jayecu/releases?per_page=100"
# What the firmware's build id is taken from (codegen/update_version.sh) — the notes and the version file
# are left out of the "did the firmware change" question: writing a note is not a firmware change.
FW_PATHS = ['firmware', 'definition', 'codegen', 'generated', 'third_party', 'cmake',
            ':(exclude)firmware/version.h', ':(exclude)firmware/CHANGES.md', ':(exclude)firmware/version.txt']


def die(msg):
    print(f'release_notes: {msg}', file=sys.stderr)
    sys.exit(1)


def vkey(v):
    """1.2.3 < 1.2.4-beta.1 < 1.2.4 — the same order as make_release and the studio."""
    base, _, pre = v.lstrip('v').partition('-')
    nums = tuple(int(x) for x in base.split('.') if x.isdigit())
    return nums + (0,) * (3 - len(nums)), (1,) if not pre else (0, pre)


def parse(text=None):
    """[(heading, [lines])] in file order; heading is 'Unreleased' or a version."""
    text = NOTES.read_text() if text is None else text
    out = []
    for line in text.splitlines():
        m = re.match(r'^##\s+(.+?)\s*$', line)
        if m:
            out.append((m.group(1), []))
        elif out and line.startswith('- '):
            out[-1][1].append(line[2:].strip())
        elif out and line.startswith('  ') and out[-1][1]:
            out[-1][1][-1] += ' ' + line.strip()          # a note wrapped onto the next line
    return out


def released():
    """[{version, changes}] for every released section — what a kit carries."""
    return [{'version': h, 'changes': lines} for h, lines in parse() if h != 'Unreleased']


def unreleased():
    return next((lines for h, lines in parse() if h == 'Unreleased'), None)


def git(*args):
    return subprocess.run(['git', *args], cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()


def published_kit(board):
    """(version, build) of the newest published kit for `board`, or (None, None)."""
    get = lambda url: json.load(urllib.request.urlopen(
        urllib.request.Request(url, headers={'User-Agent': 'jayecu-release'}), timeout=15))
    best = (None, None)
    for rel in get(RELEASES):
        for a in rel.get('assets', []):
            m = re.fullmatch(re.escape(board) + r'-(.+)-kit\.json', a.get('name', ''))
            if m and (best[0] is None or vkey(m.group(1)) > vkey(best[0])):
                best = (m.group(1), get(a['browser_download_url']).get('build', ''))
    return best


def prepare(board):
    if unreleased() is None:
        die(f'{NOTES.relative_to(REPO)} has no "## Unreleased" section')
    dirty = git('status', '--porcelain', '--', *FW_PATHS, 'firmware/CHANGES.md', 'firmware/version.txt')
    if dirty:
        die('commit first — the firmware has uncommitted changes:\n' + dirty)
    pub_ver, pub_build = published_kit(board)
    head = git('log', '-1', '--format=%h', '--', *FW_PATHS)
    if pub_build and git('rev-parse', pub_build) == git('rev-parse', head):
        print(f'  firmware unchanged since {pub_ver} (build {pub_build}): no new firmware version')
        return
    if pub_build:
        # Same code, different id? The published build is an ancestor with nothing firmware since it.
        since = git('log', '--format=- %s (%h)', f'{pub_build}..HEAD', '--', *FW_PATHS)
        if not since:
            print(f'  firmware unchanged since {pub_ver} (build {pub_build}): no new firmware version')
            return
    else:
        since = '(no published kit to compare with)'
    notes = unreleased()
    cur = VERSION.read_text().strip()
    # PREPARED BUT NOT PUBLISHED — a release that stopped part-way (a build failed, the upload did not
    # happen) and is being run again. Its version is already raised and has its section; asking again
    # would bump past a version nobody ever received. Notes written since go into that same section.
    if pub_ver and vkey(cur) > vkey(pub_ver) and any(h == cur for h, _ in parse()):
        # Since the notes last caught up — the version bump, or notes added to it since.
        stamp = git('log', '-1', '--format=%H', '--', 'firmware/version.txt', 'firmware/CHANGES.md')
        later = git('log', '--format=- %s (%h)', f'{stamp}..HEAD', '--', *FW_PATHS)
        if not notes:
            if later:
                die(f'the firmware changed after {cur} was prepared, and "## Unreleased" does not say how. '
                    'The commits since:\n' + later)
            print(f'  firmware {cur}: prepared, not yet published — carrying on')
            return
        text = NOTES.read_text()
        text = re.sub(r'^## Unreleased\s*\n(.*?)(?=^## )', '## Unreleased\n\n', text, count=1, flags=re.M | re.S)
        text = re.sub(r'^(## ' + re.escape(cur) + r'\s*\n)', r'\1' + ''.join(f'- {n}\n' for n in notes),
                      text, count=1, flags=re.M)
        NOTES.write_text(text)
        git('add', 'firmware/CHANGES.md')
        git('commit', '-q', '-m', f'firmware {cur}: more notes\n\n' + '\n'.join(f'- {n}' for n in notes))
        print(f'  firmware {cur}: prepared, not yet published — {len(notes)} more note(s) added to it')
        return
    if not notes:
        die('the firmware changed since the last release, and "## Unreleased" in firmware/CHANGES.md does '
            'not say how. Write a line per change a user would notice, commit, and make release again. '
            'The commits since ' + (pub_ver or 'the start') + ':\n' + since)
    if pub_ver and vkey(cur) <= vkey(pub_ver):
        base = re.match(r'(\d+)\.(\d+)\.(\d+)', pub_ver)
        new = f'{base.group(1)}.{base.group(2)}.{int(base.group(3)) + 1}'
    else:
        new = cur                                          # already raised by hand
    text = NOTES.read_text()
    text = re.sub(r'^## Unreleased\s*$', f'## Unreleased\n\n## {new}', text, count=1, flags=re.M)
    NOTES.write_text(text)
    VERSION.write_text(new + '\n')
    git('add', 'firmware/CHANGES.md', 'firmware/version.txt')
    git('commit', '-q', '-m', f'firmware {new}\n\n' + '\n'.join(f'- {n}' for n in notes))
    print(f'  firmware {new}: {len(notes)} note(s), committed as {git("log", "-1", "--format=%h")}')


def check():
    """For the pre-commit hook: firmware staged, notes not — a reminder, never a refusal."""
    staged = git('diff', '--cached', '--name-only', '--', *FW_PATHS).splitlines()
    staged = [f for f in staged if not f.startswith('generated/')]   # codegen output follows its source
    if staged and 'firmware/CHANGES.md' not in git('diff', '--cached', '--name-only').splitlines():
        print('note: this commit changes the firmware (' + ', '.join(staged[:3]) + (', …' if len(staged) > 3 else '')
              + ') but not firmware/CHANGES.md.\n      If a user would notice, add a line under "## Unreleased".',
              file=sys.stderr)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['check', 'prepare'])
    ap.add_argument('--board', default='jaytek_v1')
    a = ap.parse_args()
    prepare(a.board) if a.cmd == 'prepare' else check()
