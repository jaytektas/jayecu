#!/usr/bin/env bash
# Package a built studio as the Linux release file: jayecu-studio-<version>-x86_64.AppImage.
#
#   apps/studio-jf/tools/make_appimage.sh [build-dir]     (default apps/studio-jf/build)
#
# The AppImage is what a Linux user runs, and the only form that can update itself: the updater swaps
# the file named by $APPIMAGE (JFramework's JSelfInstaller), and the update check picks the release asset
# ending in -x86_64.AppImage (JRelease). So the name here is the name a release must carry.
#
# Inside, the studio sits in usr/bin with everything it ships beside it — the same list CMakeLists.txt
# copies into the build and installer/studio.iss puts beside studio.exe. resources::path looks beside
# the executable first, so nothing inside needs to know it is in an AppImage.
#
# Needs appimagetool (github.com/AppImage/appimagetool) on PATH.

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$HERE/build}"
VERSION="$(sed -n 's/^#define STUDIO_VERSION_STRING *"\([^"]*\)".*/\1/p' "$BUILD/generated/StudioVersion.cpp")"
[ -n "$VERSION" ] || { echo "make_appimage: no version in $BUILD/generated/StudioVersion.cpp" >&2; exit 1; }
OUT="$BUILD/jayecu-studio-$VERSION-x86_64.AppImage"

command -v appimagetool >/dev/null || { echo "make_appimage: appimagetool is not on PATH" >&2; exit 1; }

APPDIR="$BUILD/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"

for f in studio studio.style studio.common.style jaytek-logo.png jaytek-landing.png UbuntuSans.ttf UbuntuSans-LICENCE.txt; do
    [ -e "$BUILD/$f" ] || { echo "make_appimage: $BUILD/$f is missing — build the studio first" >&2; exit 1; }
    cp "$BUILD/$f" "$APPDIR/usr/bin/"
done

# The licence the studio and the firmware kit are conveyed under, the firmware's additional permission,
# and what third-party code both contain. The GPL asks that every recipient gets a copy.
for f in LICENSE LICENSE.exception THIRD-PARTY.md; do cp "$HERE/../../$f" "$APPDIR/usr/bin/"; done

# The shipped sensor calibration presets (Apply Preset on a calibration curve lists them).
cp -r "$HERE/calibrations" "$APPDIR/usr/bin/calibrations"

# The user manual travels beside the studio (Help ▸ User Manual opens usr/bin/manual/index.html).
MANUAL="$HERE/../../manual/site"
if [ -f "$MANUAL/index.html" ]; then cp -r "$MANUAL" "$APPDIR/usr/bin/manual"
else echo "make_appimage: no built manual at $MANUAL -- run make manual; packaging without it" >&2; fi

# The ECU firmware kits, one per board, beside the studio as firmware/<kit>: the update it offers and what
# recovery installs on a board with no firmware — of ANY board, which is why every board's kit ships.
# KITS is the folder `make ship-kits` gathers them into.
KITS="${KITS:-$HERE/../../firmware/build/ship-kits}"
n=0
for k in "$KITS"/*/; do
    [ -f "$k/kit.json" ] || continue
    mkdir -p "$APPDIR/usr/bin/firmware"; cp -r "${k%/}" "$APPDIR/usr/bin/firmware/"; n=$((n+1))
done
[ "$n" -gt 0 ] || echo "make_appimage: no firmware kits in $KITS -- run make ship-kits; packaging without any" >&2

# The CAN device templates, made by codegen.
if compgen -G "$HERE/../../shared/can_templates/*.json" >/dev/null; then
    mkdir -p "$APPDIR/usr/bin/can_templates"; cp "$HERE"/../../shared/can_templates/*.json "$APPDIR/usr/bin/can_templates/"
fi

cp "$HERE/assets/studio.png" "$APPDIR/jayecu-studio.png"

cat > "$APPDIR/jayecu-studio.desktop" <<EOF
[Desktop Entry]
Name=jayECU Studio
Comment=jayECU tuning studio
Exec=studio
Icon=jayecu-studio
Terminal=false
Type=Application
Categories=Utility;
StartupWMClass=studio
EOF

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
exec "$(dirname "$(readlink -f "$0")")/usr/bin/studio" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# BUILT BESIDE, THEN RENAMED OVER. Writing straight onto $OUT fails ("sfs_mksquashfs error") whenever
# the previous AppImage is running — a file being executed cannot be opened for writing — and the last
# build is exactly the one a developer has open. A rename replaces the directory entry instead, which
# Linux allows under a running program: it keeps its old copy until it exits. Output is kept, not
# discarded, so a failure says why.
TMP="$OUT.building"
rm -f "$TMP"
ARCH=x86_64 appimagetool --no-appstream "$APPDIR" "$TMP" > "$BUILD/appimagetool.log" 2>&1 \
    || { echo "make_appimage: appimagetool failed — see $BUILD/appimagetool.log" >&2; tail -5 "$BUILD/appimagetool.log" >&2; exit 1; }
mv -f "$TMP" "$OUT"
echo "  appimage: $OUT"
