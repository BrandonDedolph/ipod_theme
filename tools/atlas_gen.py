#!/usr/bin/env python3
"""
Pre-rasterize a TTF font at a fixed pixel size into a C header
suitable for static linking. Output is two arrays + a struct
literal; see core/apps/ui/atlas.h for the C-side type definitions.

Run via tools/atlas_gen.sh which sets up the venv with Pillow.

REQUIRES PYTHON 3.12+. The glyph-label f-string below embeds a backslash inside
its expression part, which only became legal in 3.12 (PEP 701); on 3.11 this
file is a SyntaxError at import, before any argument is parsed, so the failure
looks like the tool is broken rather than the interpreter being too old. There
is an explicit version check below so the error says which it is.

All file writes here are explicitly UTF-8. The generated header carries the
literal glyph characters in its comments (smart quotes, chevrons, the middle
dot), so writing with the platform default encoding meant this tool died with a
UnicodeEncodeError under LC_ALL=C — which is exactly the environment a CI
container or a cron job runs in.

Usage:
    atlas_gen.py <ttf-path> <pixel-size> <c-symbol-name> <output.h>

Example:
    atlas_gen.py tools/fonts-src/Nunito-Regular.ttf 13 NUNITO_REGULAR_13 \\
        core/apps/ui/atlas/nunito_regular_13.h
"""
import argparse
import os
import sys

if sys.version_info < (3, 12):
    raise SystemExit(
        f"atlas_gen.py needs Python 3.12+ (found "
        f"{sys.version_info.major}.{sys.version_info.minor}): it uses a "
        f"backslash inside an f-string expression, legal only since PEP 701.")

from PIL import Image, ImageDraw, ImageFont

# The generated headers declare `uint16_t data_offset`, so the concatenated
# glyph bitmap cannot exceed 64 KiB — past that the offsets wrap and every
# glyph after the wrap point renders as a slice of some other glyph's bitmap.
# Nothing checked this; a large face at a large pixel size would have produced
# a header that compiles cleanly and draws garbage.
DATA_OFFSET_MAX = 0xFFFF

# Advance fixed-point scale. MUST match ATLAS_ADV_SHIFT/ATLAS_ADV_ONE in
# core/ui/atlas.h — the generated headers are consumed directly by the device
# renderer, so a mismatch here silently rescales all text spacing.
ADV_SHIFT = 6
ADV_ONE = 1 << ADV_SHIFT
ADVANCE_MAX = 0xFFFF                 # the uint16_t field it is emitted into

# Kerning. Stored per PAIR in 1/32 px in an int8_t, so the representable range
# is +/-3.97px (worst real pair is bold-17 "LT" at -2.21px) at 0.03px
# resolution. A pair is only emitted when |kern| reaches KERN_MIN_PX — below
# that it cannot change a rounded pixel position often enough to be worth the
# ROM.
KERN_SHIFT = 5
KERN_ONE = 1 << KERN_SHIFT
# 0.25px. Measured trade at regular-13 across real album/artist strings:
#   threshold   ASCII pairs   worst string-width error
#     0.2500        640            0.66px
#     0.1250       1116            0.66px
#     0.0625       1394            0.41px
# Every pair big enough to change a visible gap is >= 0.25px, so the tighter
# thresholds buy only sub-pixel centring accuracy for roughly double the ROM
# (~+50KB across the seven atlases). The residual shows up as string width,
# never as the uneven letter gaps this was fixing.
KERN_MIN_PX = 0.25
KERN_ADJ_MIN, KERN_ADJ_MAX = -128, 127

# Letter-spacing added between glyphs, in PIXELS, per pixel size.
#
# Nunito's antialiased ink is wider than its advances at these sizes — measured
# mean side bearing is negative everywhere we ship (9px -0.36, 11px -0.13,
# 13px -0.21), i.e. glyphs touch before any spacing logic is involved. These
# values restore roughly half a pixel of daylight between letters. Tune here;
# the device just adds the number.
# Keyed by (is_bold, px) — NOT by size alone. Bold is fitted tighter than
# regular at the same size (design RSB 0.040em vs 0.046em) on top of carrying
# heavier stems, so keying on size under-tracks bold 11/13.
#
# Values chosen against a measured criterion rather than by eye: over ~600
# adjacent letter pairs from 53 real title/artist/menu strings, put fewer than
# ~3% of pairs' >=50%-alpha glyph CORES in contact, which lands the soft
# (any-alpha) mean gap around +0.4..+0.7px. Targeting a +1.0px mean instead
# would need 1.2-1.7px of tracking and reads as deliberately letterspaced.
TRACKING_PX = {
    (False,  9): 0.9, (False, 11): 0.9, (False, 13): 0.7,
    (True,   9): 0.9, (True,  11): 0.9, (True,  13): 0.9, (True, 17): 0.6,
}
TRACKING_DEFAULT = 0.7

PRINTABLE = range(0x20, 0x7F)  # 0x20..0x7E inclusive — 95 glyphs

# Non-ASCII glyphs appended AFTER the 95 ASCII glyphs, at fixed indices 95, 96,
# 97 … . The firmware maps a Unicode codepoint to its index via the generated
# core/ui/atlas/glyphmap.h (built from this same list, so the two never drift),
# plus three legacy private single-byte codes 0x01/0x02/0x03 for ‹ › · that
# predate UTF-8 decoding (see core/ui/text.c glyph_index()). Order is
# load-bearing: it fixes each glyph's index, which glyphmap.h then records.
#
# Coverage: the three Linen chevron/dot glyphs, the smart-punctuation that shows
# up in real music metadata (curly quotes, en/em dash, ellipsis), and the whole
# printable Latin-1 Supplement (À-ÿ and friends) so Western artist/album names
# with accents render true instead of being stripped.
EXTRAS = [
    (0x2039, "‹"),   # index 95: ‹  single left  angle quote  (also code 0x01)
    (0x203A, "›"),   # index 96: ›  single right angle quote  (also code 0x02)
    (0x00B7, "·"),   # index 97: ·  middle dot                (also code 0x03)
    # smart punctuation common in tags/filenames
    (0x2018, "‘"), (0x2019, "’"),          # ‘ ’  single curly quotes
    (0x201C, "“"), (0x201D, "”"),          # “ ”  double curly quotes
    (0x2013, "–"), (0x2014, "—"),          # – —  en / em dash
    (0x2026, "…"),                          # …    horizontal ellipsis
]
# Printable Latin-1 Supplement (0xA1..0xFF), minus 0xAD SOFT HYPHEN (non-printing)
# and 0xB7 MIDDLE DOT (already an extra above — avoid a duplicate glyph/index).
EXTRAS += [(cp, chr(cp)) for cp in range(0xA1, 0x100) if cp not in (0xAD, 0x00B7)]


def write_glyphmap(out_dir: str) -> None:
    """Emit core/ui/atlas/glyphmap.h: the codepoint -> glyph-index table the
    firmware binary-searches for any non-ASCII char. Font-independent (every
    atlas shares the same glyph order), so it is rewritten identically on each
    run. Sorted by codepoint for bsearch."""
    base = 95  # first EXTRAS index (ASCII 0x20..0x7E occupy 0..94)
    rows = sorted((cp, base + i) for i, (cp, _ch) in enumerate(EXTRAS))
    out = []
    out.append("// glyphmap.h — auto-generated by tools/atlas_gen.py.")
    out.append("// Codepoint -> atlas glyph index for non-ASCII glyphs, sorted")
    out.append("// by codepoint. Edit the generator's EXTRAS, not this file.")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("typedef struct { uint16_t cp; uint8_t idx; } atlas_cpmap_t;")
    out.append("")
    out.append(f"static const atlas_cpmap_t ATLAS_CPMAP[{len(rows)}] = {{")
    for cp, idx in rows:
        try:
            ch = chr(cp)
        except ValueError:
            ch = "?"
        out.append(f"    {{ 0x{cp:04X}, {idx:3d} }},   // '{ch}'")
    out.append("};")
    out.append(f"#define ATLAS_CPMAP_N {len(rows)}")
    out.append("")
    path = os.path.join(out_dir, "glyphmap.h")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    sys.stderr.write(f"wrote {path}\n")


def kern_pairs(font, chars):
    """Kerning for every ordered pair of the glyphs we ship, in 1/32 px.

    Measured as len(ab) - len(a) - len(b) through the SAME getlength() that
    produces the advances, with Raqm layout doing the font's own shaping. That
    is deliberate: reading GPOS separately would be a second source of truth
    that could disagree with the advances we baked.

    Returns [(left_idx, right_idx, adj32)] sorted by (left, right) so the
    device can binary-search it.
    """
    single = {c: font.getlength(c) for c in chars}
    out = []
    for li, a in enumerate(chars):
        for ri, b in enumerate(chars):
            k = font.getlength(a + b) - single[a] - single[b]
            if abs(k) < KERN_MIN_PX:
                continue
            adj = int(round(k * KERN_ONE))
            if adj == 0:
                continue
            if adj < KERN_ADJ_MIN or adj > KERN_ADJ_MAX:
                raise SystemExit(
                    f"kern {a!r}{b!r} = {k:.3f}px does not fit int8 1/32px "
                    f"(adj {adj}); widen the field")
            out.append((li, ri, adj))
    out.sort(key=lambda e: (e[0], e[1]))
    return out


def render_atlas(ttf_path: str, px_size: int, symbol: str) -> str:
    # Raqm applies the font's kerning in getlength(); the basic layout engine
    # does not, and would silently emit an empty kern table.
    font = ImageFont.truetype(ttf_path, px_size,
                              layout_engine=ImageFont.Layout.RAQM)
    ascent, descent = font.getmetrics()

    glyph_data = bytearray()
    glyphs = []   # list of (offset_x, offset_y, w, h, advance, data_offset)

    def add_glyph(ch):
        bbox = font.getbbox(ch)  # (left, top, right, bottom) — top<0 = above baseline
        # 26.6 fixed point (1/64 px), NOT whole pixels. Rounding each glyph's
        # advance to an integer here is what made letter spacing look uneven:
        # at 9-13px a true advance is routinely a half pixel, so neighbouring
        # gaps in one word disagreed by ~1px and the error accumulated along
        # the string. The device pen carries the fraction (core/ui/atlas.h,
        # ATLAS_ADV_*). Costs nothing: the glyph struct was 7 bytes padded to
        # 8 and is still 8.
        advance = int(round(font.getlength(ch) * ADV_ONE))

        if not bbox or (bbox[2] - bbox[0]) <= 0 or (bbox[3] - bbox[1]) <= 0:
            # Whitespace / no ink. Record an empty glyph with just an advance.
            glyphs.append((0, 0, 0, 0, advance, len(glyph_data)))
            return

        offset_x = int(bbox[0])
        offset_y = int(bbox[1])  # negative for ascenders
        w = int(bbox[2] - bbox[0])
        h = int(bbox[3] - bbox[1])

        # Render onto a fresh L-mode image at glyph-bbox size. Origin
        # for ImageDraw.text is the pen baseline; we shift by -bbox to
        # land the visible ink at (0, 0) of the bitmap.
        img = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((-offset_x, -offset_y), ch, font=font, fill=255)
        bytes_ = img.tobytes()
        assert len(bytes_) == w * h, \
            f"unexpected byte count: {len(bytes_)} vs {w}*{h}"

        data_offset = len(glyph_data)
        glyph_data.extend(bytes_)
        glyphs.append((offset_x, offset_y, w, h, advance, data_offset))

    for codepoint in PRINTABLE:
        add_glyph(chr(codepoint))
    for _cp, ch in EXTRAS:
        add_glyph(ch)

    # Every data_offset must fit the uint16_t field it is generated into.
    if len(glyph_data) > DATA_OFFSET_MAX:
        raise SystemExit(
            f"{symbol}: glyph bitmap data is {len(glyph_data)} bytes, past the "
            f"{DATA_OFFSET_MAX} the generated `uint16_t data_offset` field can "
            f"address. Offsets past that point would wrap and every later "
            f"glyph would draw a slice of the wrong bitmap — silently, at "
            f"runtime. Use a smaller pixel size, trim EXTRAS, or widen "
            f"data_offset in core/ui/atlas.h (and the generator).")

    # Output as a C header.
    out = []
    out.append(f"// {symbol} — auto-generated by tools/atlas_gen.py.")
    out.append(f"// Source: {os.path.basename(ttf_path)} @ {px_size}px")
    out.append("// Edit the generator, not this file.")
    out.append("")
    out.append("#include \"../atlas.h\"")
    out.append("")
    out.append(f"static const uint8_t {symbol}_DATA[] = {{")
    # Pack 16 bytes per line for diff-friendliness.
    for i in range(0, len(glyph_data), 16):
        line = ", ".join(f"0x{b:02x}" for b in glyph_data[i:i + 16])
        out.append(f"    {line},")
    out.append("};")
    out.append("")
    total = 95 + len(EXTRAS)
    out.append(f"static const atlas_glyph_t {symbol}_GLYPHS[{total}] = {{")
    for i, (ox, oy, w, h, adv, off) in enumerate(glyphs):
        if i < 95:
            cp = 0x20 + i
            label = f"0x{cp:02X} '{chr(cp) if cp != 0x5c else '\\'}'"
            idx = cp - 0x20
        else:
            uni, ch = EXTRAS[i - 95]
            label = f"U+{uni:04X} '{ch}' (extra)"
            idx = i
        out.append(
            f"    [{idx:2d}] = {{ .data_offset = {off:5d}, "
            f".w = {w:3d}, .h = {h:3d}, "
            f".offset_x = {ox:3d}, .offset_y = {oy:3d}, "
            f".advance = {adv:5d} }},   // {label}"
        )
    out.append("};")
    out.append("")

    # Kern table: sorted (left, right) so the device binary-searches it.
    chars = [chr(cp) for cp in PRINTABLE] + [ch for _cp, ch in EXTRAS]
    kerns = kern_pairs(font, chars)
    out.append(f"/* {len(kerns)} kern pairs, adj in 1/32 px, sorted by")
    out.append(" * (left<<8)|right for binary search. */")
    if kerns:
        out.append(f"static const atlas_kern_t {symbol}_KERN[{len(kerns)}] = {{")
        for li, ri, adj in kerns:
            lc = chars[li]
            rc = chars[ri]
            out.append(
                f"    {{ {li:3d}, {ri:3d}, {adj:4d} }},"
                f"   // '{lc}''{rc}'  {adj / KERN_ONE:+.2f}px")
        out.append("};")
    else:
        out.append(f"static const atlas_kern_t {symbol}_KERN[1] = {{ {{0,0,0}} }};")
    out.append("")

    # Line height: ascent + descent + a hair of leading.
    line_h = ascent + descent
    out.append(f"const atlas_t {symbol} = {{")
    out.append(f"    .glyphs      = {symbol}_GLYPHS,")
    out.append(f"    .data        = {symbol}_DATA,")
    out.append(f"    .kern        = {symbol}_KERN,")
    out.append(f"    .kern_n      = {len(kerns)},")
    # CORE_TRACKING_PX overrides the table, for sweeping candidate values
    # against tools/text_preview.c without editing this file.
    _ovr = os.environ.get("CORE_TRACKING_PX")
    _bold = "BOLD" in symbol.upper()
    _tpx = (float(_ovr) if _ovr
            else TRACKING_PX.get((_bold, px_size), TRACKING_DEFAULT))
    track = int(round(_tpx * ADV_ONE))
    out.append(f"    .tracking    = {track},"
               f"   // {track / ADV_ONE:+.2f}px letter-spacing")
    out.append(f"    .ascent      = {ascent},")
    out.append(f"    .descent     = {descent},")
    out.append(f"    .line_height = {line_h},")
    out.append("};")
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("ttf")
    ap.add_argument("px_size", type=int)
    ap.add_argument("symbol")
    ap.add_argument("output")
    args = ap.parse_args()

    contents = render_atlas(args.ttf, args.px_size, args.symbol)
    out_dir = os.path.dirname(args.output) or "."
    os.makedirs(out_dir, exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(contents)
    sys.stderr.write(f"wrote {args.output}\n")
    write_glyphmap(out_dir)                 # font-independent; rewritten each run
    return 0


if __name__ == "__main__":
    sys.exit(main())
