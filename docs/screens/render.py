#!/usr/bin/env python3
"""
Faithful 320x240 renderer for the Core (iPod firmware) "Linen" warm-light UI.

Reproduces the on-device look of core/kernel/main.c + core/ui/screen_settings.c:
the status strip, titled headers, list rows (single + two-line with art chips),
album detail, now-playing, the volume overlay, the lock/unlock modal, the About
dashboard and the Boot Details diagnostics page.

Text is drawn with the real Nunito faces via PIL, gamma-correct (sRGB->linear,
blend by glyph coverage, re-encode) so it reads as crisply as the device's
gamma-aware atlas — and, since the firmware's text stack grew a 26.6 pen,
kerning and per-atlas tracking, it lays glyphs out with the DEVICE's metrics
rather than PIL's defaults (see the Fonts section). Tracking is read out of
core/ui/atlas/*.h at runtime, so a screenshot cannot quietly drift from the
firmware again. Album art is quantized to RGB565 to match the panel.

Outputs PNGs (3x, NEAREST) + a marquee demo GIF into this directory.

Regenerate:  tools/.venv/bin/python3 docs/screens/render.py
"""

import os
import re
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
# Repo-relative, not an absolute path into one person's home directory (which
# is what this was, and which meant the script only ran on one machine).
# CORE_FONTS_DIR overrides it if the faces ever live somewhere else.
REPO = os.path.dirname(os.path.dirname(HERE))
FONTS = os.environ.get("CORE_FONTS_DIR", os.path.join(REPO, "tools", "fonts-src"))
ATLAS = os.path.join(REPO, "core", "ui", "atlas")
ART = os.path.join(HERE, "art")

if not os.path.isdir(FONTS):
    raise SystemExit(
        f"font directory not found: {FONTS}\n"
        f"Expected the Nunito faces in <repo>/tools/fonts-src; set "
        f"CORE_FONTS_DIR to point somewhere else.")

W, H = 320, 240
SCALE = 3          # PNG upscale (NEAREST)
GIF_SCALE = 2      # GIF upscale (keep file small)

# ---------------------------------------------------------------------------
# Palette — the Linen theme (RGB565 -> RGB888)
# ---------------------------------------------------------------------------
def rgb565(v):
    r = (v >> 11) & 0x1F
    g = (v >> 5) & 0x3F
    b = v & 0x1F
    return (round(r * 255 / 31), round(g * 255 / 63), round(b * 255 / 31))

BATT_RED = rgb565(0xE125)   # low-battery warning, theme-independent

# Two themes, both RGB565. SEL_BG/SEL_FG derive from INK/SURFACE so the inverted
# selection bar reads right in both (Linen: dark bar/light text; Onyx: the
# reverse). apply_palette() rebinds the module globals the screen builders read.
LINEN = dict(
    SURFACE=0xF79D, INK=0x18A2, MUTED=0x7B8D, MUTED2=0x9C70, MUTED_D=0x5A89,
    ACCENT=0xC348, BORDER=0xE71B, PLATE=0xF7BE, TRK=0xDEDA, SB_TRK=0xE73C,
    SB_THMB=0xAD34, SEL_SUB=0xB595, CHEVRON=0xC638, SEL_TRK=0x4A69,
)
ONYX = dict(
    SURFACE=0x18C2, INK=0xEF3C, MUTED=0xACF2, MUTED2=0xB533, MUTED_D=0x8C0E,
    ACCENT=0xC348, BORDER=0x3185, PLATE=0x2944, TRK=0x39A5, SB_TRK=0x2924,
    SB_THMB=0x6B0B, SEL_SUB=0x5A89, CHEVRON=0x4A27, SEL_TRK=0x8410,
)
_PAL_KEYS = list(LINEN.keys()) + ["SEL_BG", "SEL_FG"]

def apply_palette(spec):
    g = globals()
    for k, v in spec.items():
        g[k] = rgb565(v)
    g["SEL_BG"] = g["INK"]        # selection bar fill
    g["SEL_FG"] = g["SURFACE"]    # text on selection bar

# default active theme = Linen (the stills are Linen)
apply_palette(LINEN)

# ---------------------------------------------------------------------------
# Gamma-correct text blend
# ---------------------------------------------------------------------------
def _s2l(c):
    cs = c / 255.0
    return cs / 12.92 if cs <= 0.04045 else ((cs + 0.055) / 1.055) ** 2.4

LIN = [_s2l(i) for i in range(256)]

def _l2s(v):
    if v <= 0.0:
        return 0
    if v >= 1.0:
        return 255
    cs = v * 12.92 if v <= 0.0031308 else 1.055 * (v ** (1 / 2.4)) - 0.055
    return int(cs * 255 + 0.5)

# ---------------------------------------------------------------------------
# Fonts — the DEVICE's text metrics, not PIL's defaults
# ---------------------------------------------------------------------------
# core/ui/text.c lays a string out in 26.6 fixed point: it carries the
# fractional advance across glyphs (rounding the pen only when a glyph is
# blitted), adds the face's kerning pair adjustment, and adds a per-atlas
# TRACKING between glyphs — suppressed on either side of a space. None of that
# is what PIL does by default, and a renderer that guesses at it produces
# screenshots that disagree with the panel by several pixels per string (which
# is exactly what these did).
#
# So mirror the generator: tools/atlas_gen.py derives every advance and every
# kern pair from THIS Pillow, through getlength() under the Raqm layout engine,
# then quantizes them. Doing the same arithmetic here means the numbers below
# are the numbers baked into core/ui/atlas/*.h, not an approximation of them.
#
# The basic layout engine is not an option: it applies no kerning at all and
# returns advances already rounded to whole pixels.
ADV_ONE     = 64      # 26.6, == ATLAS_ADV_ONE   (core/ui/atlas.h)
KERN_ONE    = 32      # 1/32 px, == atlas_kern_t.adj scale
KERN_MIN_PX = 0.25    # == KERN_MIN_PX           (tools/atlas_gen.py)


def _atlas_field(header, field):
    """Read `.field = N,` out of a generated atlas header. Loud on absence: a
    missing header or a renamed field must not silently degrade to untracked,
    unkerned text that looks fine in isolation and wrong beside the device."""
    path = os.path.join(ATLAS, header)
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise SystemExit(
            f"atlas header not readable: {path} ({e})\n"
            f"The screenshots take their text metrics from the generated "
            f"atlases; regenerate them with tools/atlas_gen.sh.")
    m = re.search(r"\.%s\s*=\s*(-?\d+)" % field, src)
    if not m:
        raise SystemExit(f"no '.{field} =' in {path} — atlas format changed?")
    return int(m.group(1))


def _atlas_charset():
    """Codepoints the atlases actually carry: printable ASCII plus whatever
    tools/atlas_gen.py appended (read from the generated glyphmap, so it cannot
    drift from the firmware). Anything outside this renders as a .notdef box on
    the device, which is a different picture from what PIL would draw."""
    cps = set(range(0x20, 0x7F))
    path = os.path.join(ATLAS, "glyphmap.h")
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError as e:
        raise SystemExit(f"atlas glyphmap not readable: {path} ({e})")
    cps.update(int(h, 16) for h in re.findall(r"\{\s*0x([0-9A-Fa-f]{4})\s*,", src))
    return cps

ATLAS_CHARS = _atlas_charset()


class Face:
    """One (family, size) — the host-side twin of a core/ui/atlas/*.h atlas."""

    def __init__(self, ttf, size, header):
        self.font = ImageFont.truetype(os.path.join(FONTS, ttf), size,
                                       layout_engine=ImageFont.Layout.RAQM)
        self.name = "%s@%d" % (ttf, size)
        self.tracking = _atlas_field(header, "tracking")     # 26.6
        self.ascent = _atlas_field(header, "ascent")
        self.line_height = _atlas_field(header, "line_height")
        self._adv = {}
        self._kern = {}
        self._len = {}

    def _length(self, s):
        v = self._len.get(s)
        if v is None:
            v = self._len[s] = self.font.getlength(s)
        return v

    def advance(self, ch):
        """Unkerned pen advance in 26.6 — atlas_gen.py's `advance` field."""
        a = self._adv.get(ch)
        if a is None:
            if ord(ch) not in ATLAS_CHARS:
                raise SystemExit(
                    f"{ch!r} (U+{ord(ch):04X}) is not in the atlas: the device "
                    f"would draw a .notdef box here. Add it to "
                    f"tools/atlas_gen.py EXTRAS or keep it out of the mock-ups.")
            a = self._adv[ch] = int(round(self._length(ch) * ADV_ONE))
        return a

    def kern(self, a, b):
        """Kern for the ordered pair in 26.6, quantized exactly as the atlas
        bakes it (1/32 px, pairs under KERN_MIN_PX dropped)."""
        k = self._kern.get((a, b))
        if k is None:
            raw = self._length(a + b) - self._length(a) - self._length(b)
            adj = 0 if abs(raw) < KERN_MIN_PX else int(round(raw * KERN_ONE))
            k = self._kern[(a, b)] = adj * 2          # 1/32 -> 26.6
        return k

    def layout(self, s):
        """(glyphs, width): glyphs is [(char, x offset in px)] with the pen
        rounded per glyph, width is the pen rounded once at the end — the two
        halves of core/ui/text.c's contract (text_draw returns text_width)."""
        pen = 0
        prev = None
        out = []
        for ch in s:
            if prev is not None:
                # A space counts as ONE tracking unit (text.c track_adv):
                # tracking goes in on the way INTO a space, not on the way out.
                # Both sides would widen every word gap by twice the tracking;
                # neither side grows the letter gaps while the word gap stays
                # put, which is what made words merge. One side moves the word
                # gap by the same amount as every letter gap, so the ratio the
                # face was designed with survives. Kerning still applies across
                # a space if the face has such a pair.
                if prev != " ":
                    pen += self.tracking
                pen += self.kern(prev, ch)
            out.append((ch, (pen + ADV_ONE // 2) >> 6))
            pen += self.advance(ch)
            prev = ch
        return out, (pen + ADV_ONE // 2) >> 6

    def width(self, s):
        return self.layout(s)[1] if s else 0


regular_9  = Face("Nunito-Regular.ttf",  9, "nunito_regular_9.h")
regular_11 = Face("Nunito-Regular.ttf", 11, "nunito_regular_11.h")
regular_13 = Face("Nunito-Regular.ttf", 13, "nunito_regular_13.h")
bold_11    = Face("Nunito-Bold.ttf",    11, "nunito_bold_11.h")
bold_13    = Face("Nunito-Bold.ttf",    13, "nunito_bold_13.h")
bold_17    = Face("Nunito-Bold.ttf",    17, "nunito_bold_17.h")

FONT_SMALL  = regular_9
FONT_SUB    = regular_11
FONT_ROW    = regular_13
FONT_HEADER = bold_13
FONT_TITLE  = bold_17

# glyphs used as literals on-device
LAQUO = "‹"   # 'single left angle quote
RAQUO = "›"
MIDDOT = "·"

def text_width(s, font):
    return font.width(s)

# ---------------------------------------------------------------------------
# Surface / primitives
# ---------------------------------------------------------------------------
class Screen:
    def __init__(self, bg=None):
        # resolve the surface at call time so the active palette wins (a default
        # arg would freeze the Linen surface captured at def time)
        if bg is None:
            bg = SURFACE
        self.img = Image.new("RGB", (W, H), bg)
        self.px = self.img.load()

    # -- rectangles ------------------------------------------------------
    def fill_rect(self, x, y, w, h, c):
        x0 = max(0, x); y0 = max(0, y)
        x1 = min(W, x + w); y1 = min(H, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        ImageDraw.Draw(self.img).rectangle([x0, y0, x1 - 1, y1 - 1], fill=c)

    def _isqrt(self, v):
        r = 0
        while (r + 1) * (r + 1) <= v:
            r += 1
        return r

    def fill_round_rect(self, x, y, w, h, r, c):
        """Integer-inset rounded rect — matches main.c fill_round_rect exactly."""
        if r < 1:
            self.fill_rect(x, y, w, h, c)
            return
        if 2 * r > w:
            r = w // 2
        if 2 * r > h:
            r = h // 2
        for ry in range(h):
            inset = 0
            k = -1
            if ry < r:
                k = ry
            elif ry >= h - r:
                k = h - 1 - ry
            if k >= 0:
                dy = r - k
                inset = r - self._isqrt(r * r - dy * dy)
            self.fill_rect(x + inset, y + ry, w - 2 * inset, 1, c)

    def fill_round_rect_aa(self, x, y, w, h, r, c):
        """AA rounded rect for plates: solid body + 4x4 supersampled corners
        blended onto the existing framebuffer (matches main.c)."""
        if r < 1:
            self.fill_rect(x, y, w, h, c)
            return
        if 2 * r > w:
            r = w // 2
        if 2 * r > h:
            r = h // 2
        self.fill_rect(x, y + r, w, h - 2 * r, c)
        S = 4
        cN = r * 2 * S
        lr, lg, lb = LIN[c[0]], LIN[c[1]], LIN[c[2]]
        for ry in range(r):
            self.fill_rect(x + r, y + ry, w - 2 * r, 1, c)
            self.fill_rect(x + r, y + h - 1 - ry, w - 2 * r, 1, c)
            for rx in range(r):
                inside = 0
                for sy in range(S):
                    dy = ry * 2 * S + sy * 2 + 1 - cN
                    for sx in range(S):
                        dx = rx * 2 * S + sx * 2 + 1 - cN
                        if dx * dx + dy * dy <= cN * cN:
                            inside += 1
                if inside == 0:
                    continue
                a = inside / (S * S)
                for px_ in (x + rx, x + w - 1 - rx):
                    for py_ in (y + ry, y + h - 1 - ry):
                        if 0 <= px_ < W and 0 <= py_ < H:
                            if a >= 1.0:
                                self.px[px_, py_] = c
                            else:
                                br, bg, bb = self.px[px_, py_]
                                self.px[px_, py_] = (
                                    _l2s(lr * a + LIN[br] * (1 - a)),
                                    _l2s(lg * a + LIN[bg] * (1 - a)),
                                    _l2s(lb * a + LIN[bb] * (1 - a)),
                                )

    # -- text (gamma-correct) -------------------------------------------
    def text(self, x, baseline, s, font, ink, clip=None):
        if not s:
            return x
        # Glyph BY GLYPH at the device's pen positions (kerned, tracked, 26.6
        # carried) — handing PIL the whole string would lay it out with none of
        # that. Each glyph goes down at its own rounded pen x, which is what
        # text.c does before it blits.
        glyphs, width = font.layout(s)
        mask = Image.new("L", (W, H), 0)
        md = ImageDraw.Draw(mask)
        for ch, dx in glyphs:
            if ch == " ":
                continue
            md.text((x + dx, baseline), ch, font=font.font, fill=255, anchor="ls")
        bbox = mask.getbbox()
        if bbox:
            mp = mask.load()
            x0, y0, x1, y1 = bbox
            if clip:
                x0 = max(x0, clip[0])
                x1 = min(x1, clip[1])
            lir, lig, lib = LIN[ink[0]], LIN[ink[1]], LIN[ink[2]]
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    a = mp[xx, yy]
                    if not a:
                        continue
                    if a == 255:
                        self.px[xx, yy] = ink
                        continue
                    af = a / 255.0
                    br, bg, bb = self.px[xx, yy]
                    self.px[xx, yy] = (
                        _l2s(lir * af + LIN[br] * (1 - af)),
                        _l2s(lig * af + LIN[bg] * (1 - af)),
                        _l2s(lib * af + LIN[bb] * (1 - af)),
                    )
        return x + width

    def text_centered(self, baseline, s, font, ink):
        self.text((W - text_width(s, font)) // 2, baseline, s, font, ink)

    def text_right(self, right_x, baseline, s, font, ink):
        self.text(right_x - text_width(s, font), baseline, s, font, ink)

    # -- album art -------------------------------------------------------
    def blit_art(self, x, y, dim, art_name, round_r=0):
        im = load_art(art_name, dim)
        self.img.paste(im, (x, y))
        if round_r > 0:
            # knock outer corner pixels back to background (surface)
            bg = SURFACE
            for dy in range(round_r):
                for dx in range(round_r):
                    if dx + dy >= round_r:
                        continue
                    self.px[x + dx, y + dy] = bg
                    self.px[x + dim - 1 - dx, y + dy] = bg
                    self.px[x + dx, y + dim - 1 - dy] = bg
                    self.px[x + dim - 1 - dx, y + dim - 1 - dy] = bg


# ---------------------------------------------------------------------------
# Album art cache — RGB565-quantized to match the panel
# ---------------------------------------------------------------------------
_ART_CACHE = {}

def load_art(name, dim):
    key = (name, dim)
    if key in _ART_CACHE:
        return _ART_CACHE[key]
    src = Image.open(os.path.join(ART, name + ".png")).convert("RGB")
    im = src.resize((dim, dim), Image.LANCZOS)
    # quantize to RGB565 (5/6/5) like the device framebuffer
    px = im.load()
    for y in range(dim):
        for x in range(dim):
            r, g, b = px[x, y]
            r = (r >> 3); g = (g >> 2); b = (b >> 3)
            px[x, y] = (round(r * 255 / 31), round(g * 255 / 63), round(b * 255 / 31))
    _ART_CACHE[key] = im
    return im


# ---------------------------------------------------------------------------
# Shared chrome
# ---------------------------------------------------------------------------
STATUS_H = 15
HDR_BASE = 30
HDR_DIV_Y = 38
LIST_Y0 = 42
ROW_H = 24
ROW_H2 = 32
LIST_ROWS = 8
LIST_ROWS2 = 6

def draw_battery(sc, x, y, pct):
    w, h = 22, 12
    sc.fill_rect(x, y, w, 1, MUTED2)
    sc.fill_rect(x, y + h - 1, w, 1, MUTED2)
    sc.fill_rect(x, y, 1, h, MUTED2)
    sc.fill_rect(x + w - 1, y, 1, h, MUTED2)
    sc.fill_rect(x + w, y + 4, 2, h - 8, MUTED2)
    pct = max(0, min(100, pct))
    fw = ((w - 4) * pct) // 100
    if fw > 0:
        sc.fill_rect(x + 2, y + 2, fw, h - 4, BATT_RED if pct <= 20 else INK)

def draw_lock_glyph(sc, x, y, c):
    sc.fill_rect(x, y + 4, 8, 6, c)
    sc.fill_rect(x + 1, y, 2, 5, c)
    sc.fill_rect(x + 5, y, 2, 5, c)
    sc.fill_rect(x + 1, y, 6, 2, c)

def status_strip(sc, left="CORE", pct=78, locked=False):
    sc.text(12, STATUS_H - 4, left, FONT_SMALL, MUTED2)
    bx = W - 12 - 24
    draw_battery(sc, bx, 1, pct)
    if locked:
        draw_lock_glyph(sc, bx - 14, 3, INK)

def header(sc, title, right=None, back=False):
    x = 12
    if back:
        x = sc.text(x, HDR_BASE, LAQUO, FONT_HEADER, MUTED2) + 4
    sc.text(x, HDR_BASE, title, FONT_HEADER, INK)
    if right:
        sc.text_right(W - 12, HDR_BASE - 1, right, FONT_SMALL, MUTED2)
    sc.fill_rect(12, HDR_DIV_Y, W - 24, 1, BORDER)

def scrollbar(sc, y0, top, visible, total):
    if total <= visible:
        return
    track_y = y0
    track_h = H - y0 - 4
    sc.fill_rect(W - 4, track_y, 3, track_h, SB_TRK)
    thumb_h = max(16, (visible * track_h) // total)
    denom = max(1, total - visible)
    thumb_y = track_y + (top * (track_h - thumb_h)) // denom
    sc.fill_rect(W - 4, thumb_y, 3, thumb_h, SB_THMB)


# ---------------------------------------------------------------------------
# List rows
# ---------------------------------------------------------------------------
def list_row(sc, y0, r, text, sub=None, right=None, chevron=False,
             selected=False, greyed=False, chip=None, rh=ROW_H,
             title_offset=0, title_priority=False):
    ry = y0 + r * rh
    rowmid = ry + rh // 2 + 3
    if selected:
        sc.fill_round_rect(6, ry + 1, W - 16, rh - 2, 4, SEL_BG)
        fg, subc, rightc, chevc = SEL_FG, SEL_SUB, SEL_SUB, SEL_SUB
    else:
        fg = MUTED if greyed else INK
        subc, rightc, chevc = MUTED2, MUTED_D, CHEVRON

    tx = 14
    if chip:
        cd = 28
        cy = ry + (rh - cd) // 2
        sc.blit_art(12, cy, cd, chip, round_r=2)
        cbg = SEL_BG if selected else SURFACE
        for dy in range(2):
            for dx in range(2):
                if dx + dy >= 2:
                    continue
                sc.px[12 + dx, cy + dy] = cbg
                sc.px[12 + cd - 1 - dx, cy + dy] = cbg
                sc.px[12 + dx, cy + cd - 1 - dy] = cbg
                sc.px[12 + cd - 1 - dx, cy + cd - 1 - dy] = cbg
        tx = 12 + cd + 8

    tf = FONT_HEADER if selected else FONT_ROW
    show_right = bool(right)
    if right:
        reserved = W - 16 - text_width(right, bold_11) - 6
        # title_priority: a long title spans the full width (over the value) and
        # the value drops, so it truncates/marquees at the row edge (main.c).
        if title_priority and text_width(text, tf) > reserved - tx:
            title_right = W - 16
            show_right = False
        else:
            title_right = reserved
    elif chevron:
        title_right = W - 18 - 4
    else:
        title_right = W - 16
    avail = title_right - tx

    base = (ry + 16) if sub else rowmid
    sub_y = ry + rh - 4
    # marquee/clip: title clipped to [tx, tx+avail); title_offset scrolls it
    sc.text(tx - title_offset, base, text, tf, fg, clip=(tx, tx + avail))
    if sub:
        sc.text(tx, sub_y, sub, FONT_SMALL, subc)
    if show_right:
        sc.text_right(W - 16, rowmid, right, bold_11, rightc)
    elif chevron:
        sc.text(W - 18, rowmid, RAQUO, FONT_ROW, chevc)


# ---------------------------------------------------------------------------
# Screens
# ---------------------------------------------------------------------------
# The firmware sorts the album list A->Z by ALBUM TITLE (the bold main line,
# case-insensitive) — not by artist. Sort here so the render matches.
ALBUMS = sorted([
    ("AUSTIN", "Post Malone", "austin"),
    ("Rearrange My World / There's a Field (That's Only Yours)", "Daniel Caesar", "rearrange"),
    ("F-1 Trillion", "Post Malone", "f1"),
    ("Hollywood's Bleeding", "Post Malone", "hollywood"),
    ("Malibu Nights", "LANY", "malibu"),
    ("Changes", "Justin Bieber", "changes"),
], key=lambda a: a[0].lower())
# keep the selection on the long-titled album (now sorts near the end) so it
# still demonstrates truncation / the marquee.
ALBUMS_SEL = next(i for i, a in enumerate(ALBUMS) if a[0].startswith("Rearrange"))

def screen_albums(sel=ALBUMS_SEL, title_offset=0):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Albums", "%d / %d" % (sel + 1, len(ALBUMS)), back=True)
    for r, (t, a, art) in enumerate(ALBUMS):
        s = (r == sel)
        list_row(sc, LIST_Y0, r, t, sub=a, selected=s, chip=art, rh=ROW_H2,
                 title_offset=title_offset if s else 0)
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS2, len(ALBUMS))
    return sc.img


GENRES = [
    ("Hip-Hop", 214), ("Pop", 186), ("R&B", 98), ("Indie", 74),
    ("Country", 63), ("Pop Punk", 52), ("Alternative", 47), ("Hyperpop", 39),
    ("Folk", 21), ("Rock", 17),
]
GENRES_SEL = 2

def screen_genres():
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Genres", "%d / %d" % (GENRES_SEL + 1, len(GENRES)), back=True)
    for r in range(LIST_ROWS):
        if r >= len(GENRES):
            break
        name, cnt = GENRES[r]
        list_row(sc, LIST_Y0, r, name, right=str(cnt), selected=(r == GENRES_SEL))
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS, len(GENRES))
    return sc.img


TRACKS = [
    ("Don't Understand", "3:14"),
    ("Something Real", "3:02"),
    ("Chemical", "2:46"),
    ("Novacandy", "3:38"),
    ("Mourning", "2:52"),
]
DETAIL_SEL = 1
# The album has 17 tracks (header count + meta line); only the first 5 fit, and
# the device sizes the scrollbar from the WHOLE tracklist, not the visible slice.
DETAIL_N = 17

def screen_detail(sel=DETAIL_SEL):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Albums", "%d / %d" % (sel + 1, DETAIL_N), back=True)
    sc.blit_art(12, 42, 56, "austin")
    tx = 12 + 56 + 12
    sc.text(tx, 42 + 15, "AUSTIN", FONT_HEADER, INK)
    sc.text(tx, 42 + 31, "Post Malone", FONT_SUB, MUTED_D)
    sc.text(tx, 42 + 47, "17 tracks " + MIDDOT + " 52m", FONT_SMALL, MUTED2)
    sc.fill_rect(12, 108 - 6, W - 24, 1, BORDER)
    for r, (t, dur) in enumerate(TRACKS):
        ry = 108 + r * ROW_H
        sel = (r == DETAIL_SEL)
        if sel:
            sc.fill_round_rect(6, ry + 1, W - 16, ROW_H - 2, 4, SEL_BG)
        fg = SEL_FG if sel else INK
        nc = SEL_SUB if sel else MUTED2
        num = str(r + 1)
        sc.text_right(24, ry + 15, num, FONT_SMALL, nc)
        dw = text_width(dur, bold_11)
        sc.text_right(W - 16, ry + 15, dur, bold_11, SEL_SUB if sel else MUTED_D)
        sc.text(30, ry + 15, t, FONT_HEADER if sel else FONT_ROW, fg,
                clip=(30, W - 16 - dw - 8))
    scrollbar(sc, 108, 0, 5, DETAIL_N)
    return sc.img


def _now_playing_base(vol_overlay=None, elapsed=73, total=182, locked=False):
    sc = Screen()
    # top status row
    sc.text(12, 15, "Now Playing", bold_11, INK)
    bx = W - 12 - 19
    draw_battery(sc, bx, 3, 78)
    # Persistent Hold padlock in the strip while locked (main.c: drawn just left
    # of the battery at bx-14). The shuffle token shifts left to clear it.
    if locked:
        draw_lock_glyph(sc, bx - 14, 3, INK)
    sc.text_right(bx - (18 if locked else 6), 13, "SHUF", FONT_SMALL, MUTED2)
    # art 120x120 at (16,44)
    sc.blit_art(16, 44, 120, "austin")
    mx = 16 + 120 + 14
    mr = W - 14
    # eyebrow
    eb = "TRACK   3     OF     12"
    sc.text(mx, 72, eb, FONT_SMALL, MUTED2)
    sc.text(mx, 94, "Something Real", FONT_TITLE, INK, clip=(mx, mr))
    sc.text(mx, 114, "Post Malone", FONT_SUB, MUTED_D, clip=(mx, mr))
    sc.text(mx, 130, "AUSTIN", FONT_SUB, MUTED2, clip=(mx, mr))
    def fmt(s):
        return "%d:%02d" % (s // 60, s % 60)
    sc.text(18, 198, fmt(elapsed), FONT_SUB, MUTED_D)
    sc.text_right(W - 18, 198, "-" + fmt(total - elapsed), FONT_SUB, MUTED_D)
    # progress bar
    pbx, by, bw, bh = 18, 209, W - 36, 8
    sc.fill_round_rect_aa(pbx, by, bw, bh, bh // 2, TRK)
    fw = int(elapsed * bw / total)
    if fw >= bh:
        sc.fill_round_rect_aa(pbx, by, fw, bh, bh // 2, INK)
    elif fw > 0:
        sc.fill_rect(pbx, by, fw, bh, INK)
    if vol_overlay is not None:
        volume_overlay(sc, vol_overlay)
    return sc


def draw_speaker(sc, sx, sy, c, vol):
    sc.fill_round_rect(sx - 8, sy - 3, 4, 6, 1, c)          # cabinet
    for dx in range(5):                                     # cone, opening right
        half = 2 + dx
        sc.fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c)
    if vol <= 0:                                            # muted: an X (firmware)
        for i in range(6):
            sc.fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c)   # '\'
            sc.fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c)   # '/'
        return

    def arc(R, span):                                       # skinny 1px crescents
        for dy in range(-span, span + 1):
            dxx = sc._isqrt(R * R - dy * dy)
            sc.fill_rect(sx + dxx, sy + dy, 1, 1, c)
    if vol > 5:
        arc(3, 2)
    if vol > 40:
        arc(6, 4)
    if vol > 72:
        arc(9, 5)


def volume_overlay(sc, vol):
    PX, PY, PW, PH = 60, 101, 200, 32
    sc.fill_round_rect_aa(PX, PY, PW, PH, 8, PLATE)
    draw_speaker(sc, PX + 16, PY + PH // 2, INK, vol)
    bx = PX + 34
    by = PY + PH // 2 - 3
    bw = PW - 34 - 42
    bh = 6
    sc.fill_rect(bx, by, bw, bh, TRK)
    fw = max(0, min(bw, bw * vol // 100))
    sc.fill_rect(bx, by, fw, bh, INK)
    sc.text_right(PX + PW - 14, PY + PH // 2 + 4, str(vol), bold_11, INK)


def screen_nowplaying():
    return _now_playing_base().img


def screen_volume():
    return _now_playing_base(vol_overlay=78).img


# -- lock modal --------------------------------------------------------------
def draw_ring_top(sc, cx, cy, Ro, Ri, c):
    for dy in range(-Ro, 1):
        xo = sc._isqrt(Ro * Ro - dy * dy)
        if -dy <= Ri:
            xi = sc._isqrt(Ri * Ri - dy * dy)
            sc.fill_rect(cx - xo, cy + dy, xo - xi + 1, 1, c)
            sc.fill_rect(cx + xi, cy + dy, xo - xi + 1, 1, c)
        else:
            sc.fill_rect(cx - xo, cy + dy, 2 * xo + 1, 1, c)

def draw_shackle(sc, sx, ay, Ro, Ri, lL, rL, c):
    w = Ro - Ri + 1
    draw_ring_top(sc, sx, ay, Ro, Ri, c)
    sc.fill_rect(sx - Ro, ay, w, lL, c)
    sc.fill_rect(sx + Ri, ay, w, rL, c)

def draw_keyhole(sc, kx, ky, bg):
    sc.fill_round_rect(kx - 3, ky - 3, 6, 6, 3, bg)
    sc.fill_rect(kx - 1, ky + 2, 2, 4, bg)

def draw_lock_icon(sc, cx, cy, open_, c, bg):
    if open_:
        draw_shackle(sc, cx, cy - 14, 10, 6, 6, 6, c)
    else:
        draw_shackle(sc, cx, cy - 9, 10, 6, 8, 8, c)
    sc.fill_round_rect(cx - 16, cy - 3, 32, 22, 4, c)
    draw_keyhole(sc, cx, cy + 7, bg)

def lock_plate(sc, locked):
    PX, PY, PW, PH = 70, 65, 180, 110
    plate = INK if locked else SURFACE
    fg = SURFACE if locked else INK
    if not locked:
        sc.fill_round_rect_aa(PX - 1, PY - 1, PW + 2, PH + 2, 11, BORDER)
    sc.fill_round_rect_aa(PX, PY, PW, PH, 10, plate)
    draw_lock_icon(sc, PX + PW // 2, PY + 45, not locked, fg, plate)
    label = "LOCKED" if locked else "UNLOCKED"
    sc.text_centered_at(PY + 84, PX, PW, label, FONT_HEADER, fg)


def _lock_screen(locked, elapsed=73, glyph=False):
    # `glyph` draws the persistent Hold padlock in the status strip (engaged).
    sc = _now_playing_base(elapsed=elapsed, locked=glyph)
    # add a helper for centered-in-plate text
    def centered(baseline, x, w, s, font, ink):
        sc.text(x + (w - text_width(s, font)) // 2, baseline, s, font, ink)
    sc.text_centered_at = lambda baseline, x, w, s, font, ink: centered(baseline, x, w, s, font, ink)
    lock_plate(sc, locked)
    return sc.img


def screen_lock():
    return _lock_screen(False)

def screen_locked():
    return _lock_screen(True)


# -- about -------------------------------------------------------------------
def screen_about():
    sc = Screen()
    header(sc, "About", back=True)
    sc.text_centered(62, "iPod 5.5G", FONT_TITLE, INK)
    lbl = ["SONGS", "ALBUMS", "ARTISTS"]
    val = [911, 94, 32]
    colw = W // 3
    for i in range(3):
        cx = colw * i + colw // 2
        v = str(val[i])
        sc.text(cx - text_width(v, FONT_TITLE) // 2, 100, v, FONT_TITLE, INK)
        sc.text(cx - text_width(lbl[i], FONT_SMALL) // 2, 116, lbl[i], FONT_SMALL, MUTED)
        if i:
            sc.fill_rect(colw * i, 86, 1, 36, BORDER)
    sc.fill_rect(16, 130, W - 32, 1, BORDER)
    # firmware row + Core chip
    sc.text(16, 150, "FIRMWARE", FONT_SMALL, MUTED)
    cw = text_width("Core", FONT_SUB)
    chw = cw + 16
    chx = W - 16 - chw
    sc.fill_round_rect(chx, 140, chw, 15, 7, ACCENT)
    sc.text(chx + 8, 151, "Core", FONT_SUB, SURFACE)
    # storage
    sc.text(16, 176, "STORAGE", FONT_SMALL, MUTED)
    total_mb = 76319
    free_mb = 41988
    def fmt_gb(mb):
        whole = mb // 1024
        frac = (mb % 1024) * 10 // 1024
        return "%d.%d GB" % (whole, frac)
    sc.text_right(W - 16, 176, fmt_gb(free_mb) + " free", FONT_SUB, MUTED_D)
    bx, by, bw, bh = 16, 182, W - 32, 8
    sc.fill_round_rect(bx, by, bw, bh, 4, TRK)
    used = total_mb - free_mb
    fw = int(used * bw / total_mb)
    sc.fill_round_rect(bx, by, fw, bh, 4, ACCENT)
    # battery
    pct = 78
    sc.text(16, 212, "BATTERY", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 212, str(pct) + "%", FONT_SUB, INK)
    gx, gy, gw, gh = 16, 218, W - 32 - 5, 12
    sc.fill_round_rect(gx, gy, gw, gh, 3, TRK)
    sc.fill_rect(gx + gw, gy + 3, 4, gh - 6, TRK)
    fw2 = (gw - 4) * pct // 100
    sc.fill_round_rect(gx + 2, gy + 2, fw2, gh - 4, 2, ACCENT)
    return sc.img


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def upscale(im, scale):
    return im.resize((W * scale, H * scale), Image.NEAREST)

def save_png(im, name):
    path = os.path.join(HERE, name)
    upscale(im, SCALE).save(path)
    return path


# ---------------------------------------------------------------------------
# Menu screens (main + Music submenu) — real rows from core/kernel/main.c
# ---------------------------------------------------------------------------
MAIN_MENU = [   # (label, active) — idle: "Now Playing" row is hidden
    ("Music", True), ("Playlists", False), ("Podcasts", False),
    ("Audiobooks", False), ("Settings", True),
]
MUSIC_MENU = [
    ("Playlists", False), ("Artists", True), ("Albums", True), ("Songs", True),
    ("Shuffle Songs", True), ("Genres", True), ("Composers", False),
    ("Audiobooks", False),
]

def screen_menu(title, items, sel, back):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, title, back=back)
    for i, (label, active) in enumerate(items):
        if i >= LIST_ROWS:
            break
        list_row(sc, LIST_Y0, i, label, chevron=True, selected=(i == sel),
                 greyed=not active)
    return sc.img


def build_walkthrough_gif():
    """A little "someone using the iPod" story: main menu -> Music -> Albums
    (with the long title marqueeing) -> album detail -> now playing, with the
    selection bar visibly stepping row to row and the progress bar advancing."""
    frames = []
    durations = []

    def add(im, hold=1, ms=140):
        p = upscale(im, GIF_SCALE).convert("P", palette=Image.ADAPTIVE, colors=96)
        for _ in range(hold):
            frames.append(p)
            durations.append(ms)

    # 1) MAIN MENU — dwell only on ACTIVE rows; greyed rows (Playlists/Podcasts/
    #    Audiobooks) are passed over quickly (1 frame), never selected.
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=4)   # Music (active)
    for i in (1, 2, 3):                                      # pass greyed rows
        add(screen_menu("Core", MAIN_MENU, i, False), hold=1)
    add(screen_menu("Core", MAIN_MENU, 4, False), hold=3)   # Settings (active) — pause
    for i in (3, 2, 1):                                      # pass back up
        add(screen_menu("Core", MAIN_MENU, i, False), hold=1)
    add(screen_menu("Core", MAIN_MENU, 0, False), hold=4)   # settle on Music -> enter

    # 2) MUSIC SUBMENU — step from Artists down to Albums.
    add(screen_menu("Music", MUSIC_MENU, 1, True), hold=3)  # Artists
    add(screen_menu("Music", MUSIC_MENU, 2, True), hold=4)  # Albums

    # 3) ALBUMS LIST (sorted A->Z by title) — bar steps DOWN onto the long-titled
    #    album, which now sorts near the end; the marquee then scrolls it.
    LONG = ALBUMS_SEL
    for s in range(0, LONG):                                # step down to the long one
        add(screen_albums(sel=s), hold=2 if s else 3)
    add(screen_albums(sel=LONG, title_offset=0), hold=3)    # long title (start)
    # marquee reveal
    t = ALBUMS[LONG][0]
    tx = 12 + 28 + 8
    avail = (W - 16) - tx
    max_off = max(0, text_width(t, FONT_HEADER) - avail)
    o = 0
    while o < max_off:
        o = min(max_off, o + 6)
        add(screen_albums(sel=LONG, title_offset=o), hold=1, ms=90)
    add(screen_albums(sel=LONG, title_offset=max_off), hold=3)  # dwell on tail
    add(screen_albums(sel=0), hold=3)                        # bar back to AUSTIN, select

    # 4) ALBUM DETAIL — step down a couple of tracks, settle on "Something Real".
    add(screen_detail(sel=0), hold=3)                        # Don't Understand
    add(screen_detail(sel=1), hold=2)                        # Something Real
    add(screen_detail(sel=2), hold=3)                        # Chemical
    add(screen_detail(sel=1), hold=4)                        # settle -> select

    # 5) NOW PLAYING — the selected song starts; the clock ticks up one second at
    #    a time (elapsed + -remaining = length each frame) and the bar creeps.
    for e in range(0, 6):
        add(_now_playing_base(elapsed=e).img, hold=2, ms=220)
    add(_now_playing_base(elapsed=6).img, hold=5, ms=220)   # hold a beat, then loop

    path = os.path.join(HERE, "demo.gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], loop=0,
                   duration=durations, optimize=True, disposal=2)
    return path, len(frames), sum(durations)


# ---------------------------------------------------------------------------
# Per-feature GIFs — short focused loops that pair with the still grids
# ---------------------------------------------------------------------------
def _save_gif(name, spec, colors=96):
    """spec = list of (img, hold_frames, ms). Consecutive identical frames are
    collapsed by the encoder, so holds are cheap."""
    frames, durations = [], []
    for img, hold, ms in spec:
        p = upscale(img, GIF_SCALE).convert("P", palette=Image.ADAPTIVE, colors=colors)
        for _ in range(hold):
            frames.append(p)
            durations.append(ms)
    path = os.path.join(HERE, name)
    frames[0].save(path, save_all=True, append_images=frames[1:], loop=0,
                   duration=durations, optimize=True, disposal=2)
    from PIL import Image as _I
    n = _I.open(path).n_frames
    return path, n, sum(durations)


# One consistent track across every Now Playing GIF: Something Real / Post Malone
# / AUSTIN / TRACK 3 OF 12, battery 78, SHUF — all baked into _now_playing_base.
NP_TOTAL = 182

def _np(elapsed, vol=None, theme=None, locked=False):
    """A Now Playing frame at `elapsed` seconds (elapsed + -remaining = NP_TOTAL
    every frame; the progress bar tracks elapsed). Optional volume overlay, theme
    (Onyx), and the persistent Hold padlock in the status strip (`locked`)."""
    fn = lambda: _now_playing_base(vol_overlay=vol, elapsed=elapsed,
                                   total=NP_TOTAL, locked=locked).img
    return with_palette(theme, fn) if theme else fn()


def gif_browse():
    """LIBRARY: album list (sorted A->Z by title). The selection bar steps DOWN
    through the albums and lands on the long-titled one (which sorts near the
    end); its title then marquees to reveal the tail. Loops."""
    spec = []
    LONG = ALBUMS_SEL
    for s in range(0, LONG):                                 # step down the list
        spec.append((screen_albums(sel=s), 3 if s == 0 else 2, 150))
    spec.append((screen_albums(sel=LONG, title_offset=0), 3, 150))  # land, truncated
    # marquee: dwell (above), scroll once to reveal the tail, dwell, then reset
    t = ALBUMS[LONG][0]
    tx = 12 + 28 + 8
    max_off = max(0, text_width(t, FONT_HEADER) - ((W - 16) - tx))
    o = 0
    while o < max_off:
        o = min(max_off, o + 6)
        spec.append((screen_albums(sel=LONG, title_offset=o), 1, 90))
    spec.append((screen_albums(sel=LONG, title_offset=max_off), 4, 150))  # dwell tail
    spec.append((screen_albums(sel=LONG, title_offset=0), 3, 150))        # reset -> loop
    return _save_gif("browse.gif", spec, colors=80)


def gif_volume():
    """VOLUME: the overlay ramps volume 0 -> 100 -> 0 ON TOP of a track that keeps
    playing — the clock ticks up and the progress bar creeps the whole time. The
    speaker shows the mute X at 0; wave crescents grow at >5 / >40 / >72; the fill
    bar and the percent match the volume."""
    ups = [0, 4, 10, 20, 30, 41, 50, 60, 73, 82, 92, 100]
    e0, clock_ms = 73, 0
    spec = []
    def push(vol, hold, ms):
        nonlocal clock_ms
        elapsed = e0 + clock_ms // 1000           # playback advances with GIF time
        spec.append((_np(elapsed, vol=vol), hold, ms))
        clock_ms += hold * ms
    push(0, 6, 150)                               # MUTE: speaker X (held)
    for v in ups[1:]:
        push(v, 3 if v in (41, 73) else 2, 130)   # linger as waves 2 & 3 pop in
    push(100, 4, 160)                             # all three waves, full
    for v in (82, 60, 41, 20, 4):                 # coarser down-ramp (size)
        push(v, 2, 120)
    push(0, 5, 150)                               # back to MUTE
    return _save_gif("volume.gif", spec, colors=64)


def gif_themes():
    """DUAL THEME: cross-cut the SAME playing track between Linen and Onyx. The
    track keeps playing across the cuts, so the clock ticks up at each flip."""
    spec = []
    e = 73
    for theme in (None, ONYX, None, ONYX):        # Linen / Onyx / Linen / Onyx
        spec.append((_np(e, theme=theme), 6, 170))
        e += 1                                    # ~1s hold -> +1s playback
    return _save_gif("themes.gif", spec)


def gif_lock():
    """LOCK: Now Playing (no lock) -> LOCKED modal -> the screen WHILE locked with
    the persistent padlock in the status strip -> UNLOCKED modal -> back to Now
    Playing (glyph gone). Playback keeps running, so the clock ticks up throughout
    and the padlock stays in the strip the whole time Hold is engaged."""
    e = 73
    spec = []
    spec.append((_np(e), 3, 160)); e += 1
    # Hold engaged: modal flashes, and the strip padlock appears (glyph=True).
    spec.append((_lock_screen(True, elapsed=e, glyph=True), 5, 170)); e += 1
    # Modal dismissed but still locked: the small padlock persists top-right.
    spec.append((_np(e, locked=True), 5, 170)); e += 1
    # Hold disengaged: UNLOCKED modal, and the strip padlock is gone (glyph=False).
    spec.append((_lock_screen(False, elapsed=e, glyph=False), 5, 170)); e += 1
    spec.append((_np(e), 3, 160))
    return _save_gif("lock.gif", spec)


def gif_settings():
    """SETTINGS: the Sound screen's Volume slider ramps up then back down."""
    def sound_vol(v):
        rows = list(SOUND_ROWS)
        rows[0] = ("Volume", "%d%%" % v, v, 100)
        return screen_sound(rows=rows, sel_row=0)
    vals = [20, 35, 50, 65, 80, 92]
    spec = [(sound_vol(20), 3, 150)]
    for v in vals[1:]:
        spec.append((sound_vol(v), 2, 130))
    spec.append((sound_vol(92), 3, 150))
    for v in reversed(vals[:-1]):
        spec.append((sound_vol(v), 2, 130))
    spec.append((sound_vol(20), 3, 150))
    return _save_gif("settings.gif", spec)


# ---------------------------------------------------------------------------
# Extra library / browsing screens
# ---------------------------------------------------------------------------
MAIN_MENU_FULL = [  # full menu, a track is loaded so "Now Playing" shows active
    ("Music", True), ("Playlists", False), ("Podcasts", False),
    ("Audiobooks", False), ("Settings", True), ("Now Playing", True),
]

def screen_mainmenu():
    return screen_menu("Core", MAIN_MENU_FULL, 0, back=False)

def screen_music():
    return screen_menu("Music", MUSIC_MENU, 2, back=True)   # Albums selected


# Firmware sorts Artists A->Z by ARTIST NAME (case-insensitive).
ARTISTS = sorted([
    ("Post Malone", 6), ("Justin Bieber", 5), ("The Kid LAROI", 5),
    ("Daniel Caesar", 4), ("LANY", 3), ("Morgan Wallen", 3),
    ("Juice WRLD", 2), ("Rex Orange County", 2), ("Steely Dan", 2),
    ("XXXTENTACION", 2),
], key=lambda a: a[0].lower())
ARTISTS_SEL = next(i for i, a in enumerate(ARTISTS) if a[0] == "LANY")

def screen_artists():
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Artists", "%d / %d" % (ARTISTS_SEL + 1, len(ARTISTS)), back=True)
    for r in range(LIST_ROWS):
        if r >= len(ARTISTS):
            break
        name, cnt = ARTISTS[r]
        list_row(sc, LIST_Y0, r, name, right=str(cnt), selected=(r == ARTISTS_SEL))
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS, len(ARTISTS))
    return sc.img


# Firmware sorts Songs A->Z by SONG TITLE (case-insensitive).
SONGS = sorted([
    ("Something Real", "Post Malone", "3:02"),
    ("Sunflower", "Post Malone", "2:38"),
    ("Ghost", "Justin Bieber", "2:33"),
    ("STAY", "The Kid LAROI", "2:21"),
    ("Rearrange My World / There's a Field (That's Only Yours)", "Daniel Caesar", "5:16"),
    ("Malibu Nights", "LANY", "3:48"),
], key=lambda s: s[0].lower())
# keep the long-titled song selected (demonstrates truncation)
SONGS_SEL = next(i for i, s in enumerate(SONGS) if s[0].startswith("Rearrange"))

def screen_songs():
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, "Songs", "%d / %d" % (SONGS_SEL + 1, len(SONGS)), back=True)
    for r, (t, a, dur) in enumerate(SONGS):
        list_row(sc, LIST_Y0, r, t, sub=a, right=dur, selected=(r == SONGS_SEL),
                 rh=ROW_H2, title_priority=True)
    scrollbar(sc, LIST_Y0, 0, LIST_ROWS2, len(SONGS))
    return sc.img


# An ARTIST's "All Songs" — the synthetic row at the top of a filtered album
# list opens the whole discography in title order (main.c songview_build +
# songs_render). Two things differ from the plain Songs list above: the header
# is the ARTIST, and each row's sub-line is the ALBUM. It used to repeat the
# artist, which under an artist header spent the only sub-line on the one fact
# the reader already had (main.c songs_row_draw).
ALLSONGS_ARTIST = "Post Malone"
ALLSONGS = sorted([
    ("Chemical", "AUSTIN", "2:46"),
    ("Circles", "Hollywood's Bleeding", "3:35"),
    ("Don't Understand", "AUSTIN", "3:14"),
    ("Hollywood's Bleeding", "Hollywood's Bleeding", "2:36"),
    ("I Had Some Help", "F-1 Trillion", "3:58"),
    ("Mourning", "AUSTIN", "2:52"),
    ("Novacandy", "AUSTIN", "3:38"),
    ("Pour Me a Drink", "F-1 Trillion", "3:04"),
    ("Something Real", "AUSTIN", "3:02"),
    ("Sunflower", "Hollywood's Bleeding", "2:38"),
], key=lambda s: s[0].lower())
# the track Now Playing is on, so the screens tell one story
ALLSONGS_SEL = next(i for i, s in enumerate(ALLSONGS) if s[0] == "Something Real")

def screen_allsongs(sel=ALLSONGS_SEL):
    sc = Screen()
    status_strip(sc, "CORE")
    header(sc, ALLSONGS_ARTIST, "%d / %d" % (sel + 1, len(ALLSONGS)), back=True)
    top = scroll_window(sel, len(ALLSONGS), LIST_ROWS2)
    for vr in range(LIST_ROWS2):
        r = top + vr
        if r >= len(ALLSONGS):
            break
        t, album, dur = ALLSONGS[r]
        list_row(sc, LIST_Y0, vr, t, sub=album, right=dur, selected=(r == sel),
                 rh=ROW_H2, title_priority=True)
    scrollbar(sc, LIST_Y0, top, LIST_ROWS2, len(ALLSONGS))
    return sc.img


# ---------------------------------------------------------------------------
# Settings sub-screens (no status strip — the header sits at the top band)
# ---------------------------------------------------------------------------
def _sel_bar(sc, y0, rowh, r):
    sc.fill_round_rect(6, y0 + r * rowh + 1, W - 16, rowh - 2, 4, SEL_BG)

# The root Settings list — core/ui/settings.c ROOT_L, all nine rows. Nine rows
# do not fit in the eight the panel has room for, so this list SCROLLS and
# carries a scrollbar (core/ui/screen_settings.c list_render / st_scrollbar).
ROOT_L = ["Playback", "Sound", "Theme", "Display", "Clicker", "About",
          "Boot Details", "Disk Mode", "Reset Settings"]
ROOT_SEL = 1   # Sound

def scroll_window(sel, total, visible):
    """main.c scroll_window / screen_settings.c st_scroll_window: keep the
    selection about 1/3 down the window, clamped to the ends."""
    if total <= visible:
        return 0
    return max(0, min(sel - visible // 3, total - visible))

def screen_settings(sel=ROOT_SEL):
    sc = Screen()
    header(sc, "Settings", back=True)
    right_vals = {2: "Linen", 4: "Tick"}   # Theme + Clicker carry their choice
    top = scroll_window(sel, len(ROOT_L), LIST_ROWS)
    for vr in range(LIST_ROWS):
        r = top + vr
        if r >= len(ROOT_L):
            break
        ry = LIST_Y0 + vr * ROW_H
        is_sel = (r == sel)
        if is_sel:
            _sel_bar(sc, LIST_Y0, ROW_H, vr)
        fg = SEL_FG if is_sel else INK
        rightc = SEL_SUB if is_sel else MUTED_D
        chevc = SEL_SUB if is_sel else CHEVRON
        sc.text(14, ry + 15, ROOT_L[r], FONT_HEADER if is_sel else FONT_ROW, fg)
        if r in right_vals:
            sc.text_right(W - 16, ry + 15, right_vals[r], regular_11, rightc)
        else:
            sc.text(W - 18, ry + 15, RAQUO, FONT_ROW, chevc)
    scrollbar(sc, LIST_Y0, top, LIST_ROWS, len(ROOT_L))
    return sc.img


# ---------------------------------------------------------------------------
# Boot Details (Settings > Boot Details) — core/ui/screen_settings.c
# settings_diag_render(). Where a cold boot's 3.3s actually goes.
# ---------------------------------------------------------------------------
# Phase colours are fixed literals on the device too, not palette slots: the
# segments have to stay distinguishable from each other in any theme.
C_LCD, C_DISK, C_LIB, C_RES, C_OTHER = 0x3BDB, 0x3DAD, 0xE546, 0xE125, 0x94B2

def fmt_ms(ms):
    """settings_diag_render's fmt_ms: sub-second in ms (a 40ms seek must not
    round to '0.0s'), a second or more with one decimal."""
    if ms is None:
        return "--"
    if ms < 1000:
        return "%dms" % ms
    return "%d.%ds" % (ms // 1000, (ms // 100) % 10)

# Measured on the device: a cold boot is ~3.3s, and it is the library scan that
# owns it. OTHER is NOT an input — the firmware derives it as the unattributed
# remainder (total minus the four measured phases), so it is whatever the other
# phases leave behind; setting it here would let the picture disagree with the
# figures, which is the one thing this screen must never do.
DIAG_TOTAL = 3300
DIAG_LCD, DIAG_DISK, DIAG_LIB, DIAG_RESUME = 0, 300, 1800, 900
DIAG_RES_DIR, DIAG_RES_OPEN, DIAG_RES_SEEK = 0, 700, 40
DIAG_DECODE_PCT = 61          # of the 22676 us/kframe 44.1kHz real-time budget
DIAG_SEQ = 7
DIAG_LBA = (49236472, 49236474)

def screen_diag():
    sc = Screen()
    header(sc, "Boot Details", back=True)

    # headline: label left, total right, on one line
    sc.text(16, 60, "COLD BOOT", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 63, fmt_ms(DIAG_TOTAL), FONT_TITLE, INK)

    # stacked proportional bar — same numbers as the legend, so the picture
    # cannot disagree with the figures; the remainder is drawn last, in grey.
    bx, by, bw, bh = 16, 74, W - 32, 10
    sc.fill_round_rect(bx, by, bw, bh, 5, TRK)
    segs = [(DIAG_LCD, C_LCD), (DIAG_DISK, C_DISK), (DIAG_LIB, C_LIB),
            (DIAG_RESUME, C_RES)]
    x = bx
    for ms, col in segs:
        w = ms * bw // DIAG_TOTAL
        if w <= 0:
            continue
        w = min(w, bx + bw - x)
        sc.fill_rect(x, by, w, bh, rgb565(col))
        x += w
    if x < bx + bw:
        sc.fill_rect(x, by, bx + bw - x, bh, rgb565(C_OTHER))

    # legend: six entries in two columns of three (six stacked rows ran into
    # the config block at the bottom of a 240px panel)
    known = DIAG_LCD + DIAG_DISK + DIAG_LIB + DIAG_RESUME
    other = max(0, DIAG_TOTAL - known)
    names = ["LCD", "DISK", "LIBRARY", "RESUME", "OTHER", "DECODE"]
    cols = [C_LCD, C_DISK, C_LIB, C_RES, C_OTHER, None]
    vals = [DIAG_LCD, DIAG_DISK, DIAG_LIB, DIAG_RESUME, other]
    colx, colw = (16, 168), 136
    for i in range(6):
        cx = colx[i // 3]
        y = 104 + (i % 3) * 18
        sc.fill_rect(cx, y - 7, 6, 6, rgb565(cols[i]) if cols[i] else MUTED2)
        sc.text(cx + 11, y, names[i], FONT_SMALL, MUTED)
        if i < 5:
            sc.text_right(cx + colw, y, fmt_ms(vals[i]), FONT_SUB, INK)
        else:
            # decode headroom against the real-time budget; red inside 20% of it
            sc.text_right(cx + colw, y, "%d%%" % DIAG_DECODE_PCT, FONT_SUB,
                          BATT_RED if DIAG_DECODE_PCT >= 80 else INK)
    sc.fill_rect(16, 152, W - 32, 1, BORDER)

    # the resume split, as one indented dimmer line: it is a breakdown OF the
    # RESUME entry above, not three more phases
    x = 16
    sc.text(x, 168, "RESUME", FONT_SMALL, MUTED2)
    x += text_width("RESUME", FONT_SMALL) + 10
    for lbl, ms in (("dir", DIAG_RES_DIR), ("open", DIAG_RES_OPEN),
                    ("seek", DIAG_RES_SEEK)):
        sc.text(x, 168, lbl, FONT_SMALL, MUTED2)
        x += text_width(lbl, FONT_SMALL) + 4
        v = fmt_ms(ms)
        sc.text(x, 168, v, FONT_SMALL, INK)
        x += text_width(v, FONT_SMALL) + 12
    sc.fill_rect(16, 196, W - 32, 1, BORDER)

    # settings-file locator: the two absolute LBAs config_save() writes to —
    # with no serial cable, the panel is the only place to read them back
    sc.text(16, 214, "CONFIG", FONT_SMALL, MUTED)
    sc.text_right(W - 16, 214, "seq %d" % DIAG_SEQ, FONT_SUB, INK)
    sc.text_right(W - 16, 230, "%d / %d" % DIAG_LBA, FONT_SMALL, MUTED_D)
    return sc.img


SOUND_ROWS = [
    # (label, value, num, den)
    ("Volume", "72%", 72, 100),
    ("Bass", "+3 dB", 3 + 12, 24),
    ("Treble", "0 dB", 0 + 12, 24),
    ("Balance", "Center", 0 + 100, 200),
]
SOUND_SEL = 1   # Bass (boosted)

def screen_sound(rows=None, sel_row=SOUND_SEL):
    rows = rows if rows is not None else SOUND_ROWS
    sc = Screen()
    header(sc, "Sound", back=True)
    for r, (label, val, num, den) in enumerate(rows):
        ry = LIST_Y0 + r * ROW_H
        sel = (r == sel_row)
        if sel:
            _sel_bar(sc, LIST_Y0, ROW_H, r)
        fg = SEL_FG if sel else INK
        rightc = SEL_SUB if sel else MUTED_D
        sc.text(14, ry + 11, label, FONT_HEADER if sel else FONT_ROW, fg)
        sc.text_right(W - 16, ry + 11, val, regular_11, rightc)
        # slider bar
        bx, bw, by, bh = 14, W - 16 - 14, ry + 17, 3
        sc.fill_rect(bx, by, bw, bh, SEL_TRK if sel else TRK)
        fw = max(0, min(bw, bw * num // den))
        sc.fill_rect(bx, by, fw, bh, SEL_FG if sel else INK)
    return sc.img


CLICK_L = ["Off", "Tick", "Click", "Pop", "Blip", "Tock", "Double", "Chirp"]
CLICK_ACTIVE = 1   # Tick
CLICK_SEL = 2      # Click

def screen_clicker():
    sc = Screen()
    header(sc, "Clicker", back=True)
    for r, label in enumerate(CLICK_L):
        ry = LIST_Y0 + r * ROW_H
        sel = (r == CLICK_SEL)
        if sel:
            _sel_bar(sc, LIST_Y0, ROW_H, r)
        fg = SEL_FG if sel else INK
        markc = SEL_SUB if sel else INK
        sc.text(14, ry + 15, label, FONT_HEADER if sel else FONT_ROW, fg)
        if r == CLICK_ACTIVE:
            sc.text_right(W - 16, ry + 15, MIDDOT, bold_13, markc)  # marks active
    return sc.img


TH_ROW_H = 40
TH_SWATCH = [0xF79D, 0x18C2]   # each theme's own surface tone
TH_INK = [0x18A2, 0xEF3C]      # ink hint bar
TH_NAME = ["Linen", "Onyx"]
TH_SUB = ["Warm light - text-forward", "Warm dark - terracotta"]
TH_CURRENT = 0                  # Linen active
TH_SEL = 1                      # cursor on Onyx

def screen_theme():
    sc = Screen()
    header(sc, "Theme", "2 themes", back=True)
    for r in range(2):
        ry = LIST_Y0 + r * TH_ROW_H
        sel = (r == TH_SEL)
        if sel:
            _sel_bar(sc, LIST_Y0, TH_ROW_H, r)
        sw, sx = 26, 14
        sy = ry + (TH_ROW_H - sw) // 2
        sc.fill_rect(sx - 1, sy - 1, sw + 2, sw + 2, SEL_SUB if sel else BORDER)
        sc.fill_rect(sx, sy, sw, sw, rgb565(TH_SWATCH[r]))
        sc.fill_rect(sx + 6, sy + 10, 14, 3, rgb565(TH_INK[r]))
        tx = sx + sw + 10
        fg = SEL_FG if sel else INK
        subc = SEL_SUB if sel else MUTED
        sc.text(tx, ry + 17, TH_NAME[r], FONT_HEADER, fg)
        sc.text(tx, ry + 31, TH_SUB[r], FONT_SMALL, subc)
        if r == TH_CURRENT:
            sc.text_right(W - 14, ry + 20, "CURRENT", FONT_SMALL,
                          SEL_FG if sel else MUTED2)
    return sc.img


# ---------------------------------------------------------------------------
# Onyx (warm-dark) variants — same builders under the swapped palette
# ---------------------------------------------------------------------------
def with_palette(spec, fn):
    apply_palette(spec)
    try:
        return fn()
    finally:
        apply_palette(LINEN)

def screen_albums_onyx():
    return with_palette(ONYX, lambda: screen_albums())

def screen_nowplaying_onyx():
    return with_palette(ONYX, lambda: screen_nowplaying())


# ---------------------------------------------------------------------------
# System screens: charging + boot splash
# ---------------------------------------------------------------------------
def _bolt(sc, cx, y0, bh, c):
    """Lightning bolt polygon, scanline-filled (port of screen_charging.c)."""
    pxb = [0, 0, 3, 3, 10, 6, 10]
    pyb = [0, 11, 11, 20, 8, 8, 0]
    N, box_w, box_h = 7, 10, 20
    bw = (box_w * bh) // box_h
    x0 = cx - bw // 2
    sx = [x0 + (pxb[i] * bw) // box_w for i in range(N)]
    sy = [y0 + (pyb[i] * bh) // box_h for i in range(N)]
    for y in range(y0, y0 + bh):
        xs = []
        for i in range(N):
            a, b = i, (i + 1) % N
            ya, yb, xa, xb = sy[a], sy[b], sx[a], sx[b]
            if ya == yb:
                continue
            if ya < yb:
                ylo, yhi, xlo, xhi = ya, yb, xa, xb
            else:
                ylo, yhi, xlo, xhi = yb, ya, xb, xa
            if y < ylo or y >= yhi:
                continue
            xs.append(xlo + (xhi - xlo) * (y - ylo) // (yhi - ylo))
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            L, R = xs[k], xs[k + 1]
            if R > L:
                sc.fill_rect(L, y, R - L, 1, c)

def screen_charging(pct=62, charging=True, external=True):
    CHG_BG = rgb565(0x0861)
    CHG_OUTLINE = rgb565(0x5A89)
    CHG_FILL = rgb565(0xEF3B)
    CHG_GREEN = rgb565(0x3E4D)
    CHG_RED = rgb565(0xDA46)
    CHG_TEXT = rgb565(0xEF3B)
    CHG_UNIT = rgb565(0xACF2)
    CHG_MUTED = rgb565(0x7B8D)
    BW, BH = 150, 68
    BX, BY, BT, INSET = (W - BW) // 2, 56, 3, 8
    NUB_W, NUB_H = 6, 24
    sc = Screen(CHG_BG)
    fill = CHG_GREEN if charging else (CHG_RED if pct < 20 else CHG_FILL)
    # outline (4 strokes) + softened corners
    sc.fill_rect(BX, BY, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY + BH - BT, BW, BT, CHG_OUTLINE)
    sc.fill_rect(BX, BY, BT, BH, CHG_OUTLINE)
    sc.fill_rect(BX + BW - BT, BY, BT, BH, CHG_OUTLINE)
    for cxx, cyy in ((BX, BY), (BX + BW - 1, BY), (BX, BY + BH - 1), (BX + BW - 1, BY + BH - 1)):
        sc.px[cxx, cyy] = CHG_BG
    # nub
    sc.fill_rect(BX + BW, BY + (BH - NUB_H) // 2, NUB_W, NUB_H, CHG_OUTLINE)
    # inner fill
    ix, iy = BX + INSET, BY + INSET
    iw, ih = BW - 2 * INSET, BH - 2 * INSET
    fw = max(8, min(iw, iw * max(0, min(100, pct)) // 100))
    sc.fill_rect(ix, iy, fw, ih, fill)
    if charging:
        _bolt(sc, W // 2, iy + 4, ih - 8, CHG_BG)
    # big percent + unit
    num = str(max(0, min(100, pct)))
    wn = text_width(num, bold_17)
    wu = text_width("%", bold_13)
    nx = (W - (wn + 2 + wu)) // 2
    sc.text(nx, 168, num, bold_17, CHG_TEXT)
    sc.text(nx + wn + 2, 168, "%", bold_13, CHG_UNIT)
    # status line
    if charging:
        status, sink = "CHARGING", CHG_GREEN
    elif not external:
        status, sink = "CONNECT CABLE", CHG_MUTED
    else:
        status, sink = "NOT CHARGING", CHG_MUTED
    sc.text_centered(196, status, bold_11, sink)
    return sc.img


def screen_boot():
    sc = Screen(SURFACE)
    sc.text_centered(120, "Core Player", FONT_TITLE, INK)
    sc.text_centered(142, "loading", FONT_SUB, MUTED)
    return sc.img


def main():
    outputs = []
    outputs.append(save_png(screen_albums(), "albums.png"))
    outputs.append(save_png(screen_nowplaying(), "nowplaying.png"))
    outputs.append(save_png(screen_detail(), "detail.png"))
    outputs.append(save_png(screen_genres(), "genres.png"))
    outputs.append(save_png(screen_about(), "about.png"))
    outputs.append(save_png(screen_volume(), "volume.png"))
    outputs.append(save_png(screen_lock(), "lock.png"))
    outputs.append(save_png(screen_locked(), "locked.png"))
    # --- new: library / browsing ---
    outputs.append(save_png(screen_mainmenu(), "mainmenu.png"))
    outputs.append(save_png(screen_music(), "music.png"))
    outputs.append(save_png(screen_artists(), "artists.png"))
    outputs.append(save_png(screen_songs(), "songs.png"))
    outputs.append(save_png(screen_allsongs(), "allsongs.png"))
    # --- new: settings ---
    outputs.append(save_png(screen_settings(), "settings.png"))
    outputs.append(save_png(screen_diag(), "bootdetails.png"))
    outputs.append(save_png(screen_sound(), "sound.png"))
    outputs.append(save_png(screen_clicker(), "clicker.png"))
    outputs.append(save_png(screen_theme(), "theme.png"))
    # --- new: dual theme (Onyx) ---
    outputs.append(save_png(screen_nowplaying_onyx(), "nowplaying_onyx.png"))
    outputs.append(save_png(screen_albums_onyx(), "albums_onyx.png"))
    # --- new: system ---
    outputs.append(save_png(screen_charging(), "charging.png"))
    outputs.append(save_png(screen_boot(), "boot.png"))
    # --- big walkthrough gif (unchanged) ---
    gifs = [build_walkthrough_gif()]
    # --- per-feature gifs ---
    gifs.append(gif_browse())
    gifs.append(gif_volume())
    gifs.append(gif_themes())
    gifs.append(gif_lock())
    gifs.append(gif_settings())
    for p in outputs:
        print("wrote", p, os.path.getsize(p), "bytes")
    for path, nframes, total_ms in gifs:
        print("wrote %s  %d frames, %dx%d, %.1fs, %d bytes" %
              (path, nframes, W * GIF_SCALE, H * GIF_SCALE,
               total_ms / 1000.0, os.path.getsize(path)))


if __name__ == "__main__":
    main()
