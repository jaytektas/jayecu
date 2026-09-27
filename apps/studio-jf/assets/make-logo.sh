#!/usr/bin/env bash
# Rescale the About logo to the size the dialog DRAWS it at.
#
# WHY THIS EXISTS. A texture handed to the GPU at 557x400 and drawn at 292x210 is minified by the
# rasteriser: bilinear, no mipmaps, one sample per output pixel out of a 3.6-pixel footprint. The gear
# teeth and the tagline come out soft — which is exactly what a logo is judged on. Resampled here
# instead, once, with a filter that looks at every source pixel, the dialog blits it 1:1 and it is as
# sharp as the artwork.
#
# Run this when jaytek-logo-source.png changes. Needs ImageMagick; the result is committed so an
# ordinary build does not.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/jaytek-logo-source.png"
out="$here/jaytek-logo.png"
# THE SIZE IT IS DRAWN AT, because the framework's message dialog blits the picture at its native size
# — one texel per pixel, which is what keeps a logo looking like the artwork. So this is not a bounding
# box to fit inside; it is the answer.
box_w=400
box_h=400
command -v convert >/dev/null || { echo "make-logo: needs ImageMagick" >&2; exit 1; }
[ -f "$src" ] || { echo "make-logo: no artwork at $src" >&2; exit 1; }
convert "$src" -filter Lanczos -resize "${box_w}x${box_h}>" -depth 8 PNG32:"$out"

# THE SPLASH IS THE SAME ARTWORK, BIGGER. It is the whole window rather than a panel in a dialog, so it
# gets the size the mark is meant to be read at; both are resampled here for the same reason, which is
# that the renderer blits an image at its native size and does no filtering worth having.
# THE LANDING GETS THE ARTWORK AT ITS OWN SIZE. It is the whole centre of an empty studio, not a panel
# in a dialog, so there is nothing to shrink it for — and the renderer blits at native size, so "full
# size" means shipping it full size rather than asking anything to scale it up.
convert "$src" -depth 8 PNG32:"$here/jaytek-landing.png"

echo "make-logo: $(identify -format '%wx%h' "$src") -> about $(identify -format '%wx%h' "$out"), landing $(identify -format '%wx%h' "$here/jaytek-landing.png")"
