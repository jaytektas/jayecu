#!/usr/bin/env bash
# Build a BETA RELEASE: studio packages and a beta firmware kit per board, gathered into release/v<beta>/
# ready to publish as a GitHub PRE-RELEASE. Studios with Preferences ▸ "Include beta studio versions" take
# the studio; ones with "Include beta firmware" are offered the kits. Nothing is published here.
#
#   tools/beta_release.sh ["jaytek_v1 proteus_f7"]       `make beta-release [BOARDS=...]`
#
# THE ORDER IS THE POINT (as bench_beta.sh):
#   1. the whole tree is COMMITTED — the studio and every kit must name a commit
#   2. the versions: studio <next patch>-beta.N, firmware <next patch>-beta.M, each one past every beta
#      published AND every beta built here (the bench's come from firmware/build/<board>/betas) — the
#      studio offers by version alone, so a beta that sorts below one already out reaches nobody
#   3. per board: firmware BUILT (codegen first), then its kit made from that image; $BOARD LAST, because
#      generated/ holds one board at a time and the studio is built against it
#   4. the studio built with that version (STUDIO_VERSION_OVERRIDE), packaged with THESE beta kits, and the
#      build dirs put back to the plain version afterwards — a later `make studio` must not say beta
#   5. make_release.py gathers release/v<studio beta>/ + SHA256SUMS and refuses a tag that is not newer
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BOARD="${BOARD:-jaytek_v1}"
BOARDS="${1:-$BOARD}"
say() { printf '  beta-release: %s\n' "$*"; }
die() { printf '  beta-release: %s\n' "$*" >&2; exit 1; }

# 1. committed, and the Windows SDK is the pinned framework (a stale one fails halfway through)
[ -z "$(git status --porcelain)" ] || { git status --short | head -10 >&2; die "commit everything first"; }
PIN=$(cat apps/studio-jf/jframework.commit)
[ "$(cat ~/jframework-sdk-win/jframework.commit 2>/dev/null)" = "$PIN" ] || die "the Windows SDK is not JFramework $PIN: make sdk-win"
[ "$(cat ~/jframework-sdk/jframework.commit 2>/dev/null)" = "$PIN" ] || die "the Linux SDK is not JFramework $PIN: make sdk"

# 2. the versions
next_patch() { local IFS='.'; read -r a b c <<< "${1%%[-+]*}"; echo "$a.$b.$((c + 1))"; }
S_NEXT=$(next_patch "$(sed -n 's/^project.[a-z_]* VERSION \([0-9.]*\).*/\1/p' apps/studio-jf/CMakeLists.txt)")
F_NEXT=$(next_patch "$(cat firmware/version.txt)")
TAGS=$(gh release list -L 100 --json tagName --jq '.[].tagName') || die "could not list the published releases"
S_N=$(sed -n "s/^v$S_NEXT-beta\.\([0-9]*\)$/\1/p" <<< "$TAGS" | sort -n | tail -1)
STUDIO_VER="$S_NEXT-beta.$(( ${S_N:-0} + 1 ))"
# Published firmware betas are named in the assets (<board>-<ver>-kit.json); local ones are folders.
F_SEEN=$( { for t in $(grep -- '-beta\.' <<< "$TAGS" || true); do
                gh release view "$t" --json assets --jq '.assets[].name'; done
            for b in $BOARDS; do ls "firmware/build/$b/betas" 2>/dev/null || true; done; } \
          | sed -n "s/^[a-z0-9_]*-$F_NEXT-beta\.\([0-9]*\)\(-kit\.json\)\{0,1\}$/\1/p" | sort -n | tail -1)
FW_VER="$F_NEXT-beta.$(( ${F_SEEN:-0} + 1 ))"
say "studio $STUDIO_VER, firmware $FW_VER, boards: $BOARDS"

# 3. firmware + kit per board, $BOARD last
ORDER="$(for b in $BOARDS; do [ "$b" = "$BOARD" ] || echo "$b"; done) $(grep -qw "$BOARD" <<< "$BOARDS" && echo "$BOARD")"
SHIP="firmware/build/ship-kits-beta"
rm -rf "$SHIP" && mkdir -p "$SHIP"
for b in $ORDER; do
    say "firmware + kit: $b"
    make -s firmware BOARD="$b" > /tmp/beta_release_fw.log 2>&1 || { tail -20 /tmp/beta_release_fw.log >&2; die "$b firmware build failed"; }
    python3 tools/make_kit.py --board "$b" --version "$FW_VER" --out "firmware/build/$b/betas" || die "make_kit refused $b"
    cp -r "firmware/build/$b/betas/$b-$FW_VER" "$SHIP/"
done

# 4. the studio, as the beta — build dirs restored to the plain version however this ends
restore() {
    cmake -S apps/studio-jf -B apps/studio-jf/build -DSTUDIO_VERSION_OVERRIDE= > /dev/null 2>&1 || true
    [ -d apps/studio-jf/build-win ] && cmake -S apps/studio-jf -B apps/studio-jf/build-win -DSTUDIO_VERSION_OVERRIDE= > /dev/null 2>&1 || true
}
trap restore EXIT
cmake -S apps/studio-jf -B apps/studio-jf/build -DSTUDIO_VERSION_OVERRIDE="$STUDIO_VER" > /dev/null
[ -d apps/studio-jf/build-win ] && cmake -S apps/studio-jf -B apps/studio-jf/build-win -DSTUDIO_VERSION_OVERRIDE="$STUDIO_VER" > /dev/null
say "studio (linux) + manual"
make -s studio manual > /tmp/beta_release_studio.log 2>&1 || { tail -20 /tmp/beta_release_studio.log >&2; die "studio build failed"; }
KITS="$SHIP" apps/studio-jf/tools/make_appimage.sh apps/studio-jf/build > /tmp/beta_release_appimage.log 2>&1 \
    || { tail -20 /tmp/beta_release_appimage.log >&2; die "AppImage failed"; }
say "studio (windows) + installer"
make -s studio-win > /tmp/beta_release_win.log 2>&1 || { tail -20 /tmp/beta_release_win.log >&2; die "windows build failed"; }
grep -q "\"$STUDIO_VER\"" apps/studio-jf/build-win/generated/StudioVersion.cpp || die "the windows build does not say $STUDIO_VER"
(cd apps/studio-jf && WINEDEBUG=-all ${ISCC:-wine C:/InnoSetup/ISCC.exe} /DAppVersion="$STUDIO_VER" \
    '/DKitsDir=..\..\..\firmware\build\ship-kits-beta' installer/studio.iss) > /tmp/beta_release_iss.log 2>&1 \
    || { tail -20 /tmp/beta_release_iss.log >&2; die "installer failed"; }

# 5. gathered
python3 tools/make_release.py --studio-version "$STUDIO_VER" --fw-version "$FW_VER" --boards "$BOARDS"
say "ready: release/v$STUDIO_VER — publish ONLY after the go-ahead, as a pre-release:"
say "  git push --force origin HEAD:refs/heads/beta     (the tag's commit on GitHub — the beta branch, not the work branch)"
say "  gh release create v$STUDIO_VER --prerelease --target <commit> --title \"jayecu v$STUDIO_VER (beta)\" --notes-file <notes> release/v$STUDIO_VER/*"
