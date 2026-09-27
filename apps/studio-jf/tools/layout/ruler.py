"""The application's own font, measurable from Python.

The page builders in this directory lay out captions in pixels and had no way to ask how wide a caption
IS — so a name that did not fit its slot was authored anyway and guillotined at paint time (a canvas
widget clips its content to its own rect, and LabelWidget draws glyphs with no maxWidth, so there is no
ellipsis and no complaint). check_all.py could measure boxes against boxes and never text against its box.

This is the missing ruler. tools/textwidth — built on demand — loads the same face at the same pixel size
the studio does and hands over the atlas metrics plus the whole glyph advance table; `width()` below is
JTextHelper::measureWidth summing those advances, and `lines()` is LabelWidget::layoutLines.

The font is the PROFILE'S, read from ~/.config/jayecu-studio/settings.json, because that is what decides
the answer: the application face and size are settings (ui.font / ui.fontPx), every authored caption's
scale is its size divided by that atlas's line height, and an unset property reads its per-type Widget
Default (`editor.wdefProps.*`) — where `padding=0` on labels is worth a whole word on a 360px caption.
A constant here would predict a render nobody has.

    from ruler import ruler
    r = ruler()
    r.width('Cranking Threshold')             # px, at the label font's own size
    r.width('Cranking Threshold', px=15)      # …at an explicit size
"""
import json, os, subprocess

HERE     = os.path.dirname(os.path.abspath(__file__))
STUDIO   = os.path.abspath(os.path.join(HERE, '..', '..'))      # apps/studio-jf
TOOL_SRC = os.path.join(STUDIO, 'tools', 'textwidth.cpp')
TOOL     = os.path.join(STUDIO, 'tools', 'textwidth')
SETTINGS = os.path.expanduser('~/.config/jayecu-studio/settings.json')

# main.cpp: the default application face and size, used when the profile names neither.
UBUNTU_SANS      = '/usr/share/fonts/truetype/ubuntu/UbuntuSans[wdth,wght].ttf'
DEFAULT_PX       = 13
DEFAULT_LABEL_PX = 16.0   # LabelWidget::render's fallback when a font spec carries no size
LABEL_PAD        = 3.0    # LabelWidget::kTextPad
TITLE_INSET      = 16.0   # CanvasWidget::drawTitleBar — 8px each side


def settings():
    try:
        return json.load(open(SETTINGS))
    except Exception:
        return {}


def app_font():
    """(facePath, px) exactly as main.cpp resolves it: the profile's override, else Ubuntu Sans."""
    s = settings()
    px = int(s.get('ui.fontPx', DEFAULT_PX))
    face = s.get('ui.font') or ''
    if not face:
        face = UBUNTU_SANS if os.path.exists(UBUNTU_SANS) else '-'
    return face, px


def widget_defaults():
    """The profile's per-type Widget Defaults — WidgetRegistry::resolveElement's middle rung.

    A property the element does not carry reads its type-wide default LIVE from Preferences, so the answer
    to "does this caption fit" genuinely depends on this profile.
    """
    out = {}
    for k, v in settings().items():
        if k.startswith('editor.wdefProps.'):
            out[k[len('editor.wdefProps.'):]] = dict(
                kv.split('=', 1) for kv in v.split('\x1f') if '=' in kv)
    return out


def font_px(spec, dflt=DEFAULT_LABEL_PX):
    """fontSpecPx: size is field 1 of "family|size|bold|italic"; absent or <= 0 → the default."""
    parts = spec.split('|')
    if len(parts) < 2:
        return dflt
    try:
        v = float(parts[1])
    except ValueError:
        return dflt
    return v if v > 0 else dflt


def _build_tool():
    if os.path.exists(TOOL) and os.path.getmtime(TOOL) > os.path.getmtime(TOOL_SRC):
        return
    sdk = os.path.expanduser('~/jframework-sdk')
    subprocess.run(['g++', '-std=c++20', '-O1', f'-I{sdk}/include', TOOL_SRC, '-o', TOOL,
                    f'{sdk}/lib/libj_platform.a', '/usr/lib/x86_64-linux-gnu/libvulkan.so',
                    '-lxcb', '-lxcb-keysyms', '-lxcb-sync', '-ldbus-1', '-lrt', '-latspi',
                    '-lglib-2.0', '-lpthread'], check=True)


class Ruler:
    # JTextHelper::_substitute — what an absent codepoint is retried as before the fallback advance.
    SUBS = {0x2013: '-', 0x2014: '-', 0x2018: "'", 0x2019: "'",
            0x201C: '"', 0x201D: '"', 0x2026: '.', 0x00A0: ' '}

    def __init__(self, face=None, px=None):
        if face is None or px is None:
            face, px = app_font()
        self.face, self.px = face, px
        _build_tool()
        out = subprocess.run([TOOL, face, str(px)], check=True,
                             capture_output=True, text=True).stdout
        self.adv, self.line_h, self.fallback = {}, 0.0, 0.0
        for ln in out.splitlines():
            f = ln.split()
            if   f[0] == 'g':          self.adv[int(f[1])] = float(f[2])
            elif f[0] == 'lineHeight': self.line_h  = float(f[1])
            elif f[0] == 'fallback':   self.fallback = float(f[1])
        if not self.adv or self.line_h <= 0:
            raise SystemExit('ruler: textwidth returned no metrics')

    def base_width(self, text):
        """JTextHelper::measureWidth — the width in BASE-atlas pixels, before any font-spec scale."""
        w = 0.0
        for ch in text:
            a = self.adv.get(ord(ch))
            if a is None:
                a = self.adv.get(ord(self.SUBS.get(ord(ch), '?')))
            w += a if a is not None else self.fallback
        return w

    def width(self, text, px=DEFAULT_LABEL_PX, spec=None):
        """The width a caption is DRAWN at: base width scaled by its font size over the atlas's line
        height, which is LabelWidget's own `scale = fontPx / lh0`."""
        if spec is not None:
            px = font_px(spec)
        return self.base_width(text) * (px / self.line_h)

    def lines(self, text, wrap, px=DEFAULT_LABEL_PX, avail=0.0, spec=None):
        """LabelWidget::layoutLines. Hard '\\n' breaks first, then greedy whole words, then inside one."""
        if spec is not None:
            px = font_px(spec)
        scale = px / self.line_h
        fits = lambda s: self.base_width(s) * scale <= avail
        out = []
        for para in text.split('\n'):
            if not wrap or avail <= 0.0 or not para or fits(para):
                out.append(para)
                continue
            i = 0
            while i < len(para):
                fit, j = i, i
                while j < len(para):
                    sp = para.find(' ', j)
                    cand = len(para) if sp < 0 else sp
                    if not fits(para[i:cand]):
                        break
                    fit = cand
                    if sp < 0:
                        break
                    j = sp + 1
                if fit == i:                                 # not even one word fits — break inside it
                    k = i
                    while k < len(para):
                        if k > i and not fits(para[i:k + 1]):
                            break
                        k += 1
                    fit = max(k, i + 1)
                out.append(para[i:fit])
                i = fit
                if i < len(para) and para[i] == ' ':
                    i += 1
        return out


_R = None


def ruler():
    """The one ruler, built once per process (the atlas costs a subprocess and a font rasterise)."""
    global _R
    if _R is None:
        _R = Ruler()
    return _R
