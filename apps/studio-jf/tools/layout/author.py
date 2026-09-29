"""Page authoring helpers for dashboard.gui.

The studio has no way to say "lay this page out like THIS" other than placing widgets, so these are the
pen: one function per thing a page is made of (a titled panel, a labelled field, a table with its axis
setup, a live readout strip), emitting the same widget JSON the editor writes. The layout decisions live
in the page definitions that call these, not here.

Conventions taken from the pages already authored by hand in this document (Half Bridge A, ETB Feed back):
a field is a Left-aligned 14px label with its control 26px below it; panels carry their title in
labelText and lay their children out in the panel's own content box.
"""
import json, uuid

from ruler import ruler

# THE PAGE IS THE VIEWPORT'S CONTENT BOX, and nothing else. Main's viewport is 1280x700 inside a
# 1600x800 canvas, and a page that declares any other size is either scaled (and unreadable) or spilling
# past the frame — which is what every page here was doing: authored to 1280x720, declared 1000x614.
CANVAS_W, CANVAS_H = 1280, 700
# THE NOTE STARTS AT THE TOP. This used to leave a 30px band clear above it for a title bar drawn OVER
# the page — and on screen that is simply a 30px hole between the window's title and the first line of
# text, on all 104 pages. The band is gone: the note sits two pixels under the title bar, and every page
# gets those thirty pixels back as content. On the one page packed to its edges — the sensors
# switchboard, 134 inputs — that is the difference between shaving the group headings and not.
BAND = 2
TOP  = 30                   # BAND(2) + 2 + a one-line note(24) + 2 — see head() and the overrun repair
MARGIN = 10                 # the gutter every page leaves at its left and right edge
# WHERE A PAGE NOTE MAY END. The page is 1280 wide but is drawn at the interface scale, so a note running
# to the page edge ended beyond what is visible and its last words ran off. Ending here, it wraps inside
# the page on any usual window, and ~110-character lines read better than 150 (apply_note_wrap.py).
NOTE_RIGHT = 1000
# …and where any other paragraph placed on the page itself may end: the explanation columns beside the
# panels. They ran to 1260-1270 and lost their last words the same way.
TEXT_RIGHT = 1190
FONT_LBL   = '|16|0|0'      # family|size|bold|italic — the label font every authored page uses
FONT_TITLE = '|18|1|0'
FONT_SMALL = '|15|0|0'
ROW   = 30                  # one settings row: label left, control right, on one line
# A PANEL'S CHILDREN LIVE UNDER ITS TITLE BAR. PanelWidget lays them out in `h - titleBarH() - 4`, and
# titleBarH() is the text line height plus 6 — about 22 at the 14px the pages use. A panel sized as
# "header + n rows" without counting that is short by the bar, and the last row is clipped by a few
# pixels: visible, and invisible to a checker that only compared against the panel's own height.
PANEL_TITLE = 22


def panel_h(rows, row_h, top=10, bottom=6):
    """The height a panel needs to hold `rows` rows of `row_h` WITHOUT clipping the last one.

    Every builder here used to write "34 + 25 * rows" or similar, which is the title bar guessed at and
    guessed short: PanelWidget gives its children `h - titleBarH() - 4`, so the last row lost a few pixels
    on every panel on every page. One formula now, and it is derived from the widget rather than eyeballed.
    """
    return PANEL_TITLE + 4 + top + rows * row_h + bottom
# THE APPLICATION'S OWN CONTROL SIZES, mirrored from JStyle (src/../JStyle.h: labelHeight 20,
# controlHeight 30, buttonHeight 32, checkHeight 22). These installers are offline Python emitting JSON,
# so they cannot ASK the style the way the C++ ini converter now does — but they can agree with it
# instead of inventing their own numbers. They used to say 24/25 with checkboxes at 20 (or 16 when a row
# was tight), which is why one document held checkboxes in three different sizes and its rows never
# matched a converted page's.
#
# If JStyle's metrics change, change these to match and re-run the installers. That is the whole cost of
# the duplication, and it is cheaper than a page that disagrees with the app it is drawn in.
LBL_H, CTL_H = 20, 30
CHECK_H  = 22    # JStyle::checkHeight — a tick box is its own size, never squeezed to fit a row
# JStyle::itemPadding — the gap JCheckBox leaves between its box and its own caption. A studio page pairs a
# caption-less box with a separate label (the label carries the link and the greying), so each builder
# placed that label itself, at 0, 2, 4 or 6px from the box across 1,372 of them: the text touched the box
# on the sensor group pages and nearly did everywhere else. Every label beside a box starts here instead.
CHECK_GAP = 8
BUTTON_H = 32    # JStyle::buttonHeight

# Colours picked from the pages already in the document, so new pages match what is there.
C_DIM   = '#8a8f98'
C_RED   = '#ff453a'
C_AMBER = '#ff8c28'
C_GREEN = '#30d158'
C_BLUE  = '#0a84ff'


# ---- table geometry -----------------------------------------------------------------------------
_META_TABLES = None


def table_dims(path):
    """(cols, rows, digits) for a config table, from the meta — None if it names none.

    THE SHIPPED AXIS LENGTHS, not the allocation, and the docstring used to claim the opposite while the
    code did this. Both are defensible and the anchors settled it: a table page's grid stretches into
    whatever the window gives it (Surface::screenRectOf) and scrolls when there is not enough, so the
    authored size only has to be a sensible FLOOR. Sizing every page to the allocation instead — target
    lambda may grow to 32x32, which is about 2100px — would set that floor beyond most screens and make
    a page scroll that had no need to.

    So a tune whose axis is longer than the shipped one scrolls, which is the honest answer for a table
    with more columns than the window can show at a readable cell width."""
    global _META_TABLES
    if _META_TABLES is None:
        import json as _json
        META = __import__('paths').META
        m = _json.JSONDecoder().raw_decode(open(META, encoding='utf-8', errors='replace').read(), 0)[0]
        _META_TABLES = {}

        def walk(o, path=''):
            if isinstance(o, dict):
                # A ONE-ROW CURVE HAS NO `rows`, and requiring one made every curve in the document
                # invisible to this: table_dims answered None, table_box fell back to (1260, 400), and
                # the page had no measurement to author against. Small-Pulse Correction is 32 bins at
                # three decimals — it needs well over a full sheet — and it was drawn in 164px, because
                # nothing here could say otherwise. A curve is a table one row tall; say so.
                if o.get('type') == 'table' and 'cols' in o:
                    # THE SIZE IT CAN GROW TO, not the size it ships at. A resizable table is allocated
                    # for its maximum in the config — the gear correction ships 8x8 and holds 16x16 —
                    # and every page built from the shipped number boxed it at 8x8 for ever. Grow the
                    # table and the grid scrolls inside a box a quarter the size of the empty page it
                    # is the only thing on. The meta has carried cols_max/rows_max all along; this is
                    # the line that was reading past them.
                    # DIGITS TOO: the runtime sizes a cell from the widest number it can PRINT, which is
                    # "-8888" plus this table's decimals (TableWidget::tableGeom). Sizing every box for
                    # one decimal made a 2-decimal map's columns a character too narrow each — twenty-one
                    # of them on Target Lambda, which is a table clipped off the right of its own page.
                    _META_TABLES[path.lstrip('/').replace('/', '.')] = (
                        int(o.get('cols_max') or o['cols']),
                        int(o.get('rows_max') or o.get('rows') or 1),   # a curve is one row
                        int(o.get('digits') or 0),
                        int(o['cols']), int(o.get('rows') or 1))     # …and the size it ships at
                for k, v in o.items():
                    walk(v, path + '/' + k)
        walk(m.get('config') or {})
    return _META_TABLES.get(path)


def table_fmt(path, fallback='%.2f'):
    """The printf format a table's own VALUE should be read at — its declared decimals.

    A readout bound to a table path shows what the table is putting out (Cache::solveTable interpolates
    it against its live axes). Printing that at some other precision than the cells above it invites the
    obvious question of which one is rounded, so it takes the decimals the meta gives the table."""
    dims = table_dims(path)
    return f'%.{dims[2]}f' if dims else fallback


def table_box_shipped(path, fallback=(1260, 400)):
    """The pixels the table needs AS IT SHIPS — the floor under any cap a caller applies.

    table_box() answers with the size it can grow to, which is what a page should give it. A page that
    then caps that (because a page cannot be wider than a page) must not cap it below THIS, or a map
    that fits today starts scrolling for columns nobody has added: Target Lambda ships at twenty-one
    columns needing 1378 px and can grow to 2065, and capping at the canvas took it to 1260."""
    dims = table_dims(path)
    if not dims or len(dims) < 5:
        return fallback
    return _box_for(dims[3], dims[4], dims[2], fallback)


def table_box(path, fallback=(1260, 400)):
    """The pixels a table needs to show every cell it can hold, using the app's own text metrics.

    Mirrors TableWidget::tableGeom: a cell is the widest number it can print plus padding, a row is the
    line height plus padding, and the grid carries a bin strip down the left and one across the top with
    an axis-name bar above that. Authoring the box at this size is what lets the page underneath sit
    directly beneath the grid instead of a screen away from it."""
    dims = table_dims(path)
    if not dims:
        return fallback
    return _box_for(dims[0], dims[1], dims[2], fallback)


def _box_for(cols, rows, digits, fallback):
    """The pixels a grid of this shape needs. Shared by table_box (what it can grow to) and
    table_box_shipped (what it needs today), because the arithmetic is the runtime's and there must be
    exactly one copy of it."""
    r = ruler()
    # base_width / line_h, NOT width(px): a grid draws at the atlas's own metrics (TableGeom's fscale is
    # 1 for an unset font), while width() scales a caption to a font size. Measured against the runtime:
    # a cell of "-8888" comes back 46.98 there and 46.9 here.
    # ROUND UP, AND ONLY ONCE. A cell is 54.89 px wide here; truncating it to 54 loses nine tenths of a
    # pixel per COLUMN, and a 21-column ignition map loses 19 px — which the grid answers with a
    # horizontal scroll bar inside a window that fits perfectly. Keep the fractions, total them, then
    # ceil the one number that has to be an integer.
    import math
    # THE SAME SAMPLE THE RUNTIME MEASURES (TableWidget::tableGeom): "-8888", then a point and one '8'
    # per decimal this table prints. A fixed '-8888.8' here is right only for the 1-decimal tables.
    sample = '-8888' + (('.' + '8' * digits) if digits > 0 else '')
    cell_w = r.base_width(sample) + 10           # a cell is its widest number plus the grid's padding
    row_h  = r.line_h + 6
    hdr_w  = r.base_width(sample) + 4            # the y-bin strip down the left
    # NO CAP. This used to stop at 1260 — the widest a 1280 canvas can hold — which quietly turned "the
    # size this table needs" into "the size the page happens to be", and a map wider than that was
    # clipped with no way to tell from here that it had been. Target Lambda is 21 columns at two
    # decimals: 1378, capped to 1260, and the last column and a half simply gone. The caller decides
    # what to do with a number bigger than the sheet; a page can be authored wider and the window then
    # opens at what it needs (Surface::contentExtent_).
    return (math.ceil(hdr_w + cols * cell_w) + 10,
            math.ceil(row_h * 2 + rows * row_h) + 12)   # axis-name bar, the x-bin strip, then the rows


class Page:
    """One node page: widgets, and the ids/uids that keep them apart."""

    def __init__(self, w=CANVAS_W, h=CANVAS_H, title=''):
        self.w, self.h, self.title = w, h, title
        self.widgets, self._id, self._grp = [], 1, 1
        self.has_note = False        # set by head(); decides whether content starts at TOP or at BAND
        # THE ELEMENT THIS PAGE IS ABOUT ("sensors.sensor[clt]", "outputs.output[4]"), which is what every
        # "[*]" on it resolves to. One body serves a whole array; the element used to ride on the VIEWPORT
        # that showed the page, and pages open in windows now with no viewport in the document at all.
        # Set it on a per-element page and the bindings resolve however the page is reached.
        self.element = ''

    def _new(self, type_, x, y, w, h, props, group=0):
        wid = {'id': self._id, 'uid': str(uuid.uuid4()), 'type': type_,
               'x': x, 'y': y, 'w': w, 'h': h, 'groupId': group, 'props': props}
        self._id += 1
        return wid

    def group(self):
        self._grp += 1
        return self._grp

    def add(self, wid, into=None):
        (into['kids'] if into else self.widgets).append(wid)
        return wid

    # ---- pieces ---------------------------------------------------------------------------------
    def heading(self, x, y, text, w=600):
        return self.add(self._new('label', x, y, w, 30,
                                  {'labelText': text, 'align': 'Left', 'fontName': FONT_TITLE}))

    def title_switch(self, x, y, text, enable_path, w=600):
        """The page title with the feature's own switch in front of it — one dot that says whether the
        thing this page describes is running at all, in the place the eye starts."""
        g = self.group()
        self.add(self._new('checkbox', x, y + 6, CHECK_H, CHECK_H, {'signalName': enable_path}, g))
        self.add(self._new('label', x + CHECK_H + CHECK_GAP, y, w, 30,
                           {'labelText': text, 'align': 'Left', 'fontName': FONT_TITLE}, g))
        return y + 36

    def head(self, note='', enable='', title='', enable_label='Enabled'):
        """The row under the viewport's title bar: what this page is for, and — for a page whose whole
        subject can be switched off — that switch at the right of the same row, which is one place for it
        on every page instead of one per author. Returns where content starts, so no page carries these
        numbers itself."""
        x = 10
        if enable:
            g = self.group()
            self.add(self._new('checkbox', 10, BAND + 1, CHECK_H, CHECK_H, {'signalName': enable}, g))
            self.add(self._new('label', 10 + CHECK_H + CHECK_GAP, BAND + 3, 90, LBL_H,
                               {'labelText': enable_label, 'align': 'Left', 'fontName': FONT_LBL}, g))
            x = 136
        bottom = TOP
        if note:
            self.has_note = True
            # AS WIDE AS THE WORDS, up to the page. A note used to take the full width whatever it said,
            # which made every page claim the whole canvas as its width even when a table and three
            # readouts were all it held — and a window opens at the width its content needs now
            # (Surface::contentExtent_). A note too long for one line still gets the width it needs to
            # wrap into, because that IS what it needs.
            avail = min(self.w - x - 10, NOTE_RIGHT - x)
            n = self.note(x, BAND + 2, note, w=min(avail, int(ruler().width(note)) + 12))
            # WHERE CONTENT STARTS IS WHERE THE NOTE ENDS, not a constant. This returned TOP flat, so a
            # blurb that wrapped to two or three lines ran straight down through the panels beneath it —
            # on nineteen pages, none of which said anything, because a page that trusts this number has
            # no other way to know. One line still lands on TOP exactly as before.
            # +2, which is what TOP already encodes: BAND(2) + 2 + a one-line note(24) + 2 = 30. So a
            # single-line blurb lands exactly where it always did and no page moves; only a note that
            # genuinely wrapped pushes content down, which is the whole point.
            bottom = max(TOP, n['y'] + n['h'] + 2)
        return bottom

    @staticmethod
    def wrap_h(text, w, font_px=13, at_px=None):
        """How tall a wrapped label has to be to show ALL of `text` at width `w`.

        Every hand-placed paragraph on a page was a guess at this, and the guesses were low — the checker
        measures the BOX, so a label whose text overflows it passes every check and clips on screen. One
        estimate, used by note() and by any panel label long enough to wrap.

        It is no longer an estimate. It used to divide the character count by a nominal character width
        (font_px * 0.46), which is right on average and therefore wrong on any particular sentence: it
        read "Frequency 0 derives the resonance from the bore…" as four lines when the words break into
        five, and the fifth was drawn outside the box and clipped. ruler.py breaks the text with the
        app's own font and LabelWidget's own greedy wrap, so the line count is the one that will be
        drawn.

        `at_px` is the size the text is DRAWN at when that differs from the row pitch the caller wants
        (note() draws FONT_SMALL but keeps the 13px pitch its pages are laid out to), so a page whose
        line count was already right does not move.
        """
        from ruler import ruler, LABEL_PAD
        # Measured at the built-in 3px inset, not this profile's Widget Default of 0: a page is authored
        # once and opened on any profile, so the room to author against is the smaller of the two.
        lines = ruler().lines(text, True, at_px or font_px, w - 2 * LABEL_PAD)
        return len(lines) * (font_px + 3) + 8

    @staticmethod
    def wrapped_h(text, w, font=None):
        """Exactly the height wrapped() will give this text at this width.

        Pages that place something UNDER a note have to know how tall the note is before the note
        exists, and they were each re-deriving it — at the wrong pitch, and sometimes at a different
        width than the note is drawn at (330 reserved for a caption drawn at 324). Both mistakes are the
        same mistake: two answers to one question. There is one now, and it is the one wrapped() asks.
        """
        f = font or FONT_SMALL
        return Page.wrap_h(text, w, int(f.split('|')[1]))

    @staticmethod
    def note_h(text, w=600):
        """…and the same for note(), which draws at FONT_SMALL against the 13px pitch its pages use."""
        return Page.wrap_h(text, w, at_px=int(FONT_SMALL.split('|')[1]))

    def wrapped(self, x, y, w, text, into=None, colour=C_DIM, font=None, align='Left',
                cond='', enable=''):
        """A wrapped paragraph that is AS TALL AS ITS TEXT — for the captions inside panels.

        note() has always done this; these were built by hand with a literal height instead, and the
        literal was a guess. 215 distinct captions were short, the sensor pages' interface note among
        them on all 128 of them — three lines of text in a box sized for two, passing every geometry
        check because the BOX fitted its panel perfectly. Nothing measured the words.
        """
        f = font or FONT_SMALL
        try:
            px = int(f.split('|')[1])
        except Exception:
            px = 13
        if into is None:
            w = min(w, TEXT_RIGHT - x)      # on the page itself: end inside what is visible
        props = {'labelText': text, 'align': align, 'fontName': f, 'fgColor': colour, 'wrap': '1'}
        if cond:   props['condition'] = cond
        if enable: props['enableCondition'] = enable
        return self.add(self._new('label', x, y, w, Page.wrap_h(text, w, px), props), into=into)

    def note(self, x, y, text, w=600, colour=C_DIM, h=0, into=None):
        """A wrapped paragraph under a panel — sized to the text it actually holds.

        The height was hard-coded to 22px, one line, while the label wraps: a three-line note was drawn
        into a one-line box, and what showed was the tail of it starting mid-sentence. Nothing complained,
        because the checker measures the box and the box was small. The height now follows the text, so a
        note either fits or the checker says it does not.

        The estimate is deliberately generous (about six pixels a character at the small font, and 16px a
        line): a note with a spare line under it looks like a note, and one that is a line short reads as
        a sentence someone cut off.
        """
        if not h:
            h = Page.wrap_h(text, w, at_px=int(FONT_SMALL.split('|')[1]))
        # `into` PLACES IT IN A PANEL, and its absence is why every "note under the side panel's
        # readouts" was in fact drawn on the page at x=12 — on top of the table, in panel-local
        # coordinates that meant nothing outside the panel. The call sites all read as though this
        # existed; now it does.
        return self.add(self._new('label', x, y, w, h,
                                  {'labelText': text, 'align': 'Left', 'fontName': FONT_SMALL,
                                   'fgColor': colour, 'wrap': '1'}), into=into)

    def flow(self, x, y, w, h, into=None, gap=0):
        """An invisible box whose children FLOW — left to right, wrapping when the next will not fit.

        layoutChildRect mode 7, which the status-lamp dock already uses: children keep the size they were
        authored at and the row count is a result of the width, not something the page states. That is
        what a column of switchboard panels wants — hand-rolled "start a new column past y=520" walks give
        the same two columns on a phone and on a 4K screen, and the wrap decides for itself.

        `gap` is the gutter BETWEEN children (PanelWidget's layoutGap). A flow packs edge to edge without
        one, which is right for a strip of status lamps and wrong for titled panels: their borders meet and
        the row reads as a single box with lines through it. Say it here rather than padding every child
        with an invisible margin — a child whose box is wider than the child is a width that every geometry
        check downstream then has to second-guess.

        No border, no title, no padding: it is a rule about arrangement, not a thing on the page.
        """
        props = {'labelText': '', 'layoutMode': '7', 'padding': '0', 'borderWidth': '0'}
        if gap: props['layoutGap'] = str(gap)
        b = self._new('panel', x, y, w, h, props)
        b['kids'] = []
        return self.add(b, into=into)

    def panel(self, x, y, w, h, title, enable='', cond='', into=None):
        """A titled box. An `enable` rule greys the WHOLE panel including its children (PanelWidget passes
        its disabled state down), which is how a feature that is switched off says so once instead of on
        every control inside it."""
        props = {'labelText': title, 'layoutMode': '0', 'padding': '0'}
        if enable: props['enableCondition'] = enable
        # `cond` HIDES the panel and everything in it — a sub-tab page that is not the selected one is not
        # on screen at all, rather than greyed. See subtabs().
        if cond: props['condition'] = cond
        p = self._new('panel', x, y, w, h, props)
        p['kids'] = []
        return self.add(p, into=into)

    # ---- which control an enum gets ---------------------------------------------------------------
    # A plain enum is a DROPDOWN. The enum picker is a different instrument: it is a searchable chooser
    # for the fields with hundreds of options and rules about which apply — a signal selector (328
    # channels), a pin picker (the list changes with the interface). Using it for a three-option field
    # like Engine Cycle makes a two-click job out of a one-click one, and shows a "…" button where a
    # dropdown arrow belongs.
    #
    # Decided from the DEFINITION rather than at each call site: a field that gains a picker later stops
    # being a dropdown by itself, and nobody has to remember which is which.
    _META = None

    @staticmethod
    def _meta():
        if Page._META is None:
            from apply_fuel import load_meta
            Page._META = load_meta()
        return Page._META

    # HOW MANY DECIMALS A CHANNEL DESERVES. The telemetry descriptor already answers this: every channel
    # ships `digits`, decided beside the scale it is transmitted at — a frame counter is an integer, a
    # cam angle is stored in tenths of a degree and cannot say more than one decimal, and a lambda ratio
    # wants three. A blanket "%.2f" makes both mistakes at once: "46254220.00" for a counter, and a
    # trailing zero that pretends to a precision the wire never carried.
    @staticmethod
    def chan_digits(channel, fallback=2):
        t = Page._meta().get('telemetry') or {}
        if not isinstance(t, dict):
            t = {x['id']: x for x in t}
        e = t.get(str(channel).strip().lstrip('[$~').rstrip(']'))
        if not isinstance(e, dict): return fallback
        d = e.get('digits')
        return fallback if d is None else int(d)

    @staticmethod
    def chan_fmt(channel, fallback=2):
        """The printf format for a live channel, from its own descriptor."""
        return f'%.{Page.chan_digits(channel, fallback)}f'

    @staticmethod
    def chan_enum_labels(channel):
        """The words a STATE channel can show, or [] for a plain number.

        A readout bound to an enum-valued channel prints the label, not the index — the number means
        nothing to a reader ('idle_state = 3'). So a cell sized for '-8888.8' is not sized for that
        channel at all: 'Not Running' is 101 px where the number is 84, and the last letters are
        guillotined at paint time with nothing to say so."""
        meta = Page._meta()
        t = meta.get('telemetry') or {}
        if not isinstance(t, dict):
            t = {x['id']: x for x in t}
        e = t.get(str(channel).strip().lstrip('[$~').rstrip(']'))
        if not isinstance(e, dict):
            return []
        if e.get('options'):
            return [str(o) for o in e['options']]
        ids = (meta.get('enums') or {}).get(e.get('enum'))
        return [str(o['label']) for o in ids] if ids else []

    @staticmethod
    def chan_value_px(channel, px=20, sample='-8888.8'):
        """How wide this channel's READING can get, measured with the app's own font."""
        from ruler import ruler
        r = ruler()
        labels = Page.chan_enum_labels(channel)
        return int(max([r.width(sample, px)] + [r.width(l, px) for l in labels]))

    @staticmethod
    def _pc_var(name):
        """A host variable ("pc.<name>") — declared in the schema's pc_vars, carried in the meta."""
        for v in Page._meta().get('pcVars', []):
            if v.get('name') == name: return v
        return None

    @staticmethod
    def _strip_subscripts(path):
        """Drop every "[...]" subscript, counting depth.

        A regex for "[^]]*" cannot do this: a COMPUTED subscript nests, and
        "outputs.output[@[#pc.output_sel]].kind" lost only its inner half, leaving a stray bracket and a
        path that matched nothing. Everything asked through _field_def then fell back to its no-meta
        default — which is why every enum on a selector-indexed page was authored as the generic picker
        instead of the dropdown its two options deserve, and why a 0..1 field there was never spotted as
        a tick box. Silent, because a fallback is not an error.
        """
        out, depth = [], 0
        for ch in path:
            if ch == '[':
                depth += 1
            elif ch == ']':
                depth = max(0, depth - 1)
            elif depth == 0:
                out.append(ch)
        return ''.join(out)

    @staticmethod
    def _field_def(path):
        """The definition of a config path — scalar, or a field of an array-of-structs element."""
        import re
        if path.startswith('pc.'):
            return Page._pc_var(path[3:])
        m = Page._meta()['config']
        p = Page._strip_subscripts(path)             # "sensors.sensor[*].type" -> "sensors.sensor.type"
        parts = p.split('.')
        for i in range(len(parts) - 1, 0, -1):
            mod, rest = '.'.join(parts[:i]), parts[i:]
            node = m
            for seg in mod.split('.'):
                node = node.get(seg) if isinstance(node, dict) else None
                if node is None: break
            if not isinstance(node, dict): continue
            if len(rest) == 1 and isinstance(node.get(rest[0]), dict):
                return node[rest[0]]
            if isinstance(node.get('fields'), dict):
                if rest[-1] in node['fields']:
                    return node['fields'][rest[-1]]
                # A NESTED ARRAY inside an element: an output slot's cand[0].role lives under the slot's
                # own `arrays`, one level further down. Without this the walk stopped at the slot, found
                # no field called "role", and every candidate row was authored as the generic picker —
                # next to a `kind` dropdown two panels above it, for a field of three fixed options.
                if len(rest) >= 2 and isinstance(node.get('arrays'), dict):
                    sub = node['arrays'].get(rest[-2])
                    if isinstance(sub, dict) and isinstance(sub.get('fields'), dict) \
                            and rest[-1] in sub['fields']:
                        return sub['fields'][rest[-1]]
                # A BIT GROUP inside a packed field: "…diag_severity.raw_min" is two bits with its own
                # option list, and it is as much a plain enum as any scalar.
                if len(rest) >= 2 and rest[-2] in node['fields']:
                    bits = node['fields'][rest[-2]].get('bits')
                    if isinstance(bits, dict) and rest[-1] in bits:
                        return bits[rest[-1]]
        return None

    @staticmethod
    def enum_control(path):
        """'combobox' for a plain enum, 'enum' for a picker (signal selector / interface-gated pin list)."""
        try:
            d = Page._field_def(path)
        except Exception:
            return 'enum'                      # no meta to consult: leave it as authored
        if not isinstance(d, dict):            return 'enum'
        if d.get('kind') == 'signal':          return 'enum'
        # A TABLE SELECTOR is a picker for the same reason a signal selector is: the list is every
        # table the ECU has, which is not a drop-down. It DRAWS as a combo box (EnumPickerWidget) —
        # what differs is the dialog behind it, not the control.
        if d.get('kind') == 'table_ref':       return 'enum'
        if 'picker' in d or 'picker_sets' in d: return 'enum'
        return 'combobox' if d.get('options') else 'enum'

    @staticmethod
    def is_flag(path):
        """Is this field a YES/NO? An integer field whose range is 0..1 and which offers no options.

        The meta already says so — every "Enable …" in the schema is declared `min: 0, max: 1` — and a
        yes/no asked as a spin box is a worse control in every way: it takes two clicks to say what a
        tick says in one, it shows "0" where the answer is "no", and its arrows offer a range of exactly
        two. Decided from the DEFINITION rather than at each call site, because the generic builders
        (feature_pages) place fields they have never been told anything about.
        """
        try:
            d = Page._field_def(path)
        except Exception:
            return False
        if not isinstance(d, dict) or d.get('options'):
            return False
        # An integer. A 0..1 FLOAT is a ratio (0-100% as a fraction), not a flag, and stays a number.
        if not str(d.get('datatype', '')).startswith(('U0', 'S0', 'U1', 'S1')):
            return False
        return d.get('min') == 0 and d.get('max') == 1

    @staticmethod
    def widest_value_w(path):
        """How wide the longest number this field can hold prints, in the app's font.

        The box reserves room for the widest value its RANGE can produce rather than the value of the
        moment (JDoubleSpinBox::_widestNumberW) — otherwise the unit would appear and vanish as digits
        come and go, and a box that fits "99 RPM" would lose its unit at 100.
        """
        try:
            d = Page._field_def(path) or {}
        except Exception:
            d = {}
        lo, hi = d.get('min', 0) or 0, d.get('max', 0) or 0
        # TWO DECIMALS, not the field's own precision: the precision belongs to the UNIT and is decided at
        # run time (UnitManager::unitDigits), so it is not a number this side can read — and it CHANGES,
        # because switching km/h to mph switches the format with it. Two is the widest any unit here
        # prints, so the floor is right for all of them and a little generous for the integers.
        return max(ruler().width(f'{float(lo):.2f}'), ruler().width(f'{float(hi):.2f}'))

    @staticmethod
    def control_for(path, kind=''):
        """The control a config field wants: a tick for a flag, a list for an enum, else a number box.

        `kind` is the schema's own kind string where a builder has it ('signal'/'enum'), so this is the
        single answer to "what control does this field get" for every builder that places fields it was
        not hand-told about.
        """
        if Page.is_flag(path):
            return 'checkbox'
        if kind in ('signal', 'enum'):
            return Page.enum_control(path)
        # An EXPRESSION is bytecode, not a number. Without this it fell through to the number box below
        # and got a spin box over byte 0 of a compiled program — which is what `lambda.ltft_learn_when`
        # has been on the installed page all along, the one field that reaches here by this path.
        if kind == 'expression':
            return 'expression'
        return 'configedit'

    def field(self, panel, x, y, label, path, kind='configedit', ctl_w=150, unit='', cond='', enable='',
              lbl_w=0, ranges=''):
        """One settings row: NAME on the left, control on the right, unit after it.

        The label used to sit above its control, which is what the pages authored by hand here did — but
        that is a form-builder's habit, and it halves how many settings fit on a screen. A settings page
        reads down the left edge: the names line up, the controls line up, and the eye finds a row by its
        name rather than by counting boxes.

        `enable` greys the whole row — name, control and unit together — when the setting is not read in
        the current configuration. Greyed, not hidden: the row keeps its place, so a page does not
        reshuffle itself as modes change.
        """
        # THE ROW FITS ITS PANEL. lbl_w used to be a fixed 190, so a 300-wide panel held a 384-wide row and
        # every name in it was clipped mid-word — the caller had to know the panel's width and pass a
        # narrower label, and none of them did. The name takes whatever is left after the control and its
        # unit, which is the only number that can be right for every panel.
        # ROOM FROM HERE TO THE PANEL'S RIGHT EDGE. It was (w - 2x), which is the same number for a row at
        # the left margin and nonsense for a SECOND COLUMN: at x=230 in a 450-wide panel it is -10, so the
        # fit test below always fired and the control was clamped to its 60px minimum. Two columns of the
        # same field then had visibly different boxes — Kp 110px, Kd 60px — for no reason a reader could see.
        avail = (panel['w'] - x - 10) if panel else 400
        if kind == 'enum':
            kind = Page.enum_control(path)     # a plain enum is a dropdown; a picker stays a picker
        # A YES/NO IS A TICK, whatever the call site asked for. Pages were authored field by field and
        # nine boolean settings ended up as spin boxes offering the numbers 0 and 1 — "Enable Overall
        # Correction: 0". The meta knows the field is 0..1, so no page has to remember.
        elif kind == 'configedit' and Page.is_flag(path):
            kind = 'checkbox'
            # …AND A TICK IS 22 WIDE, NOT 150. The label below is stretched to `avail - ctl_w`, which
            # pushes the control slot to the row's right edge so every control in a panel ends in the
            # same column. With ctl_w left at the numeric default, a 22px tick was drawn at the LEFT of
            # a 150px slot — 128px short of that column, which is what made every boolean on every page
            # look adrift of the rows above and below it.
            ctl_w = CHECK_H
        # THE CONTROL PRINTS ITS OWN UNIT. A configedit or a value widget appends the field's unit from the
        # schema, so a label after it says the same thing twice — "2000 cc  cc", "0 RPM  RPM". The unit
        # argument stays in the signature because it still reserves the space the widget's own suffix
        # needs (and reads as documentation at the call site); it just no longer draws a second copy.
        prints_own_unit = kind in ('configedit', 'value')
        # …WHICH MEANS THE BOX HAS TO HOLD BOTH, and the box decides that for itself: JDoubleSpinBox shows
        # its suffix only while `width - steppers - suffix >= widest number`, and DROPS it otherwise so the
        # value is never the thing that gets cut (JDoubleSpinBox.h:330 — 80px showed "661 RPM" as "1 RPM",
        # a wrong number that reads as a right one). Silently dropping it is the right call for the box and
        # the wrong outcome for the page: a reading without its unit is not a reading.
        #
        # So the floor is that same sum, measured here with the app's own font: the steppers are 0.7 of the
        # row height, the suffix is its text plus an 8px gap, the number is the widest its RANGE can print
        # plus 10px of field padding, and 6px of air on top so a box is not sitting on the limit. A flat
        # 100px was fine for "RPM" and not for "pulses/km", which fitted by six pixels.
        if prints_own_unit and unit:
            ctl_w = max(ctl_w, int(CTL_H * 0.7 + ruler().width(unit.strip()) + 8
                                   + Page.widest_value_w(path) + 10 + 6))
        # …and it reserves NOTHING for the unit when the control draws its own: the suffix lives inside the
        # box (which is why the floor above is 100px), so the 66px this used to hold back was empty space
        # taken out of the name.
        uw = 0 if prints_own_unit else (66 if unit else 0)
        # THE NAME IS NOT NEGOTIABLE, AND ITS FLOOR IS THE NAME. This floor was a flat 90px, so a row with
        # a wide control gave its name 90px whatever the name was: "Injector Timing Method" is 150px of
        # text and lost half of itself, and "Manifold Pressure" a third. A control can be narrowed — the
        # clause below does exactly that — and a name cannot be shortened, so the name is measured and the
        # control gives way to it. (+8: a caption is drawn one 3px inset in from each edge of its box.)
        name_w = int(ruler().width(label)) + 8
        if not lbl_w:
            lbl_w = max(name_w, avail - ctl_w - uw - 20)
        else:
            lbl_w = max(lbl_w, name_w)
        # …BUT NEVER PAST THE BOX IT IS IN. Both branches take a max against the MEASURED name, which is
        # the right instinct — a caption should not be cut to fit a column — and both therefore let a long
        # one run out of its panel. A panel hands its children w-4, so this is what "inside" means.
        if panel:
            lbl_w = min(lbl_w, max(20, panel['w'] - x - 4))
        # …and if the CONTROL is what does not fit (a wide enum in a narrow panel), the control gives way
        # too. A row that overflows its panel is clipped, and a clipped enum shows half a signal name.
        if lbl_w + ctl_w + uw + 20 > avail:
            ctl_w = max(60, avail - lbl_w - uw - 20)
        g = self.group()
        lp = {'labelText': label, 'align': 'Left', 'fontName': FONT_LBL}
        cp = {'signalName': path}
        up = {'labelText': unit, 'align': 'Left', 'fontName': FONT_LBL, 'fgColor': C_DIM}
        for d in (lp, cp, up):
            if cond:   d['condition'] = cond
            if enable: d['enableCondition'] = enable
        # `ranges` paints the CONTROL, never the name: a red label would say the FIELD is wrong, where
        # what is wrong is the value someone put in it.
        if ranges: cp['ranges'] = ranges
        if kind == 'checkbox':
            # TICK FIRST, THEN THE NAME — not name-then-tick like a numeric row. A boolean is read as a
            # LIST: Corrections puts its ticks under an "On" column, the Buses grid under "Enabled",
            # every switchboard the same, and the eye runs down that column to see what is on. Putting
            # the tick where a spin box goes buries it at the end of a stretched label, which is what it
            # used to do — and then the answer to "what is switched on" is somewhere different on every
            # row. The name takes whatever is left of the row.
            lw = max(20, avail - CHECK_H - CHECK_GAP - 10)
            if panel:
                lw = min(lw, max(20, panel['w'] - x - CHECK_H - CHECK_GAP - 8))
            self.add(self._new('checkbox', x, y + 3, CHECK_H, CHECK_H, cp, g), into=panel)
            self.add(self._new('label', x + CHECK_H + CHECK_GAP, y + 4, lw, LBL_H, lp, g), into=panel)
            return y + ROW
        self.add(self._new('label', x, y + 4, lbl_w, LBL_H, lp, g), into=panel)
        self.add(self._new(kind, x + lbl_w + 6, y, ctl_w, CTL_H, cp, g), into=panel)
        if unit and not prints_own_unit:
            self.add(self._new('label', x + lbl_w + ctl_w + 14, y + 4, 60, LBL_H, up, g), into=panel)
        return y + ROW

    def check(self, panel, x, y, label, path, cond='', enable=''):
        """One boolean row: the TICK first, then its name — the shape every list of switches uses.

        A boolean is read as a list, not as a settings row: Corrections puts its ticks under an "On"
        column, the CAN Buses grid under "Enabled", the Sensors switchboard the same. The eye runs down
        that column to see what is on, which only works if the column is the left edge. field() draws
        its boolean rows this way too, so the two agree.
        """
        g = self.group()
        cprops = {'signalName': path}
        lprops = {'labelText': label, 'align': 'Left', 'fontName': FONT_LBL}
        if cond: cprops['condition'] = lprops['condition'] = cond
        if enable: cprops['enableCondition'] = lprops['enableCondition'] = enable
        self.add(self._new('checkbox', x, y, CHECK_H, CHECK_H, cprops, g), into=panel)
        self.add(self._new('label', x + CHECK_H + CHECK_GAP, y - 3, 220, LBL_H, lprops, g), into=panel)
        return y + 34

    def table(self, x, y, w, h, path, axis_mode='1', trace=True, into=None, enable='',
              anchor_x='', anchor_y='', cols_sizing='', rows_sizing=''):
        """A grid. `anchor_x`/`anchor_y` say what it does with room the page was not authored for —
        'both' stretches, 'right'/'bottom' follows the far edge; empty stays put.

        `cols_sizing`/`rows_sizing` set the cell sizing, and they are SEPARATE because the two axes do
        not want the same answer: '2' (Stretch) spreads the columns into extra width rather than
        leaving a gap beside them, while the same setting on the rows makes a four-row table's cells a
        quarter of the page tall. '3' is Fixed — the natural size of the number. Left empty, the
        property is not written at all and the table follows the reader's own Preferences ▸ Globals,
        which is the right answer for anything this page has no opinion about."""
        props = {'signalName': path, 'axisMode': axis_mode, 'displayUnit': 'Auto',
                 'displayUnitX': 'Auto', 'displayUnitY': 'Auto', 'displayUnitZ': 'Auto'}
        if enable: props['enableCondition'] = enable
        if trace: props['cellTrace'] = '2'      # the live cell lights up as the engine moves through it
        if anchor_x: props['anchorX'] = anchor_x
        if anchor_y: props['anchorY'] = anchor_y
        if cols_sizing: props['hSectionMode'] = cols_sizing
        if rows_sizing: props['vSectionMode'] = rows_sizing
        return self.add(self._new('table', x, y, w, h, props), into=into)


    def anchor(self, el, x='', y=''):
        """Say what an element does with room the page was not authored for — see Surface::screenRectOf.

        The default ('' / 'left' / 'top') is to stay where it was drawn, which is what every page did
        before anchors existed, so this changes nothing until it is asked for. Slack is measured from the
        AUTHORED width, so at the compact size and below it there is none and the surface scrolls: the
        authored layout is the minimum layout, and there is no second geometry to maintain.
        """
        if x: el['props']['anchorX'] = x
        if y: el['props']['anchorY'] = y
        return el

    def learned_action(self, x, y, w, table, action='apply', label='', into=None):
        """A learned trim's own workflow, on the page that owns it: fold it into the map it corrects, or
        forget it. Both are studio-side operations (model/LearnedOps.h) and both are confirmed; the
        button greys itself out when the surface is all zero and there is nothing to do."""
        return self.add(self._new('learnedaction', x, y, w, 26,
                                  {'table': table, 'action': action,
                                   'labelText': label or ('Reset Learned Values' if action == 'reset'
                                                          else 'Apply to Base Table')}), into=into)

    def curve(self, x, y, w, h, path, into=None):
        return self.add(self._new('curve', x, y, w, h,
                                  {'signalName': path, 'displayUnit': 'Auto',
                                   'axisUnit': 'Auto', 'valueUnit': 'Auto'}), into=into)

    def readout(self, panel, x, y, label, channel, fmt='%.1f', w=110, ranges='', anchor_y=''):
        """A live number with its name under it — the strip along the bottom of a tuning page.

        `anchor_y` moves BOTH halves together (they are one control drawn as two boxes), so a strip under
        a grid that grew into the extra height follows the bottom instead of being left in the middle."""
        g = self.group()
        props = {'signalName': channel, 'format': fmt, 'fontName': '|20|0|0', 'align': 'Center'}
        if ranges: props['ranges'] = ranges
        lprops = {'labelText': label, 'align': 'Center', 'fontName': FONT_SMALL, 'fgColor': C_DIM}
        if anchor_y: props['anchorY'] = anchor_y; lprops['anchorY'] = anchor_y
        self.add(self._new('value', x, y, w, 30, props, g), into=panel)
        self.add(self._new('label', x, y + 30, w, 20, lprops, g), into=panel)

    def bar(self, panel, x, y, w, label, channel, lo, hi, colour=C_BLUE, fmt='%.0f'):
        g = self.group()
        self.add(self._new('label', x, y, w, 20,
                           {'labelText': label, 'align': 'Left', 'fontName': FONT_SMALL,
                            'fgColor': C_DIM}, g), into=panel)
        self.add(self._new('value', x + w - 70, y, 70, 20,
                           {'signalName': channel, 'format': fmt, 'fontName': FONT_SMALL,
                            'align': 'Right'}, g), into=panel)
        self.add(self._new('gauge', x, y + 20, w, 16,
                           {'signalName': channel, 'minValue': str(lo), 'maxValue': str(hi),
                            'fillColor': colour, 'dialColor': '#2a2a2e', 'showPeak': '0',
                            'orientation': 'Horizontal'}, g), into=panel)
        return y + 44

    def switch(self, panel, x, y, label, enable_path, link='', enable='', w=250, pitch=25):
        """One row of a SWITCHBOARD: the thing's on/off flag, and its name as a link to its own page.

        This is the shape the Sensors page already uses, and it is what a category page is for — the
        answer to "what is this ECU doing?" should be one screen of ticks, and the way to the detail
        should be the name of the thing rather than a hunt through the tree. A row with no flag (some
        features have no enable of their own) is just the link.
        """
        # `pitch` is the row spacing, and the control shrinks with it. The default is the comfortable one
        # every other switchboard uses; the Sensors page asks for tighter rows because it lists 122 inputs
        # and a switchboard you have to scroll cannot answer the question it exists to answer.
        # A TICK BOX IS ITS OWN SIZE. This shrank it to 16 when the row pitch was tight, which is how
        # one document ended up holding checkboxes at 20, 18 and 16 — the same control, three sizes, for
        # no reason a reader could see. A tight row is a tight row; the box in it is still a box.
        box = CHECK_H
        g = self.group()
        if enable_path:
            cp = {'signalName': enable_path}
            if enable: cp['enableCondition'] = enable
            self.add(self._new('checkbox', x, y, box, box, cp, g), into=panel)
        lp = {'labelText': label, 'align': 'Left', 'fontName': FONT_LBL}
        if link:   lp['link'] = link            # LabelWidget navigates the tree to this node
        # THE NAME GREYS WITH THE SWITCH. A row is a thing and its state: on, it is bright and its name
        # is the way in; off, it is grey and goes nowhere. Reading the page is then reading the engine —
        # which of these is this car actually running — instead of reading a list of everything possible.
        gate = enable or (f'[#{enable_path}] == 1' if enable_path else '')
        if gate: lp['enableCondition'] = gate
        # A ROW WITH NO FLAG STARTS AT x. The label was always indented by the width of a checkbox,
        # including on the rows that have none — so a flagless link row (a table that lives on its own
        # page, say) ran 30px past the `w` its caller sized for the panel, and check_all caught it as an
        # overflow the caller could not fix without knowing this indent existed.
        lx = x + (box + CHECK_GAP if enable_path else 0)
        self.add(self._new('label', lx, y - 2, w, min(LBL_H, max(15, pitch)), lp, g), into=panel)
        return y + pitch

    def action(self, panel, x, y, w, label, writes, enable='', h=25):
        """A button that works out settings and writes them — see ActionButtonWidget.

        `writes` is a list of (config path, expression) pairs, or a single pair. The expressions are in
        ENGINEERING units (the widget cooks every [#…] before evaluating), so they read as the arithmetic
        they are rather than as counts, and a pair whose value comes out zero or infinite is SKIPPED —
        which is how one button calibrates whichever inputs are live without erasing the ones that
        are not."""
        if writes and isinstance(writes[0], str):
            writes = [writes]
        props = {'labelText': label,
                 'writes': ';'.join(f'{t} = {e}' for t, e in writes)}
        if enable: props['enableCondition'] = enable
        return self.add(self._new('action', x, y, w, h, props), into=panel)

    def subtabs(self, x, y, w, h, var, names):
        """SUB-TABS, out of parts that already exist.

        There is no tab widget, and none is needed: a panel in CARD layout stacks its children and the
        run-mode visibility gate picks which one shows, so a chooser plus one condition per page IS a tab
        strip. The chooser writes a host variable (`pc.<var>`, declared in the schema's pc_vars) —
        "virtual configuration": the studio owns the storage, the ECU knows nothing about it, and it reads
        back through the same [#…] sigil as any other field.

        Returns one CONDITION per name. Pass it as `cond=` to whatever that view is made of — panels,
        tables, anything — and only the selected view is ever on screen. Conditions rather than container
        panels because a view is usually several panels, and a panel cannot hold another panel.
        """
        g = self.group()
        self.add(self._new('label', x, y + 4, 40, LBL_H,
                           {'labelText': 'View', 'align': 'Left', 'fontName': FONT_LBL,
                            'fgColor': C_DIM}, g))
        self.add(self._new('enum', x + 44, y, 280, CTL_H, {'signalName': f'pc.{var}'}, g))
        return [f'[#pc.{var}] == {i}' for i in range(len(names))]

    def graph(self, x, y, w, h, channels, into=None):
        return self.add(self._new('livegraph', x, y, w, h,
                                  {'signalName': channels, 'displayUnit': 'Auto'}), into=into)

    def traces(self, x, y, w, h, lines, into=None, title=''):
        """A multi-line trace: [(channel, lo, hi)] on ONE set of axes, fixed ranges so the shape is
        comparable between runs instead of the auto-scale flattering every wobble to full height. The
        legend's eye hides a line without disturbing the rest.

        A line names a CHANNEL, not an expression. The rows are "channel,min,max,autoMin,autoMax,hidden"
        (LineGraphModel) with no name field — the channel IS the name — so a line written as "[$rpm]"
        drew correctly and then appeared in Edit ▸ Edit Lines as an entry with no name at all. Callers may
        pass either form; the sigil is stripped here so one habit cannot produce nameless lines.
        """
        def chan(c):
            c = c.strip()
            return c[2:-1].strip() if c.startswith('[$') and c.endswith(']') else c
        rows = '\n'.join(f'{chan(ch)},{lo},{hi},0,0,0' for ch, lo, hi in lines)
        props = {'lines': rows, 'showLegend': '1', 'displayUnit': 'Auto', 'borderWidth': '1'}
        if title: props['labelText'] = title
        return self.add(self._new('livegraph', x, y, w, h, props), into=into)

    # ---- serialise ------------------------------------------------------------------------------
    def to_json(self):
        # A TICK BOX'S NAME SITS ON ITS BOX. Twelve builders pair a checkbox with its label and each chose
        # its own vertical offset — y-2, y+2, y-3, y-1 — so across the dashboard the text sat anywhere from
        # 4 px above its box to 1 px below it, and a switchboard read as a column of boxes with the words
        # floating off them. Centred once, here, for every page: a one-line label beside a checkbox in the
        # same group is placed so its centre is the box's centre.
        # A READING IN A UNIT THE STUDIO KNOWS IS SHOWN AT THAT UNIT'S PRECISION (and in the unit the user
        # chose — psi, °F — which it is either way). Pages hard-coded '%.0f' on MAP and fuel load and the
        # reading lost the decimal the kPa unit carries. A page format is kept only when it asks for MORE
        # than the unit gives (injector pulse width in hundredths of a ms): that is precision, not habit.
        def unit_formats(ws):
            ws = [dict(w) for w in ws]
            for w in ws:
                if w.get('type') == 'value' and (w.get('props') or {}).get('format'):
                    if unit_keeps_format(w['props'].get('signalName', ''), w['props']['format']) is False:
                        w['props'] = {k: v for k, v in w['props'].items() if k != 'format'}
            return ws

        def centre_ticks(ws):
            ws = [dict(w) for w in ws]
            boxes = [w for w in ws if w.get('type') == 'checkbox' and w.get('groupId')]
            for w in ws:
                if w.get('type') != 'label' or not w.get('groupId') or w.get('h', 0) > LBL_H:
                    continue
                for b in boxes:
                    if b['groupId'] == w['groupId'] and 0 <= w['x'] - (b['x'] + b['w']) <= 16:
                        w['y'] = b['y'] + (b['h'] - w['h']) // 2
                        break
            return ws

        # NOTHING LEAVES WIDER THAN THE BOX IT IS IN. A panel hands its children w-4, and a child authored
        # to the panel's full width is therefore 2-4 px past what it is given. Individually those are
        # invisible; collectively they are what the document's own geometry check counts, and chasing them
        # emitter by emitter across twelve installers is a losing game — a caption's width is computed
        # from a measured name in half a dozen places and any of them can win a max().
        #
        # So it is enforced once, on the way out, where every widget from every installer passes. Only
        # something ALREADY overflowing is touched, which is the case that was wrong anyway.
        def pack(ws, box_w=None, box_h=None):
            ws = unit_formats(centre_ticks(ws))
            out = []
            for w in ws:
                w = dict(w)
                if box_w is not None and w.get('x', 0) + w.get('w', 0) > box_w:
                    w['w'] = max(8, box_w - w.get('x', 0))
                # …AND NOTHING LEAVES TALLER THAN THE BOX EITHER. The width rule below was written first
                # because that is where the overflows were; the same arithmetic applies down the page and
                # the same fix does. A panel hands its children h less its title band and its 4px inset,
                # so a control authored to the panel's full height ends a title-height past the box it is
                # actually given — which is what the document's own checker counts, in the same terms.
                if box_h is not None and w.get('y', 0) + w.get('h', 0) > box_h:
                    w['h'] = max(8, box_h - w.get('y', 0))
                kids = w.pop('kids', None)
                if kids is not None:
                    w['props'] = dict(w['props'])
                    titled = bool(w['props'].get('labelText'))
                    inner_w = w.get('w', 0) - 4
                    inner_h = w.get('h', 0) - 4 - (PANEL_TITLE if titled else 0)
                    w['props']['children'] = json.dumps(pack(kids, inner_w, inner_h), separators=(',', ':'))
                out.append(w)
            return out
        # THE INTRO NOTE IS AS TALL AS ITS WORDS, and content that starts at a constant runs into it.
        # Page.head() returns where the content should begin — note bottom, or TOP when the note is one
        # line — and a caller that uses A.TOP instead lays its first row 14 px up inside a two-line blurb.
        # It reads as a heading printed over a panel, and every geometry check passes because the boxes
        # both fit the page. Repaired once here, where every page from every installer passes: if anything
        # in the band overruns the first row below it, that row and everything under it move down, and the
        # page grows by the same amount so nothing is pushed off the bottom.
        widgets = [dict(w) for w in self.widgets]
        height  = self.h
        band  = [w for w in widgets if w.get('y', 0) < TOP]
        below = [w for w in widgets if w.get('y', 0) >= TOP]
        if band and below:
            overrun = max(w.get('y', 0) + w.get('h', 0) for w in band) + 2 - min(w['y'] for w in below)
            if overrun > 0:
                for w in below:
                    w['y'] += overrun
                height += overrun

        # A PAGE IS THE SIZE OF WHAT IS ON IT, not the size of the sheet it was drawn on. Every page here
        # declared the full 1280x700 canvas whatever it held, and the window that opens it believes that:
        # Idle Control > Target RPM is a 4x8 grid and three readouts, and it opened 1280 wide and swamped
        # the tab's own instruments. Measured from the widgets, plus the margin they were laid out with —
        # nothing can be outside it, because this IS the outside of it.
        right  = max((w.get('x', 0) + w.get('w', 0) for w in widgets), default=self.w)
        bottom = max((w.get('y', 0) + w.get('h', 0) for w in widgets), default=height)
        page_w = int(min(self.w, right + MARGIN))
        page_h = int(min(height, bottom + MARGIN))

        return {
            'uid': str(uuid.uuid4()),
            'canvasWidth': page_w, 'canvasHeight': page_h,
            # THE SAME FLOOR THE CONVERTER DECLARES, because a page is a page. A reflow page hands the
            # layout less room as the window narrows, which is right until there is not enough for a
            # caption and its control side by side; past that the rows stop getting narrower and start
            # getting unreadable. minWidth is where that stops and the surface scrolls instead — and it is
            # also the size the window opens at. Native pages were built to self.w, so that is the number.
            # Without it these pages behaved differently from imported ones for no reason anybody could
            # name, which is exactly the divergence the import is supposed to remove.
            'minWidth': page_w,
            **({'elementScope': self.element} if self.element else {}),
            'canvasStatic': 3, 'canvasAnchor': 0, 'guideW': 0, 'guideH': 0,   # 3 = reflow: the page is the view
            # FREE, WITH THE COORDINATES THESE PAGES WERE AUTHORED IN. They were briefly turned into a
            # Column of X Axis rows, banded by y — which cannot express what these pages actually are. The
            # Sensors page is FIVE INDEPENDENT COLUMNS, each with its own headings and its own row pitch;
            # banding it by y merges rows that only happen to share a y, and every column below a heading
            # drifts out of step with its neighbours. The tick boxes ended up detached from the names they
            # tick. The rows conversion existed to stop a page being SCALED down in a small window, and
            # that job now belongs to minWidth above: the canvas stops shrinking at the authored width and
            # the surface scrolls, so the coordinates are honoured 1:1 exactly as they were authored.
            'title': self.title, 'layout': 0, 'gridColumns': 2, 'focusIndex': 0,   # 0 = Free
            'borderColor': '', 'borderWidth': 0, 'borderStyle': 1, 'borderRadius': 4,
            'titleFont': '', 'titleColor': '', 'titlePadding': 4, 'titleStyle': 1,
            'titlePlace': 0, 'titleEdge': 0, 'titleAlign': 0,
            'widgets': pack(widgets, self.w, height),   # packed against the SHEET; the page is measured above
        }


# ---- validation ---------------------------------------------------------------------------------
def check(page, name, band=True):
    """Does everything on this page actually FIT?

    This exists because the answer was no, everywhere, and nothing said so: pages were authored to one
    size and declared another, so the right-hand column of every table page fell outside its viewport and
    the first row went under the viewport's title bar. A page is a fixed canvas — there is no reflow to
    save it — so the only way it stays right is for something to measure it.

    Returns a list of complaints, empty when the page fits.
    """
    bad = []
    for w in page.widgets:
        if w['x'] < 0 or w['y'] < 0:
            bad.append(f"{name}: {w['type']} at ({w['x']},{w['y']}) is off the top/left")
        if w['x'] + w['w'] > page.w:
            bad.append(f"{name}: {w['type']} right edge {w['x'] + w['w']} > canvas {page.w}")
        if w['y'] + w['h'] > page.h:
            bad.append(f"{name}: {w['type']} bottom edge {w['y'] + w['h']} > canvas {page.h}")
        # …only for a page shown INSIDE a viewport. A tab surface (Diagnostics) has no viewport over it
        # and owns its whole canvas, top row included.
        # …the note row itself holds only the note and, at its right end, the page's own enable switch.
        in_switch_slot = w['x'] < 136        # the page's own enable, at the left of the note row
        if (band and page.has_note and w['type'] != 'label' and not in_switch_slot
                and BAND <= w['y'] < TOP):
            bad.append(f"{name}: {w['type']} at y={w['y']} overlaps the note row (content starts at {TOP})")
        if band and w['y'] < BAND:
            bad.append(f"{name}: {w['type']} at y={w['y']} is under the viewport's title bar (BAND={BAND})")
        # Measured against the CONTENT box, not the panel: children start below the title bar.
        if w['props'].get('layoutMode', '0') not in ('', '0'):
            continue                       # arranged at runtime — see child_overlaps
        inner_h = w['h'] - (PANEL_TITLE if w['props'].get('labelText') else 0) - 4
        for k in w.get('kids', []):
            if k['x'] + k['w'] > w['w'] - 4:
                bad.append(f"{name}: {w['props'].get('labelText','panel')} child {k['type']} "
                           f"right edge {k['x'] + k['w']} > panel width {w['w']}")
            if k['y'] + k['h'] > inner_h:
                bad.append(f"{name}: {w['props'].get('labelText','panel')} child {k['type']} "
                           f"bottom edge {k['y'] + k['h']} > content height {inner_h} "
                           f"(panel {w['h']} less its {PANEL_TITLE}px title bar)")
    # NOTHING PRINTS OVER A PANEL. The Sensors switchboard put a summary line at a fixed y near the page
    # bottom while its columns packed all the way down, so the note ran straight across the last panel's
    # final rows — inside the canvas, inside every panel, and wrong. Overlap is the check that catches it,
    # and it is only asked of top-level widgets: children are laid out inside their panel's own box.
    # EVERY TOP-LEVEL PAIR, not just the ones touching a panel. Two exemptions used to gut this: labels
    # above y=TOP were skipped entirely, and any pair without a panel in it was skipped as "a caption".
    # Between them, a label sitting on a table and a label sitting on its own value were both invisible
    # to the checker — which is how the VE and Advance pages carried a two-pixel label/value overlap and
    # a note drawn across the grid, for as long as they did, with the checker reporting clean.
    #
    # The caption case is real, so it is exempted PRECISELY: widgets in the same group are one control
    # written as two boxes (a readout's value and its caption), and those are allowed to touch.
    tops = list(page.widgets)
    for i, a_ in enumerate(tops):
        for b_ in tops[i + 1:]:
            if a_.get('groupId') and a_.get('groupId') == b_.get('groupId'):
                continue                      # one control drawn as two boxes
            # Two panels that can never be on screen together cannot collide on it: a stack of sub-tab
            # pages is exactly that — same rect, one condition each, one visible (see Page.subtabs).
            if never_together(a_, b_):
                continue
            if (a_['x'] < b_['x'] + b_['w'] and b_['x'] < a_['x'] + a_['w']
                    and a_['y'] < b_['y'] + b_['h'] and b_['y'] < a_['y'] + a_['h']):
                bad.append(f"{name}: {a_['type']} \"{a_['props'].get('labelText','')[:20]}\" overlaps "
                           f"{b_['type']} \"{b_['props'].get('labelText','')[:20]}\"")
    bad += child_overlaps(page, name)
    bad += text_check(page, name)
    return bad


# ---- validation: children inside a panel --------------------------------------------------------
SLACK = 2      # px of touching that is not a collision — a box is usually a little bigger than its ink


def never_together(a, b):
    """Can these two widgets never be on screen at the same time? Then they cannot collide on it.

    Two ways, both decided at run time and mirrored here: different `condition`s (a stack of sub-tab
    pages), or the CALIBRATION rule — every widget bound to a sensor's `.cal` is shown for one kind of
    calibration only (CanvasWidget::showsCalibrationOf): the band editor for a multi-position switch's,
    everything else (the table, the curve, their caption) for an ordinary one. So a `bands` widget and a
    non-`bands` widget on the same calibration share their space by design."""
    ca, cb = a['props'].get('condition', ''), b['props'].get('condition', '')
    if ca and cb and ca != cb:
        return True
    sa, sb = a['props'].get('signalName', ''), b['props'].get('signalName', '')
    if sa and sa == sb and sa.endswith('.cal') and (a['type'] == 'bands') != (b['type'] == 'bands'):
        return True
    return False


def child_overlaps(page, name):
    """Do two things inside the same panel sit on top of each other?

    The pass in check() walks TOP-LEVEL widgets only, so a panel's own children were never compared with
    each other — and a panel's children are where the pages actually live: a form row is a name, a control
    and a unit, three children, and it is rows that get retuned.

    Measured on INK, not on boxes. A caption's box is routinely taller and wider than the words in it (it
    is centred in whatever height the author gave it), so box-against-box calls a collision on two things
    that are plainly clear of each other on screen. What matters is whether the DRAWN text lands on the
    neighbour, which is a question ruler.py can answer.
    """
    bad = []
    for w in page.widgets:
        # A MANAGED CONTAINER PLACES ITS OWN CHILDREN. In a flow (layoutMode 7) or any of the other
        # arranged modes the authored x/y are what the layout hands out, not where anything ends up —
        # every child is written at 0,0 — so comparing their rects reports an overlap per pair and says
        # nothing. The arrangement is the runtime's, and is checked by looking at it.
        if w.get('props', {}).get('layoutMode', '0') not in ('', '0'):
            continue
        kids = w.get('kids', [])
        boxes = [ink_box(k) for k in kids]
        for i, a_ in enumerate(kids):
            for j in range(i + 1, len(kids)):
                b_ = kids[j]
                if a_.get('groupId') and a_.get('groupId') == b_.get('groupId'):
                    continue                  # one control drawn as two boxes (a value and its caption)
                if never_together(a_, b_):
                    continue                  # never on screen together
                ax, ay, aw, ah = boxes[i]
                bx, by, bw, bh = boxes[j]
                if (ax + SLACK < bx + bw and bx + SLACK < ax + aw
                        and ay + SLACK < by + bh and by + SLACK < ay + ah):
                    bad.append(f"{name}: inside \"{w['props'].get('labelText','panel')}\", "
                               f"{a_['type']} \"{a_['props'].get('labelText','')[:24]}\" overlaps "
                               f"{b_['type']} \"{b_['props'].get('labelText','')[:24]}\"")
    return bad


def ink_box(w):
    """What this widget actually PAINTS: (x, y, w, h). Everything but a caption fills its box; a caption
    is its lines, as wide as the widest of them and as tall as all of them, placed by its own alignment
    and centred vertically — which is what LabelWidget::render does with it."""
    from ruler import ruler, font_px, LABEL_PAD
    box = (w['x'], w['y'], w['w'], w['h'])
    text = w['props'].get('labelText', '')
    if w['type'] != 'label' or not text:
        return box
    r = ruler()
    px = font_px(w['props'].get('fontName', ''))
    lines = r.lines(text, w['props'].get('wrap') == '1', px, w['w'] - 2 * LABEL_PAD)
    tw = max((r.width(ln, px) for ln in lines), default=0.0)
    th = px * len(lines)
    align = w['props'].get('align', 'Center')
    if   align == 'Left':  ix = w['x'] + LABEL_PAD
    elif align == 'Right': ix = w['x'] + w['w'] - LABEL_PAD - tw
    else:                  ix = w['x'] + (w['w'] - tw) / 2
    return (ix, w['y'] + (w['h'] - th) / 2, tw, th)


# ---- validation: the TEXT, not the box ----------------------------------------------------------
def text_check(page, name):
    """caption_complaints over a Page as the builders make it — its children live in 'kids'."""
    return caption_complaints(name, page.widgets, lambda w: w.get('kids', []))


def caption_complaints(name, tops, child_of):
    """Does every caption here fit inside its own box?

    The checks above measure boxes against boxes. A caption is clipped to its widget's rect
    (CanvasWidget::paint pushes the clip) and LabelWidget draws its glyphs with no maxWidth, so a caption
    wider than its box is guillotined mid-word — no ellipsis, nothing on screen to say the rest exists —
    and every geometry check passes, because the BOX fits. ruler.py measures the words with the app's own
    font; this is LabelWidget::render's arithmetic over them.

    Deliberately measured at the built-in 3px inset (LabelWidget::kTextPad) rather than at this profile's
    Widget Default, which happens to be 0: a page is authored once and opened on whatever profile, so the
    room to author against is the SMALLER of the two. (The FONT is still the profile's, and has to be:
    the whole authored text scale is derived from the application font's atlas, so a fixed canvas is only
    ever right FOR a font. This says whether it is right for the one in use.)

    `child_of(w)` yields a widget's children — a list on a Page, a JSON string in the document — so the
    document audit measures captions with this same arithmetic rather than a second copy of it.
    """
    from ruler import ruler, font_px, LABEL_PAD, TITLE_INSET
    r = ruler()
    bad = []

    def one(w, where):
        text = w['props'].get('labelText', '')
        if not text:
            return
        if w['type'] != 'label':
            # A titled card's own title: base-atlas text at 1:1, inset 8px each side, clipped to the bar.
            over = r.base_width(text) - (w['w'] - TITLE_INSET)
            if over > 0.5:
                bad.append(f'{name}: {where}title "{text[:40]}" is {over:.0f}px wider than its '
                           f'{w["w"]}px title bar')
            return
        px = font_px(w['props'].get('fontName', ''))
        lines = r.lines(text, w['props'].get('wrap') == '1', px, w['w'] - 2 * LABEL_PAD)
        room = w['w'] if w['props'].get('align', 'Center') == 'Center' else w['w'] - LABEL_PAD
        over = max((r.width(ln, px) for ln in lines), default=0.0) - room
        if over > 0.5:
            bad.append(f'{name}: {where}caption "{text[:40]}" is {over:.0f}px wider than its '
                       f'{w["w"]}px box — the tail is cut off')
        tall = px * len(lines) - w['h']
        if tall > 0.5:
            bad.append(f'{name}: {where}caption "{text[:40]}" wraps to {len(lines)} lines '
                       f'({px * len(lines):.0f}px) in a {w["h"]}px box — the first and last are cut off')

    for w in tops:
        one(w, '')
        for k in child_of(w):
            one(k, f'in "{w["props"].get("labelText", "panel")}", ')
    return bad


# ---- unit precision -----------------------------------------------------------------------------
# The Studio's own unit table (src/model/UnitManager.cpp): each unit's default decimals. A reading of a
# channel in one of these units is formatted by the unit, not the page (Page.to_json, unit_formats).
UNIT_DECIMALS = {
    'Nm': 1, 'lb-ft': 1, 'kg-m': 2, 'W': 0, 'kW': 1, 'hp': 1, 'ps': 1, 'm/s': 1, 'km/h': 1, 'mph': 1,
    'deg': 1, 'RPM': 0, 'rad/s': 1, 'deg/s': 0, 'RPS': 2, 'Hz': 1, 'kHz': 2, 'MHz': 3,
    's': 2, 'ms': 1, 'us': 0, 'min': 2, 'Pa': 0, 'kPa': 1, 'bar': 2, 'psi': 1, 'C': 1, 'F': 1, 'K': 1,
    'pulses/km': 0, 'pulses/mi': 0, 'RPM/km/h': 1, 'RPM/mph': 1, 'mV': 0, 'V': 2, 'ADC': 0,
}


FINER_THAN_UNIT = {'inj_pw', 'narrowband_1', 'narrowband_2'}


def unit_keeps_format(signal, fmt, telemetry=None):
    """False when a page format on `signal` should give way to its unit's precision; True when the page
    asks for more decimals than the unit gives; None when the channel has no unit the Studio knows."""
    import re
    ch = re.sub(r'^\[\$|\]$', '', signal or '')
    tel = telemetry if telemetry is not None else Page._meta().get('telemetry', {})
    e = tel.get(ch) if isinstance(tel, dict) else None
    if not e:
        return None
    u = e.get('units') or ''
    if u not in UNIT_DECIMALS:
        return None
    m = re.search(r'\.(\d+)f', fmt or '')
    # FINER THAN THE UNIT ONLY WHERE IT IS MEANT. Injector pulse width in hundredths of a ms (it matters at
    # idle) and a narrowband's volts (it lives inside 0.1-0.9 V). Everything else asking for more — a µs to
    # a decimal, hundredths of a degree the firmware sends in tenths — was habit, not precision.
    return (int(m.group(1)) if m else 0) > UNIT_DECIMALS[u] and ch in FINER_THAN_UNIT

