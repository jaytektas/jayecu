#!/usr/bin/env bash
# Put a BETA on the bench: a firmware kit (and, with STUDIO=1, the studio) — in the one order that works.
#
#   tools/bench_beta.sh <ssh host> [board]          e.g.  tools/bench_beta.sh bench.lan
#   STUDIO=1 tools/bench_beta.sh bench.lan           …and deploy the studio too (it must be closed there)
#   `make bench-beta BENCH=bench.lan [STUDIO=1]`
#
# THE ORDER IS THE POINT. Each step depends on the one before, and doing them out of order has already
# cost a failed update on the bench:
#   1. everything the firmware is built from is COMMITTED — the build id is the newest commit touching
#      firmware/definition/codegen/…, and a -dirty build cannot be told from any other
#   2. the firmware is BUILT, which runs codegen first: the meta, version.h and the image all come from
#      this one step, so they name the same build. (A codegen after the last firmware build left an image
#      reporting 43ab121 inside a kit labelled 1efcde7; the studio flashed it, saw the old build come
#      back, and refused to put the tune back.)
#   3. the kit is made from that image — make_kit.py reads the build out of code.bin and refuses a mismatch
#   4. the version is NEWER than every beta the bench has seen: the studio offers kits by version only,
#      so a beta that sorts below one already installed is never offered. <next patch>-beta.<N+1>.
#   5. the kit goes into the bench studio's firmware folder (what Tools > Install Firmware Kit does) and
#      onto the Desktop; the studio offers it on the next connect
#   6. STUDIO=1: the AppImage is built and deployed with bench_deploy.sh — only if the studio is closed
#      there, because replacing it under a running one is not a deploy anybody can check
set -euo pipefail
BENCH="${1:?usage: tools/bench_beta.sh <ssh host> [board]}"
BOARD="${2:-jaytek_v1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
say() { printf '  beta: %s\n' "$*"; }
die() { printf '  beta: %s\n' "$*" >&2; exit 1; }

# 1. committed
FW_PATHS=(firmware definition codegen generated third_party cmake ':(exclude)firmware/version.h'
          ':(exclude)firmware/CHANGES.md')
if [ -n "$(git status --porcelain -- "${FW_PATHS[@]}")" ]; then
    git status --short -- "${FW_PATHS[@]}" | head -10 >&2
    die "commit the firmware-side changes first: a beta's build must name a commit"
fi

# 2. built (codegen included)
say "building firmware for $BOARD"
make -s firmware BOARD="$BOARD" > /tmp/bench_beta_fw.log 2>&1 || { tail -20 /tmp/bench_beta_fw.log >&2; die "firmware build failed"; }
BUILD=$(grep -ao "jayecu $BOARD [^ ]* [0-9a-f]\{7,\}" "firmware/build/$BOARD/code.bin" | head -1 | awk '{print $4}')
[ -n "$BUILD" ] || die "no build id found in firmware/build/$BOARD/code.bin"
say "image is build $BUILD"

# 4. the version: next patch of version.txt, beta number one past the highest the bench or this box has
BASE=$(cat firmware/version.txt)
IFS='.' read -r MA MI PA <<< "${BASE%%[-+]*}"
NEXT="$MA.$MI.$((PA + 1))"
SEEN=$( { ssh -o ConnectTimeout=8 "$BENCH" "ls ~/.local/share/jayecu/jayecu\\ Studio/firmware/ 2>/dev/null" || true;
          ls "firmware/build/$BOARD/betas" 2>/dev/null || true; } \
        | sed -n "s/^$BOARD-$NEXT-beta\.\([0-9]*\)$/\1/p" | sort -n | tail -1)
VERSION="$NEXT-beta.$(( ${SEEN:-0} + 1 ))"
say "version $VERSION (highest beta seen: ${SEEN:-none})"

# 3. the kit, from that image
OUT="firmware/build/$BOARD/betas"
python3 tools/make_kit.py --board "$BOARD" --version "$VERSION" --out "$OUT" || die "make_kit refused (see above)"
KIT="$OUT/$BOARD-$VERSION"
grep -q "\"build\": *\"$BUILD\"" "$KIT/kit.json" || die "kit.json does not name build $BUILD"

# 5. onto the bench
say "installing on $BENCH"
ssh "$BENCH" "mkdir -p ~/Desktop/test-kit && rm -rf ~/Desktop/test-kit/*"
scp -rq "$KIT" "$BENCH:Desktop/test-kit/"
ssh "$BENCH" "cp -r ~/Desktop/test-kit/$BOARD-$VERSION ~/.local/share/jayecu/jayecu\\ Studio/firmware/ &&
              grep -q '\"build\": *\"$BUILD\"' ~/.local/share/jayecu/jayecu\\ Studio/firmware/$BOARD-$VERSION/kit.json" \
    || die "the kit did not land on $BENCH intact"

# 6. the studio, if asked and closed
if [ "${STUDIO:-0}" = "1" ]; then
    if ssh "$BENCH" "pgrep -f 'mount_jayecu.*/usr/bin/studio' >/dev/null"; then
        say "the studio is OPEN on $BENCH — close it and run again with STUDIO=1 (the kit is installed already)"
    else
        make -s studio-appimage > /tmp/bench_beta_studio.log 2>&1 || { tail -20 /tmp/bench_beta_studio.log >&2; die "studio build failed"; }
        apps/studio-jf/tools/bench_deploy.sh "$(ls -t apps/studio-jf/build/jayecu-studio-*-x86_64.AppImage | head -1)" "$BENCH"
    fi
fi

say "done — $BOARD $VERSION, build $BUILD, installed on $BENCH; connect and accept the update"
