/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Boot bring-up (proven on real 5.5G hardware): UART banner, 30 MHz clock,
 * unified cache, 100 Hz tick + IRQs, then — if the BCM LCD is powered — the
 * disk/audio stack. From there kernel_main mounts the FAT32 volume and runs the
 * menu UI: a main menu (Music / … / Now Playing), a Music sub-menu, and an
 * album/folder browser that streams the selected track through the background
 * player (core/player/player.c) and out the DMA-fed DAC. MENU pops screens; a
 * playing track keeps going while you navigate.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hw/backlight.h"
#include "hw/battery.h"
#include "hw/i2c.h"
#include "hw/power.h"
#include "hw/piezo.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "../player/player.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "../ui/text.h"
#include "../ui/thumb.h"
#include "../ui/artcache.h"
#include "../ui/screen_charging.h"
#include "../ui/settings.h"
#include "../ui/palette.h"
#include "hw/volume.h"

/*
 * Idle-task CPU sleep. Program the per-core countdown to wake this core
 * after ~`ms` milliseconds and halt until then (01-soc-pp5022.md, "Sleep
 * / wake"). We use PROC_WAIT_CNT (self-wakes on the countdown), NOT
 * PROC_SLEEP — with no interrupt controller installed yet, sleep-until-
 * interrupt would never wake. Only the CPU runs the kernel in Phase 1,
 * so CPU_CTL is correct. Three NOPs after the write per the doc's
 * pipeline rule.
 */
static void cpu_wait_ms(uint8_t ms) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | ms);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/* ---------------------------------------------------------------------------
 * Linen theme + Nunito faces
 * ------------------------------------------------------------------------- */

/* Theme palette. The tokens keep their LINEN_ names but now resolve to the
 * live, theme-selected palette (ui/palette.h): g_pal[] is swapped as a block by
 * theme_set() when Settings -> Theme changes (Linen / Onyx dark). The names are
 * historical — under Onyx these carry the dark values. SEL_BG/SEL_FG stay
 * derived from INK/SURFACE so the inverted selection bar reads right in both. */
#define LINEN_SURFACE g_pal[PAL_SURFACE]
#define LINEN_INK     g_pal[PAL_INK]
#define LINEN_MUTED   g_pal[PAL_MUTED]
#define LINEN_ACCENT  g_pal[PAL_ACCENT]
#define LINEN_BORDER  g_pal[PAL_BORDER]
#define LINEN_SEL_BG  g_pal[PAL_INK]     /* selection bar = ink (inverts w/ theme)*/
#define LINEN_SEL_FG  g_pal[PAL_SURFACE] /* selection text = surface              */
#define LINEN_MUTED2  g_pal[PAL_MUTED2]
#define LINEN_MUTED_D g_pal[PAL_MUTED_D]
#define LINEN_SEL_SUB g_pal[PAL_SEL_SUB]
#define LINEN_CHEVRON g_pal[PAL_CHEVRON]
#define LINEN_SB_TRK  g_pal[PAL_SB_TRK]
#define LINEN_SB_THMB g_pal[PAL_SB_THMB]
#define LINEN_PLATE   g_pal[PAL_PLATE]   /* raised plate (volume overlay)         */
#define LINEN_TRK     g_pal[PAL_TRK]     /* slider / meter track                  */

/* Nunito faces (freestanding renderer, core/ui/text.h). */
#define FONT_HEADER   text_font_bold_13()
#define FONT_ROW      text_font_regular_13()
#define FONT_TITLE    text_font_bold_17()
#define FONT_SUB      text_font_regular_11()
#define FONT_SMALL    text_font_regular_9()

/* Draw a NUL-terminated string; thin wrapper over text_draw with the panel
 * dimensions baked in. `y` is the text baseline. Returns the advance. */
static int ui_text(int x, int y, const char *s, const text_font_t *font,
                   uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Centre a string horizontally at baseline `y`. */
static void ui_text_centered(int y, const char *s, const text_font_t *font,
                             uint16_t ink)
{
    int w = text_width(s, font);
    ui_text((LCD_WIDTH - w) / 2, y, s, font, ink);
}

/* Filled rounded rectangle (radius `r`) — the design's selection bars (r=4) and
 * plates (r=6) are rounded, not square; this replaces the hard console_fill_rect
 * at those spots. Each corner row is inset along a quarter-circle. */
static int isqrt_i(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r < 1) { console_fill_rect(x, y, w, h, c); return; }
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    for (int ry = 0; ry < h; ry++) {
        int inset = 0, k = -1;
        if (ry < r)            k = ry;
        else if (ry >= h - r)  k = h - 1 - ry;
        if (k >= 0) {
            int dy = r - k;
            inset = r - isqrt_i(r * r - dy * dy);
        }
        console_fill_rect(x + inset, y + ry, w - 2 * inset, 1, c);
    }
}

/* Linear RGB565 blend: fg over bg by alpha a (0..256). Cheap (no gamma) — fine
 * for the subtle 1px anti-alias fringe on modal corners. */
static uint16_t blend565(uint16_t bg, uint16_t fg, int a)
{
    int inv = 256 - a;
    int r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) >> 8;
    int g = (((fg >>  5) & 0x3F) * a + ((bg >>  5) & 0x3F) * inv) >> 8;
    int b = (( fg        & 0x1F) * a + ( bg        & 0x1F) * inv) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Anti-aliased filled rounded rect: solid interior + straight edges, with the
 * four corner quadrants super-sampled (4x4) so their boundary pixels blend into
 * whatever is already in the framebuffer — smooth corners instead of the integer
 * stair-steps of fill_round_rect. Reads the FB, so the background under the
 * corners must already be drawn (true for the modals). Costlier than the plain
 * version, so it's used only for the volume/lock plates (drawn on events). */
static void fill_round_rect_aa(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r < 1) { console_fill_rect(x, y, w, h, c); return; }
    if (2 * r > w) r = w / 2;
    if (2 * r > h) r = h / 2;
    console_fill_rect(x, y + r, w, h - 2 * r, c);          /* solid middle band  */
    uint16_t *fb = console_fb();
    const int S = 4, cN = r * 2 * S;                       /* circle centre, x2S */
    for (int ry = 0; ry < r; ry++) {
        console_fill_rect(x + r, y + ry,         w - 2 * r, 1, c);   /* top edge */
        console_fill_rect(x + r, y + h - 1 - ry, w - 2 * r, 1, c);   /* bot edge */
        for (int rx = 0; rx < r; rx++) {
            int inside = 0;
            for (int sy = 0; sy < S; sy++) {
                int dy = ry * 2 * S + sy * 2 + 1 - cN;
                for (int sx = 0; sx < S; sx++) {
                    int dx = rx * 2 * S + sx * 2 + 1 - cN;
                    if (dx * dx + dy * dy <= cN * cN) inside++;
                }
            }
            if (inside == 0) continue;
            int a = inside * 256 / (S * S);
            int xs[2] = { x + rx, x + w - 1 - rx };
            int ys[2] = { y + ry, y + h - 1 - ry };
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++) {
                    int px = xs[i], py = ys[j];
                    if (px < 0 || py < 0 || px >= LCD_WIDTH || py >= LCD_HEIGHT) continue;
                    uint16_t *d = &fb[py * LCD_WIDTH + px];
                    *d = (a >= 256) ? c : blend565(*d, c, a);
                }
        }
    }
}

/* Marquee: a single "currently-scrolling" text target (the selected overflowing
 * row, or the now-playing title). Render fns set it while drawing; the main loop
 * scrolls it in place via a tiny partial present. `text` points at our own
 * copy (g_mq.last), so callers may pass a transient buffer safely. */
static struct {
    int                active, x, y, w;
    const char        *text;
    const text_font_t *font;
    uint16_t           ink, bg;
    uint32_t           t0;                 /* phase clock origin (target start)   */
    char               last[72];           /* content fingerprint of that target  */
    int                last_x, last_y;
    int                cy0, cy1;           /* vertical clip (keeps it in its row) */
} g_mq;

/* Marquee cadence: dwell showing the truncated start, scroll once to reveal the
 * tail, dwell on the tail, then reset to the start and repeat. `t_us` is time
 * SINCE the target was registered (g_mq.t0), so it always begins from the
 * truncated look rather than mid-scroll, and — because callers draw the title
 * through this at the SAME live offset — a full repaint never shows a truncated
 * frame that the scroll loop then has to correct (no wheel/lock stutter). */
#define MQ_US_PER_PX  26000u              /* ~38 px/s scroll                     */
#define MQ_HOLD_START 1500000u            /* dwell on the truncated start (1.5s) */
#define MQ_HOLD_END    900000u            /* dwell on the revealed tail   (0.9s) */

/* Current scroll offset (px) for overflowing text `tw` wide in a `w` window at
 * elapsed phase time `t_us`. 0 while it fits. */
static int mq_offset(int tw, int w, uint32_t t_us)
{
    if (tw <= w) return 0;
    int max_off = tw - w;
    uint32_t scroll = (uint32_t)max_off * MQ_US_PER_PX;
    uint32_t cycle  = MQ_HOLD_START + scroll + MQ_HOLD_END;
    uint32_t tp = t_us % cycle;
    int off;
    if (tp < MQ_HOLD_START) {
        off = 0;                                      /* truncated (start)      */
    } else if (tp < MQ_HOLD_START + scroll) {
        off = (int)((tp - MQ_HOLD_START) / MQ_US_PER_PX);
    } else {
        off = max_off;                                /* hold on the tail       */
    }
    if (off > max_off) off = max_off;
    return off;
}

/* Draw `s` in the window [x, x+w) at baseline y. Clears the text band to bg
 * first, then draws it at its current scroll offset (`t_us` = phase time). */
static void draw_marquee(int x, int y, int w, const char *s,
                         const text_font_t *font, uint16_t ink, uint16_t bg,
                         uint32_t t_us, int cy0, int cy1)
{
    /* Clear only the glyph ink box (ascent+descent), NOT the full line_height —
     * line_height includes leading that reaches into a row's sub-line below, so
     * a scrolling title would otherwise erase the artist text under it. The
     * [cy0, cy1) window keeps the clear AND the glyphs inside the row, so tall
     * glyph tops (baseline sits close to the bar top) can't paint above the
     * selection bar into the row above. */
    int asc = text_ascent(font);
    int top = y - asc, bot = y + text_descent(font);
    if (top < cy0) top = cy0;
    if (bot > cy1) bot = cy1;
    if (bot > top) console_fill_rect(x, top, w, bot - top, bg);
    int off = mq_offset(text_width(s, font), w, t_us);
    text_draw_clip_v(console_fb(), LCD_WIDTH, LCD_HEIGHT, x - off, y, s, font, ink,
                     x, x + w, cy0, cy1);
}

/* Register the selected/overflowing text as the marquee target (only if it
 * actually overflows its window). Resets the phase clock (g_mq.t0) when the
 * target changes — compared by CONTENT + position, since the now-playing title
 * reuses one buffer across tracks so a pointer check would miss the change — so
 * the scroll always restarts from the truncated look on a new row/track. */
static void mq_set(int x, int y, int w, const char *text,
                   const text_font_t *font, uint16_t ink, uint16_t bg,
                   int cy0, int cy1)
{
    if (text_width(text, font) <= w) return;

    int changed = (x != g_mq.last_x || y != g_mq.last_y);
    if (!changed) {
        int i = 0;
        while (i < (int)sizeof g_mq.last - 1 && text[i] && text[i] == g_mq.last[i]) {
            i++;
        }
        changed = (text[i] != g_mq.last[i]);
    }
    if (changed) {
        int i = 0;
        for (; i < (int)sizeof g_mq.last - 1 && text[i]; i++) g_mq.last[i] = text[i];
        g_mq.last[i] = '\0';
        g_mq.last_x = x; g_mq.last_y = y;
        g_mq.t0 = mmio_read32(USEC_TIMER_ADDR);
    }
    g_mq.active = 1; g_mq.x = x; g_mq.y = y; g_mq.w = w;
    /* Point at our OWN copy (g_mq.last), not the caller's buffer: some callers
     * (e.g. the album list) build the row text in a per-iteration stack local,
     * which is dead by the time the scroll tick re-reads it — a dangling pointer
     * that painted a different album's name. last[] is static and always holds
     * the current target's content, so the periodic redraw stays valid. */
    g_mq.text = g_mq.last; g_mq.font = font; g_mq.ink = ink; g_mq.bg = bg;
    g_mq.cy0 = cy0; g_mq.cy1 = cy1;
}

/* Draw `text` in [x, x+w) at baseline y AND register it as the marquee target.
 * Overflowing text is rendered at its current scroll offset (not a static
 * offset-0), so a full repaint mid-scroll shows the live position rather than a
 * truncated frame the scroll loop then jumps to correct. Non-overflowing text
 * just draws normally. Clears the text band to bg first. */
static void mq_text(int x, int y, int w, const char *text,
                    const text_font_t *font, uint16_t ink, uint16_t bg,
                    int cy0, int cy1)
{
    if (text_width(text, font) <= w) {
        /* Fits: just draw it. NO background band clear — the caller already
         * painted the row/selection background, and a clear at y-ascent would
         * bleed ABOVE the selection bar (ascent 14 > the sub-row title lift of
         * 11) and paint a black bar over the row above. */
        text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, text, font, ink,
                       x, x + w);
        return;
    }
    mq_set(x, y, w, text, font, ink, bg, cy0, cy1);
    draw_marquee(x, y, w, text, font, ink, bg,
                 mmio_read32(USEC_TIMER_ADDR) - g_mq.t0, cy0, cy1);
}

/* ---------------------------------------------------------------------------
 * File / album browser
 *
 * browse_entry_t + BROWSE_MAX/NAME_MAX are shared with the player (player.h):
 * a folder's worth of entries is copied into the player as its queue.
 * ------------------------------------------------------------------------- */

static browse_entry_t g_browse[BROWSE_MAX];
static int            g_browse_n;

/* Browser navigation. The album LIST (depth 0) is index-driven — see g_albums
 * below — while the TRACKLIST (depth 1) is a live folder read into g_browse.
 * Albums are flat on disk (import flattens multi-disc), so the browser is only
 * ever two levels: g_dir_depth is 0 (albums) or 1 (an album's tracks).
 * g_cur_dir is the cluster of the album whose tracks are currently listed. */
static uint32_t g_cur_dir;
static int      g_dir_depth;

/* Index-derived album list — the source of truth is CORELIB.IDX, so an album
 * appears here only if the index references it. Stale/orphan or differently
 * named folders left on disk by an old import simply never show (which is what
 * kept "Kid Laroi" and "The Kid LAROI" from splitting into two entries). Built
 * once by library_ensure, sorted A->Z; g_albumview is the on-screen slice
 * (all, or one artist's). */
#define LIB_MAX_ALBUMS 256
typedef struct {
    char     folder[NAME_MAX + 1];
    uint32_t clus;                       /* album folder cluster                  */
    uint32_t art_clus, art_size;         /* folder.art (120x120) — now-playing    */
    uint32_t thm_clus, thm_size;         /* folder.thm (28x28)  — list chip       */
} lib_album_t;
static lib_album_t g_albums[LIB_MAX_ALBUMS];
static int         g_albums_n;
static uint16_t    g_albumview[LIB_MAX_ALBUMS];
static int         g_albumview_n;

/* Album art (folder.art) location captured while enumerating the current
 * folder; handed to player_play_queue so the player owns/validates it. */
static uint32_t g_art_clus, g_art_size;

/* When set, the album list (depth 0) only shows folders whose "Artist - Album"
 * name has this artist prefix — the Artists → one-artist's-albums drill-down.
 * Empty means the plain "Albums" list (every folder). */
static char g_artist_filter[NAME_MAX + 1];

/* Output volume (WM8758 codec gain, 0..100) + how long the on-screen volume
 * overlay stays up after the last wheel tick (volume-demo.jsx: ~1.5 s then fade). */
static int      g_volume = 70;
static uint32_t g_vol_show_until;

/* Case-insensitive ASCII match of a dirent name against a literal. */
static int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Junk-filter for the album list: skip iPod/OS system folders and any dotfolder
 * (.Trashes, .Spotlight-V100, .fseventsd, …) so only music folders show. */
static int is_junk_dir(const char *name)
{
    if (name[0] == '.') {
        return 1;
    }
    static const char *const junk[] = {
        "iPod_Control", "Calendars", "Contacts", "Photos", "Recordings",
        "Notes", "System Volume Information", "$RECYCLE.BIN", "LOST.DIR",
        "Find My iPod",
    };
    for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
        if (name_eq_ci(name, junk[i])) {
            return 1;
        }
    }
    return 0;
}

/* Copy `src` into `dst` (<= NAME_MAX bytes), keeping printable ASCII AND UTF-8
 * multibyte bytes (the atlas now covers Latin-1 + smart punctuation, and the FAT
 * reader hands us real UTF-8) — only C0 control bytes (0x00..0x1F, incl. the
 * legacy 0x01 placeholder) are dropped. If `drop_ext`, trim a trailing ".ext".
 * Truncation is byte-bounded; a split multibyte tail just renders as one U+FFFD. */
static void copy_display_name(char *dst, const char *src, int drop_ext)
{
    int end = 0;
    while (src[end]) end++;
    if (drop_ext) {
        int dot = -1;
        for (int j = 0; src[j]; j++) {
            if (src[j] == '.') dot = j;
        }
        if (dot > 0) end = dot;              /* trim the extension */
    }
    int i = 0;
    for (int j = 0; j < end && i < NAME_MAX; j++) {
        unsigned char c = (unsigned char)src[j];
        if (c >= 0x20) {                     /* keep ASCII + all UTF-8 bytes */
            dst[i++] = (char)c;
        }
    }
    dst[i] = '\0';
}

/* Decode one UTF-8 sequence at *p, advance past it, return the codepoint (-1 at
 * NUL). Malformed bytes yield one byte of progress so a bad name can't stall. */
static int mn_utf8_next(const unsigned char **p)
{
    unsigned char c = **p;
    if (c == 0) return -1;
    if (c < 0x80) { (*p)++; return c; }
    int n, cp;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*p)++; return 0xFFFD; }
    const unsigned char *q = *p + 1;
    for (int i = 0; i < n; i++) {
        if ((q[i] & 0xC0) != 0x80) { (*p)++; return 0xFFFD; }
        cp = (cp << 6) | (q[i] & 0x3F);
    }
    *p += n + 1;
    return cp;
}

/* Case/quote-folded FNV-1a-32 over a UTF-8 name — the on-disk locator that binds
 * an index record to its file/folder without depending on byte-exact names
 * (quote-style drift can't break a match). MUST stay byte-identical to
 * tools/build_index.py name_hash(): fold smart quotes/dashes to ASCII, lowercase
 * A-Z, then FNV-1a over the re-encoded UTF-8 bytes. */
static uint32_t name_hash(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t h = 0x811c9dc5u;
    for (;;) {
        int cp = mn_utf8_next(&p);
        if (cp < 0) break;
        if      (cp == 0x2018 || cp == 0x2019) cp = '\'';
        else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        unsigned char b[4]; int n;
        if      (cp < 0x80)  { b[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800) { b[0] = (unsigned char)(0xC0 | (cp >> 6));
                               b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
        else                 { b[0] = (unsigned char)(0xE0 | (cp >> 12));
                               b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                               b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
        for (int i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
    }
    return h;
}

/* Split a "Artist - Album" folder name on its first " - " separator (the loader
 * names album folders this way). No separator -> artist empty, album = whole
 * name. Both outputs NAME_MAX-bounded (copy_display_name, UTF-8-preserving). */
static void split_artist_album(const char *name, char *artist, char *album)
{
    int sep = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == ' ' && name[i + 1] == '-' && name[i + 2] == ' ') {
            sep = i;
            break;
        }
    }
    if (sep < 0) {
        artist[0] = '\0';
        copy_display_name(album, name, 0);
        return;
    }
    char tmp[NAME_MAX + 1];
    int n = (sep < NAME_MAX) ? sep : NAME_MAX;
    for (int i = 0; i < n; i++) tmp[i] = name[i];
    tmp[n] = '\0';
    copy_display_name(artist, tmp, 0);
    copy_display_name(album, name + sep + 3, 0);
}

/* Strip a leading track-number prefix ("NN. " / "NN.") from a track filename so
 * the tracklist shows a clean title (the row's own number gutter provides the
 * index). Only a digits-then-'.' prefix is removed, so titles that merely start
 * with a number ("99 Luftballons") are left alone. */
static const char *track_display(const char *name)
{
    const char *p = name;
    while (*p >= '0' && *p <= '9') p++;
    if (p != name && *p == '.') {
        p++;
        while (*p == ' ') p++;
        if (*p) return p;
    }
    return name;
}

/* MP3 playback is parked: dr_mp3's float synthesis can't hit real-time on this
 * FPU-less CPU (buffer starves -> stutter), and FLAC is lossless so there's no
 * quality reason to prefer it. The device is FLAC-only; a companion loader app
 * ensures music lands as FLAC. Flip to 1 to re-surface MP3 files (they'll open
 * but stutter) once a fixed-point/COP decoder exists. */
#define CORE_ENABLE_MP3 0

/* Classify by extension: 0 = FLAC (.fla/.flac), 1 = MP3 (.mp3), -1 = skip. */
static int classify_ext(const char *name)
{
    int dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }
    if (dot < 0) return -1;

    char ext[5];
    int n = 0;
    for (const char *e = name + dot + 1; *e && n < 4; e++) {
        char c = *e;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        ext[n++] = c;
    }
    ext[n] = '\0';

    if (n == 3 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A') return 0;
    if (n == 4 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A' && ext[3] == 'C') return 0;
#if CORE_ENABLE_MP3
    if (n == 3 && ext[0] == 'M' && ext[1] == 'P' && ext[2] == '3') return 1;
#endif
    return -1;
}

/* fat32_readdir callback: collect music subdirectories + playable files into
 * g_browse. Directories named like iPod/OS system folders (or dotfolders) are
 * junk-filtered out; non-playable files are skipped. */
static int browse_collect(void *ud, const fat32_dirent_t *e)
{
    (void)ud;

    /* Capture the folder's album-art sidecar (not shown as a list row). */
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_art_clus = e->first_clus;
        g_art_size = e->size;
        return 0;
    }

    if (g_browse_n >= BROWSE_MAX) return 1;   /* array full: stop enumeration */

    if (e->is_dir) {
        if (is_junk_dir(e->name)) return 0;       /* skip system/Apple folders */
        /* Artists drill-down: at the album-list level, keep only this artist's
         * folders (name "Artist - Album"). */
        if (g_dir_depth == 0 && g_artist_filter[0]) {
            char a[NAME_MAX + 1], b2[NAME_MAX + 1];
            split_artist_album(e->name, a, b2);
            if (!name_eq_ci(a, g_artist_filter)) return 0;
        }
        browse_entry_t *b = &g_browse[g_browse_n++];
        copy_display_name(b->name, e->name, 0);   /* keep folder name as-is */
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        return 0;
    }

    /* The album/artist lists (depth 0) show only folders — hide loose files at
     * the root (leftover TEST.*, stray downloads) so they don't clutter it. */
    if (g_dir_depth == 0) return 0;

    int fmt = classify_ext(e->name);
    if (fmt < 0) return 0;
    browse_entry_t *b = &g_browse[g_browse_n++];
    copy_display_name(b->name, e->name, 1);       /* trim ".flac" -> title */
    b->clus   = e->first_clus;
    b->size   = e->size;
    b->fmt    = (uint8_t)fmt;
    b->is_dir = 0;
    b->art_clus = b->art_size = 0;                 /* album play: queue-level art */
    return 0;
}

/* Vertical layout (menus.jsx): a status strip up top, a titled header with a
 * divider, then the scrolling list. */
#define STATUS_Y0  0                      /* status strip band (battery/track)    */
#define STATUS_H   15                     /* strip height                         */
#define HDR_BASE   30                     /* header title text baseline           */
#define HDR_DIV_Y  38                     /* header divider row                    */
#define LIST_Y0    42                     /* first list row top                   */
#define ROW_H      24                     /* px per single-line list row          */
#define LIST_ROWS  8                       /* visible rows: (240-42)/24 ~= 8       */
/* Two-line rows (album/song list: title + artist sub) get a taller row so the
 * bold title (bold_13, 19px ink) and the artist line (regular_9, 14px) don't
 * vertically overlap — 32px clears both so the marquee is cleanly title-only
 * (no artist repaint, no clipping of descenders). Fewer fit on screen. */
#define ROW_H2     32
#define LIST_ROWS2 6                       /* (240-42)/32 ~= 6                     */

/* Wheel scroll feel. The driver reports the raw differenced position count (up
 * to ~half a rotation per poll), and a single slow detent crosses the wheel's
 * sensitivity gate at ~CW_WHEEL_SENSITIVITY (4) units. Dividing by 3 left a
 * remainder every detent, so the carry periodically double-stepped (move 1,1,2)
 * — felt like "it skipped, then jumped two". Matching the divisor to the
 * sensitivity makes one detent advance exactly one row; MAX_DELTA keeps the
 * 2-rows-per-event headroom (8/4) so a fast flick still scrolls quickly. */
#define WHEEL_CLICKS_PER_ITEM CW_WHEEL_SENSITIVITY   /* = 4: one detent, one row */
#define WHEEL_MAX_DELTA       (2 * CW_WHEEL_SENSITIVITY) /* fast flick: <=2 rows/evt */

/* ---------------------------------------------------------------------------
 * Design-matched list chrome (menus.jsx): status strip, header, rows, scrollbar
 * ------------------------------------------------------------------------- */

/* Hold-switch lock state. While g_locked, all wheel/button input is swallowed
 * (playback keeps running); a brief plate flashes on the engage/disengage edge,
 * and a small padlock stays in the status strip while held. */
static int      g_locked;
static uint32_t g_lock_flash_until;

/* Small padlock glyph (system-screens.jsx corner lock): body + shackle. */
static void draw_lock_glyph(int x, int y, uint16_t c)
{
    console_fill_rect(x,     y + 4, 8, 6, c);   /* body      */
    console_fill_rect(x + 1, y,     2, 5, c);   /* left post */
    console_fill_rect(x + 5, y,     2, 5, c);   /* right post*/
    console_fill_rect(x + 1, y,     6, 2, c);   /* top arch  */
}

/* Cached battery/power readout. The gauge read is an I2C transaction (slow, and
 * shares the codec bus), so sample it a few seconds apart — NOT every present —
 * and hold the last value. mv/pct < 0 means the read failed / not yet sampled. */
static int      g_bat_mv  = -1;
static int      g_bat_pct = -1;
static int      g_bat_ext = 0;               /* external power present            */
static uint32_t g_bat_last_us;

/* Volume capacity / free (MB), computed once from the FS after mount for the
 * About screen. g_free_mb == 0xFFFFFFFF means the FSInfo free count was absent. */
static uint32_t g_total_mb, g_free_mb = 0xFFFFFFFFu;

/* Returns 1 if it actually resampled this call (so a live screen can repaint). */
static int battery_refresh(int force)
{
    uint32_t now = mmio_read32(USEC_TIMER_ADDR);
    if (!force && (uint32_t)(now - g_bat_last_us) < 5000000u) {
        return 0;
    }
    g_bat_last_us = now;
    g_bat_mv  = battery_millivolts();
    g_bat_pct = battery_percent();
    g_bat_ext = power_is_external();
    return 1;
}

/* Battery glyph: outline + nub + a fill proportional to `pct`. The fill turns a
 * clear RED at <=20% (a low-battery warning), else ink. ~30% larger than the
 * original 17x9 for legibility. Drawn at top-left (x,y), ~24px wide incl. nub. */
#define BATT_LOW_RED 0xE125u        /* distinct warning red (RGB ~226,40,44)    */
static void draw_battery(int x, int y, int pct)
{
    const int w = 22, h = 12;
    console_fill_rect(x, y, w, 1, LINEN_MUTED2);           /* top    */
    console_fill_rect(x, y + h - 1, w, 1, LINEN_MUTED2);   /* bottom */
    console_fill_rect(x, y, 1, h, LINEN_MUTED2);           /* left   */
    console_fill_rect(x + w - 1, y, 1, h, LINEN_MUTED2);   /* right  */
    console_fill_rect(x + w, y + 4, 2, h - 8, LINEN_MUTED2); /* nub   */
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int fw = ((w - 4) * pct) / 100;
    if (fw > 0) {
        console_fill_rect(x + 2, y + 2, fw, h - 4,
                          pct <= 20 ? BATT_LOW_RED : LINEN_INK);
    }
}

/* The top status strip: the now-playing track name on the left (so you always
 * see what's playing while browsing), battery on the right. During bring-up the
 * right side also shows raw millivolts (to calibrate the %-curve; see
 * battery.h "DEVICE-GATED CALIBRATION"). */
static void status_strip_render(void)
{
    /* Left: playing track (or the wordmark when idle). Clipped by the right
     * cluster, which is painted over it. */
    const char *left = player_active() ? track_display(player_track_name())
                                       : "CORE";
    ui_text(12, STATUS_Y0 + 11, left, FONT_SMALL, LINEN_MUTED2);

    /* Clear a right-hand region so a long track name can't collide with it. */
    console_fill_rect(LCD_WIDTH - 70, STATUS_Y0, 70, STATUS_H, LINEN_SURFACE);

    int bx = LCD_WIDTH - 12 - 24;             /* battery block (22 + 2 nub)        */
    draw_battery(bx, STATUS_Y0 + 1, g_bat_pct);

    /* Persistent padlock while Hold is engaged (design keeps it in the strip). */
    if (g_locked) {
        draw_lock_glyph(bx - 14, STATUS_Y0 + 3, LINEN_INK);
    }

    /* Raw mV to the left of the glyph — a device-gated CALIBRATION aid, not part
     * of the design. Off for the shipping look; flip SHOW_BATTERY_MV to 1 to
     * read the millivolts and calibrate battery.c's %-curve (see battery.h). */
#define SHOW_BATTERY_MV 0
    if (SHOW_BATTERY_MV && g_bat_mv > 0 && !g_locked) {
        char mv[8];
        int v = g_bat_mv, i = 0;
        char tmp[8]; int t = 0;
        if (v > 9999) v = 9999;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v && t < 7);
        while (t > 0) mv[i++] = tmp[--t];
        mv[i] = '\0';
        int w = text_width(mv, FONT_SMALL);
        ui_text(bx - 5 - w, STATUS_Y0 + 11, mv, FONT_SMALL, LINEN_MUTED2);
    }
}

/* Titled header with an optional back chevron and a right-aligned count/value,
 * plus the divider under it (menus.jsx ScreenHeader). */
static void header_render(const char *title, const char *right, int back)
{
    int x = 12;
    if (back) {
        x = ui_text(x, HDR_BASE, UI_GLYPH_LAQUO, FONT_HEADER, LINEN_MUTED2) + 4;
    }
    ui_text(x, HDR_BASE, title, FONT_HEADER, LINEN_INK);
    if (right && right[0]) {
        int w = text_width(right, FONT_SMALL);
        ui_text(LCD_WIDTH - 12 - w, HDR_BASE - 1, right, FONT_SMALL, LINEN_MUTED2);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, LINEN_BORDER);
}

/* One list row (menus.jsx Row) at list origin `y0`. Selected = filled ink bar +
 * light bold text; greyed = muted (inactive menu item). Optional sub-line, right
 * value, chevron, and a leading 22x22 art chip (RGB565, or NULL). */
static void list_row_at(int y0, int r, const char *text, const char *sub,
                        const char *right, int chevron, int selected, int greyed,
                        const uint16_t *chip, int title_priority, int rh)
{
    int ry = y0 + r * rh;
    int rowmid = ry + rh / 2 + 3;         /* vertical centre for value/chevron   */
    uint16_t fg, subc, rightc, chevc;
    if (selected) {
        fill_round_rect(6, ry + 1, LCD_WIDTH - 16, rh - 2, 4, LINEN_SEL_BG);
        fg = LINEN_SEL_FG; subc = LINEN_SEL_SUB; rightc = LINEN_SEL_SUB;
        chevc = LINEN_SEL_SUB;
    } else {
        fg = greyed ? LINEN_MUTED : LINEN_INK;
        subc = LINEN_MUTED2; rightc = LINEN_MUTED_D; chevc = LINEN_CHEVRON;
    }

    int tx = 14;
    if (chip) {
        int cd = ARTCACHE_DIM;                 /* cached chip is cd x cd RGB565 */
        int cy = ry + (rh - cd) / 2;
        console_blit565(12, cy, cd, cd, chip);
        /* Round the chip's corners (~2px, menus.jsx Chip radius) by knocking the
         * outer corner pixels back to the row background. */
        uint16_t cbg = selected ? LINEN_SEL_BG : LINEN_SURFACE;
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                if (dx + dy >= 2) continue;
                console_fill_rect(12 + dx,            cy + dy,            1, 1, cbg);
                console_fill_rect(12 + cd - 1 - dx,   cy + dy,            1, 1, cbg);
                console_fill_rect(12 + dx,            cy + cd - 1 - dy,   1, 1, cbg);
                console_fill_rect(12 + cd - 1 - dx,   cy + cd - 1 - dy,   1, 1, cbg);
            }
        }
        tx = 12 + cd + 8;
    }
    const text_font_t *tf = selected ? FONT_HEADER : FONT_ROW;
    /* Where the title must stop (before the right value / chevron). */
    int title_right;
    int show_right = (right && right[0]);
    if (right && right[0]) {
        int reserved = LCD_WIDTH - 16 - text_width(right, text_font_bold_11()) - 6;
        /* title_priority: the title owns the row. Reserve the value column only
         * while the title still fits inside it; once it's too long the title
         * spans the full width (over where the value was) and the value drops —
         * so it clips at the row edge and marquees on select, instead of being
         * cramped into a short column. */
        if (title_priority && text_width(text, tf) > reserved - tx) {
            title_right = LCD_WIDTH - 16;
            show_right = 0;
        } else {
            title_right = reserved;
        }
    } else if (chevron) {
        title_right = LCD_WIDTH - 18 - 4;
    } else {
        title_right = LCD_WIDTH - 16;
    }
    int avail = title_right - tx;
    /* Two-line rows: title sits a little below the row top (there's room in the
     * 32px row) with the artist near the bottom; single-line rows centre it. */
    int base  = sub ? ry + 16 : rowmid;
    int sub_y = ry + rh - 4;                   /* artist baseline, near row bottom */
    /* Marquee vertical clip. Top = the selection bar's interior (ry+1), NOT
     * y-ascent: the font ascent (14) reaches 1px ABOVE the bar, and the clear
     * would paint a sel-coloured line there ("clips above"). Bottom = just above
     * the artist baseline's cap (sub_y-10) so the scroll tick's clear can't erase
     * the static artist line. Single-line rows clip to the bar bottom. */
    int mqy0 = ry + 1;
    int mqy1 = sub ? (sub_y - 10) : (ry + rh - 1);
    if (selected) {
        mq_text(tx, base, avail, text, tf, fg, LINEN_SEL_BG, mqy0, mqy1);
    } else {
        text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, tx, base, text, tf, fg,
                       tx, tx + avail);
    }
    if (sub) {
        ui_text(tx, sub_y, sub, FONT_SMALL, subc);
    }
    if (show_right) {
        int w = text_width(right, text_font_bold_11());   /* right values 10/600 */
        ui_text(LCD_WIDTH - 16 - w, rowmid, right, text_font_bold_11(), rightc);
    } else if (chevron) {
        ui_text(LCD_WIDTH - 18, rowmid, UI_GLYPH_RAQUO, FONT_ROW, chevc);
    }
}

/* Convenience: a single-line (24px) row. */
static void list_row(int r, const char *text, const char *sub, const char *right,
                     int chevron, int selected, int greyed, const uint16_t *chip)
{
    list_row_at(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H);
}

/* A taller (28px) two-line row for the album list: title + artist sub with a
 * cover chip, so the two lines don't overlap and the marquee stays title-only. */
static void list_row_tall(int r, const char *text, const char *sub, const char *right,
                          int chevron, int selected, int greyed, const uint16_t *chip)
{
    list_row_at(LIST_Y0, r, text, sub, right, chevron, selected, greyed, chip, 0, ROW_H2);
}

/* Like list_row_tall, but the title takes priority over the right-hand value: a
 * long title spans the full width (over the value) and marquees on select. */
static void list_row_titled(int r, const char *text, const char *sub,
                            const char *right, int selected, const uint16_t *chip)
{
    list_row_at(LIST_Y0, r, text, sub, right, 0, selected, 0, chip, 1, ROW_H2);
}

/* Slim right-edge scrollbar (menus.jsx Scrollbar); no-op when everything fits.
 * `y0` is the list origin (differs between the full list and the detail view). */
static void scrollbar_render(int y0, int top, int visible, int total)
{
    if (total <= visible) {
        return;
    }
    int track_y = y0;
    int track_h = LCD_HEIGHT - y0 - 4;
    console_fill_rect(LCD_WIDTH - 4, track_y, 3, track_h, LINEN_SB_TRK);
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 16) thumb_h = 16;
    int denom = total - visible;
    if (denom < 1) denom = 1;
    int thumb_y = track_y + (top * (track_h - thumb_h)) / denom;
    console_fill_rect(LCD_WIDTH - 4, thumb_y, 3, thumb_h, LINEN_SB_THMB);
}

/* Windowed-list scroll origin (menus.jsx useScrollWindow): keep the selection
 * about 1/3 from the top, clamped to the ends. Pure function — the browser
 * derives the visible window from the selection each paint (no separate top
 * state to keep in sync). */
static int scroll_window(int sel, int total, int visible)
{
    if (total <= visible) return 0;
    int start = sel - visible / 3;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    return start;
}

/* Write unsigned `v` as decimal into `dst`, return the length. The one decimal
 * writer — replaces the do/while digit-reversal that was open-coded ~8 times. */
static int u32_to_dec(char *dst, unsigned v)
{
    char nb[10];
    int t = 0;
    do { nb[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < t; i++) dst[i] = nb[t - 1 - i];
    dst[t] = '\0';
    return t;
}

static void fmt_time(char *buf, uint32_t s);   /* "M:SS", defined below         */

/* Format "a / b" into dst (needs >= 12 bytes). */
static void fmt_count(char *dst, int a, int b)
{
    int i = u32_to_dec(dst, (unsigned)a);
    dst[i++] = ' '; dst[i++] = '/'; dst[i++] = ' ';
    u32_to_dec(dst + i, (unsigned)b);
}

/* ---------------------------------------------------------------------------
 * Album detail view (collection-detail.jsx AlbumDetail): a 56x56 art hero with
 * title + track count, then the folder's tracklist. Shown when you enter an
 * album folder (browser depth > 0). The hero art is the entered folder's
 * folder.art, downscaled on-device to 56x56 (thumb_downscale_rgb565).
 * ------------------------------------------------------------------------- */
#define DET_HERO_Y   42                   /* art hero top                         */
#define DET_ART      56                   /* hero art dimension                   */
#define DET_LIST_Y0  108                  /* tracklist first row top              */
#define DET_ROWS     5                     /* visible rows: (240-108)/24 = 5 fit   */

#define ART_RAW_MAX  (12 + 120 * 120 * 2)  /* a full 120x120 CoreArt file          */
static uint8_t  g_art_scratch[ART_RAW_MAX];      /* raw folder.art read buffer      */
static uint16_t g_detail_art[DET_ART * DET_ART]; /* downscaled 56x56 hero           */
static int      g_detail_art_ok;
static char     g_album_title[NAME_MAX + 1];     /* album part of "Artist - Album" */
static char     g_album_artist[NAME_MAX + 1];    /* artist part (empty if none)    */
static int      g_album_track_n;                  /* playable files in the folder   */

/* Per-track disc/duration (from the index) + a display "view" that interleaves
 * "Disc N" header rows for multi-disc albums. Filled in detail_load_meta. A view
 * entry >= 0 is a track index into g_browse; < 0 encodes a header disc as
 * -(disc+1). */
#define DET_VIEW_MAX (BROWSE_MAX + 8)
static uint16_t g_track_dur[BROWSE_MAX];
static uint16_t g_track_num[BROWSE_MAX];
static uint8_t  g_track_disc[BROWSE_MAX];
/* Real (tag) title per browse row, bound from the index in detail_load_meta.
 * The on-disk filename is FAT-sanitized (?,*,:,/ -> _), so the tracklist shows
 * this instead when available; NULL falls back to the filename. */
static const char *g_track_title[BROWSE_MAX];
static int16_t  g_det_view[DET_VIEW_MAX];
static int      g_det_view_n;
static int      g_detail_multidisc;

/* Read the current folder's folder.art (clus/size captured by browse_load) and
 * downscale it to the 56x56 hero. Leaves g_detail_art_ok=0 if absent/malformed.
 * Same CoreArt "CART" validation the player uses (player.c load_folder_art). */
static void detail_art_load(fat32_t *fs)
{
    g_detail_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < 12 || g_art_size > sizeof g_art_scratch) {
        return;
    }
    int32_t n = fat32_read_file(fs, g_art_clus, g_art_scratch, g_art_size);
    if (n < 12) {
        return;
    }
    if (g_art_scratch[0] != 'C' || g_art_scratch[1] != 'A' ||
        g_art_scratch[2] != 'R' || g_art_scratch[3] != 'T') {
        return;
    }
    int w = g_art_scratch[6] | (g_art_scratch[7] << 8);
    int h = g_art_scratch[8] | (g_art_scratch[9] << 8);
    if (w <= 0 || h <= 0 || w > 120 || h > 120) {
        return;
    }
    if ((int32_t)(12 + w * h * 2) > n) {
        return;
    }
    thumb_downscale_rgb565((const uint16_t *)(g_art_scratch + 12), w, h,
                           g_detail_art, DET_ART, DET_ART);
    g_detail_art_ok = 1;
}

/* Animated three-bar "now playing" glyph (collection-detail.jsx NowPlayingDot).
 * Each bar bounces on a triangle wave with its own phase; bottom-anchored at
 * y+9, heights 3..9. `t_us` is the free-running microsecond clock. */
#define NP_BARS_W  9                          /* bounding box width  (x..x+8)      */
#define NP_BARS_H  9                          /* bounding box height (y..y+8)      */

static int nowplaying_bar_h(uint32_t t_ms, int phase)
{
    uint32_t p = (t_ms + (uint32_t)phase) % 760u;      /* period 760 ms          */
    int tri = (p < 380u) ? (int)p : (int)(760u - p);   /* 0..380..0 triangle     */
    return 3 + (tri * 6) / 380;                          /* 3..9 px               */
}

static void nowplaying_bars(int x, int y, uint16_t c, uint32_t t_us)
{
    uint32_t ms = t_us / 1000u;
    static const int ph[3] = { 0, 250, 500 };
    for (int i = 0; i < 3; i++) {
        int h = nowplaying_bar_h(ms, ph[i]);
        console_fill_rect(x + i * 3, y + (NP_BARS_H - h), 2, h, c);
    }
}

static void detail_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    fmt_count(right, sel + 1, g_browse_n > 0 ? g_browse_n : 1);
    header_render("Albums", right, 1);

    /* Hero art (or a placeholder tile when the folder has no folder.art). */
    if (g_detail_art_ok) {
        console_blit565(12, DET_HERO_Y, DET_ART, DET_ART, g_detail_art);
    } else {
        console_fill_rect(12, DET_HERO_Y, DET_ART, DET_ART, LINEN_BORDER);
    }
    int tx = 12 + DET_ART + 12;
    ui_text(tx, DET_HERO_Y + 15, g_album_title, FONT_HEADER, LINEN_INK);
    if (g_album_artist[0]) {
        ui_text(tx, DET_HERO_Y + 31, g_album_artist, FONT_SUB, LINEN_MUTED_D);
    }
    char meta[40];
    int mi = u32_to_dec(meta, (unsigned)g_album_track_n);
    for (const char *p = (g_album_track_n == 1) ? " track" : " tracks"; *p; p++)
        meta[mi++] = *p;
    /* Append total runtime ("11 tracks · 1h 41m") when durations are known
     * (index path). collection-detail.jsx meta line. */
    uint32_t tot_s = 0;
    for (int i = 0; i < g_browse_n; i++) tot_s += g_track_dur[i];
    if (tot_s > 0) {
        for (const char *p = " " UI_GLYPH_MIDDOT " "; *p; p++) meta[mi++] = *p;
        uint32_t tot_m = (tot_s + 59u) / 60u;      /* round UP to whole minutes */
        uint32_t hh = tot_m / 60u, mm = tot_m % 60u;
        if (hh > 0) {
            mi += u32_to_dec(meta + mi, hh);
            meta[mi++] = 'h'; meta[mi++] = ' ';
        }
        mi += u32_to_dec(meta + mi, mm);
        meta[mi++] = 'm';
    }
    meta[mi] = '\0';
    ui_text(tx, DET_HERO_Y + 47, meta, FONT_SMALL, LINEN_MUTED2);

    console_fill_rect(12, DET_LIST_Y0 - 6, LCD_WIDTH - 24, 1, LINEN_BORDER);

    if (g_browse_n == 0) {
        ui_text(14, DET_LIST_Y0 + 14, "Empty folder", FONT_ROW, LINEN_MUTED);
        return;
    }
    /* Scroll over the display view (tracks + any "Disc N" headers), centered on
     * the selected track's position within it. */
    int sel_view = 0;
    for (int i = 0; i < g_det_view_n; i++) {
        if (g_det_view[i] == (int16_t)sel) { sel_view = i; break; }
    }
    int top = scroll_window(sel_view, g_det_view_n, DET_ROWS);
    const char *playing = player_active() ? player_track_name() : 0;
    /* Single-disc albums have no "DISC N" header rows, so the per-disc number
     * gutter is wasted width — pull the numbers and titles left to reclaim it. */
    int multi_disc = 0;
    for (int i = 0; i < g_det_view_n; i++) {
        if (g_det_view[i] < 0) { multi_disc = 1; break; }
    }
    int num_rx  = multi_disc ? 30 : 24;   /* track-number right-align x */
    int title_x = multi_disc ? 38 : 30;   /* title left x               */
    for (int r = 0; r < DET_ROWS; r++) {
        int vi = top + r;
        if (vi >= g_det_view_n) break;
        int ry = DET_LIST_Y0 + r * ROW_H;
        int16_t v = g_det_view[vi];
        if (v < 0) {                          /* a "Disc N" section header */
            char h[12];
            int hi = 0;
            for (const char *p = "DISC "; *p; p++) h[hi++] = *p;
            u32_to_dec(h + hi, (unsigned)(-(int)v - 1));
            ui_text(14, ry + 15, h, FONT_SMALL, LINEN_MUTED_D);
            continue;
        }
        int idx = v;
        const browse_entry_t *e = &g_browse[idx];
        int is_sel = (idx == sel);
        if (is_sel) {
            fill_round_rect(6, ry + 1, LCD_WIDTH - 16, ROW_H - 2, 4, LINEN_SEL_BG);
        }
        uint16_t fg = is_sel ? LINEN_SEL_FG : LINEN_INK;
        uint16_t nc = is_sel ? LINEN_SEL_SUB : LINEN_MUTED2;
        /* Left gutter: the now-playing bars for the playing track, else its
         * (per-disc) track number (collection-detail.jsx). */
        if (playing && name_eq_ci(e->name, playing)) {
            nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK,
                            mmio_read32(USEC_TIMER_ADDR));
        } else {
            char num[6];
            u32_to_dec(num, (unsigned)(g_track_num[idx] ? g_track_num[idx]
                                                        : idx + 1));
            int nw = text_width(num, FONT_SMALL);
            ui_text(num_rx - nw, ry + 15, num, FONT_SMALL, nc);
        }
        /* Duration on the right (from the index); the title must stop before it. */
        int title_right = LCD_WIDTH - 16;
        if (g_track_dur[idx]) {
            char dts[8];
            fmt_time(dts, g_track_dur[idx]);
            int dw = text_width(dts, text_font_bold_11());
            ui_text(LCD_WIDTH - 16 - dw, ry + 15, dts, text_font_bold_11(),
                    is_sel ? LINEN_SEL_SUB : LINEN_MUTED_D);
            title_right = LCD_WIDTH - 16 - dw - 8;
        }
        /* Title (clean — number gutter provides the index), indented past it,
         * CLIPPED before the duration so a long title can't overlap it; the
         * selected row's long title scrolls (marquee). */
        const text_font_t *tf = is_sel ? FONT_HEADER : FONT_ROW;
        /* Prefer the real tag title (bound from the index): the filename is
         * FAT-sanitized (e.g. "WHO CARES?" -> "WHO CARES_"), the tag isn't. */
        const char *tt = g_track_title[idx] ? g_track_title[idx]
                                            : track_display(e->name);
        if (is_sel) {
            mq_text(title_x, ry + 15, title_right - title_x, tt, tf, fg,
                    LINEN_SEL_BG, ry, ry + ROW_H);
        } else {
            text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, title_x, ry + 15,
                           tt, tf, fg, title_x, title_right);
        }
    }
    scrollbar_render(DET_LIST_Y0, top, DET_ROWS, g_det_view_n);
}

/* A neutral tile shown in a row's chip slot until its real cover loads, so the
 * text doesn't shift right when the thumbnail pops in. Filled once at startup. */
static uint16_t g_chip_ph[ARTCACHE_DIM * ARTCACHE_DIM];

static void chip_placeholder_init(void)
{
    for (int i = 0; i < ARTCACHE_DIM * ARTCACHE_DIM; i++) {
        g_chip_ph[i] = LINEN_BORDER;
    }
}

/* Queue every album row's cover into the incremental thumbnail cache. Called
 * when the album list is (re)entered; artcache_pump loads them a few frames
 * apart off the audio path. NOT reset here — artcache_queue is idempotent (a
 * slot whose cover is unchanged keeps its loaded pixels), so backing out of an
 * album detail re-uses the covers already loaded instead of reloading them all.
 * A changed view (e.g. a different artist filter) re-queues only the slots whose
 * album actually changed. */
static void albumlist_queue_chips(void)
{
    for (int i = 0; i < g_albumview_n && i < ARTCACHE_SLOTS; i++) {
        const lib_album_t *al = &g_albums[g_albumview[i]];
        artcache_queue(i, al->thm_clus, al->thm_size, al->art_clus, al->art_size);
    }
}

/* Album LIST (browser depth 0): the folder list with the design chrome, each
 * album row carrying a 22x22 cover chip (or a placeholder until it loads). */
static void albumlist_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_albumview_n > 0) {
        fmt_count(right, sel + 1, g_albumview_n);
    } else {
        right[0] = '\0';
    }
    /* Header title = the artist when drilled in from Artists, else "Albums". */
    header_render(g_artist_filter[0] ? g_artist_filter : "Albums", right, 1);

    if (g_albumview_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No albums", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_albumview_n, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= g_albumview_n) break;
        const lib_album_t *e = &g_albums[g_albumview[idx]];
        const uint16_t *chip = (idx < ARTCACHE_SLOTS) ? artcache_get(idx) : 0;
        if (!chip) chip = g_chip_ph;           /* reserve the space meanwhile       */
        /* Show "Album" as the title and the artist as a sub-line (parsed from the
         * "Artist - Album" folder name). In an artist's own list the artist sub
         * is redundant, so drop it there. */
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(e->folder, artist, album);
        const char *sub = (!g_artist_filter[0] && artist[0]) ? artist : 0;
        list_row_tall(r, album, sub, 0, 1, idx == sel, 0, chip);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS2, g_albumview_n);
}

/* ---------------------------------------------------------------------------
 * Artists (menus.jsx ArtistsList): a list of the unique artist prefixes parsed
 * from the "Artist - Album" folder names. Selecting one filters the album list
 * (g_artist_filter) to just that artist's albums.
 * ------------------------------------------------------------------------- */
#define ARTISTS_MAX 96
/* Each artist carries a representative album cover (its first album's) so the
 * Artists list can show a chip, same as albums — indexed, no per-row scan. */
typedef struct {
    char     name[NAME_MAX + 1];
    uint32_t thm_clus, thm_size, art_clus, art_size;
} lib_artist_t;
static lib_artist_t g_artists[ARTISTS_MAX];
static int          g_artists_n;
static int  g_artist_sel, g_artist_accum;

static int title_cmp(const char *a, const char *b);   /* case-insensitive, defined below */

/* De-duplication / sort key for an artist name: ignore a leading "The " so
 * "The Kid LAROI" and "Kid LAROI" collapse to one entry (and sort together
 * under K). Combined with title_cmp's case folding this also merges pure
 * case variants ("blackbear" vs "Blackbear"). Returns a pointer INTO `s`. */
static const char *artist_key(const char *s)
{
    if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'h' || s[1] == 'H') &&
        (s[2] == 'e' || s[2] == 'E') && s[3] == ' ') {
        return s + 4;
    }
    return s;
}

/* Build the unique, de-duplicated artist list from the index-derived album
 * list, then sort it A->Z. De-dup is by artist_key (leading "The " and case
 * ignored), so a differently-cased folder can't split one artist into two rows.
 * Because g_albums is index-driven, artists of stale on-disk folders never
 * appear here at all. */
static void build_artists(void)
{
    g_artists_n = 0;
    for (int i = 0; i < g_albums_n; i++) {
        char artist[NAME_MAX + 1], album[NAME_MAX + 1];
        split_artist_album(g_albums[i].folder, artist, album);
        if (!artist[0]) continue;                 /* no "Artist - " prefix        */
        int found = 0;
        for (int j = 0; j < g_artists_n; j++) {
            if (title_cmp(artist_key(g_artists[j].name), artist_key(artist)) == 0) {
                found = 1;
                break;
            }
        }
        if (!found && g_artists_n < ARTISTS_MAX) {
            lib_artist_t *a = &g_artists[g_artists_n];
            int k = 0;
            for (; artist[k] && k < NAME_MAX; k++) a->name[k] = artist[k];
            a->name[k]  = '\0';
            a->thm_clus = g_albums[i].thm_clus;   /* first album = artist's chip */
            a->thm_size = g_albums[i].thm_size;
            a->art_clus = g_albums[i].art_clus;
            a->art_size = g_albums[i].art_size;
            g_artists_n++;
        }
    }

    /* Insertion-sort A->Z by artist_key (so "The xyz" files under X); the whole
     * struct moves, so each artist's chip travels with its name. */
    for (int i = 1; i < g_artists_n; i++) {
        lib_artist_t v = g_artists[i];
        int j = i - 1;
        while (j >= 0 && title_cmp(artist_key(g_artists[j].name),
                                   artist_key(v.name)) > 0) {
            g_artists[j + 1] = g_artists[j];
            j--;
        }
        g_artists[j + 1] = v;
    }
}

/* Build the on-screen album slice (indices into the already-sorted g_albums),
 * either all albums (artist_filter NULL/empty) or one artist's — matched by
 * artist_key so the "The "/case-insensitive grouping matches the Artists menu. */
static void albumview_build(const char *artist_filter)
{
    g_albumview_n = 0;
    for (int i = 0; i < g_albums_n && g_albumview_n < LIB_MAX_ALBUMS; i++) {
        if (artist_filter && artist_filter[0]) {
            char a[NAME_MAX + 1], b[NAME_MAX + 1];
            split_artist_album(g_albums[i].folder, a, b);
            if (title_cmp(artist_key(a), artist_key(artist_filter)) != 0) continue;
        }
        g_albumview[g_albumview_n++] = (uint16_t)i;
    }
}

static void artists_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_artists_n > 0) fmt_count(right, sel + 1, g_artists_n);
    else                 right[0] = '\0';
    header_render("Artists", right, 1);

    if (g_artists_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No artists", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_artists_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_artists_n) break;
        list_row(r, g_artists[idx].name, 0, 0, 1, idx == sel, 0, 0);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS, g_artists_n);
}

static void browse_render(int sel)
{
    if (g_dir_depth > 0) {
        detail_render(sel);
    } else {
        albumlist_render(sel);
    }
}

/* ---------------------------------------------------------------------------
 * Library index (Songs / Genres): a one-time tag scan of every FLAC in the
 * library into RAM. The anti-skip buffer (~30 s) covers playback while the scan
 * hits the disk, so it can run without dropouts. Rebuilt only once per session.
 * ------------------------------------------------------------------------- */
#define LIB_MAX_SONGS  1200
#define LIB_MAX_GENRES 48
#define LIB_TITLE_MAX  48
#define LIB_GENRE_MAX  24

#define LIB_ARTIST_MAX 40
#define LIB_FILE_MAX   64

typedef struct {
    char     title[LIB_TITLE_MAX];
    char     artist[LIB_ARTIST_MAX];
    char     file[LIB_FILE_MAX];          /* track filename, ext trimmed        */
    uint32_t file_hash;                   /* name_hash of full filename (locator) */
    uint32_t dir_clus;                    /* album folder (queue context + play)*/
    uint32_t file_clus, file_size;        /* the track file itself (resolved at   */
                                          /* load) — play/shuffle without a scan  */
    uint32_t duration_s;
    uint16_t track, disc;
    int16_t  genre;                       /* index into g_genres, -1 = none     */
} lib_song_t;

static lib_song_t g_songs[LIB_MAX_SONGS];
static int        g_songs_n;
static uint16_t   g_song_sorted[LIB_MAX_SONGS];   /* song indices, title order  */
static char       g_genres[LIB_MAX_GENRES][LIB_GENRE_MAX];
static int        g_genres_n;
static int        g_genre_count[LIB_MAX_GENRES]; /* songs per genre (precomputed) */
static int        g_lib_scanned;
static int        g_lib_indexed;                  /* loaded from CORELIB.IDX     */

/* Root-folder name -> cluster map (built once), for resolving an index record's
 * album folder to a cluster without a per-album directory read. */
#define FOLDER_MAP_MAX 160
static struct { char name[NAME_MAX + 1]; uint32_t clus, hash; } g_folder_map[FOLDER_MAP_MAX];
static int      g_folder_n;
static uint32_t g_idx_clus, g_idx_size;           /* CORELIB.IDX location        */

/* Scan temporaries (kept off the browser's g_browse). */
static uint32_t   g_scan_dirs[BROWSE_MAX];
static int        g_scan_dirs_n;
typedef struct { char name[NAME_MAX + 1]; uint32_t clus, size; } scan_file_t;
static scan_file_t g_scan_files[BROWSE_MAX];
static int         g_scan_files_n;
static uint32_t    g_scan_art_clus, g_scan_art_size;

/* Filtered, on-screen song list (all songs, or one genre's). */
static uint16_t   g_songview[LIB_MAX_SONGS];
static int        g_songview_n;
static int        g_song_sel, g_song_accum;
static int        g_genre_sel, g_genre_accum;

/* Case-insensitive title compare (for the sort). */
static int title_cmp(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)*a - (int)*b;
}

static int16_t genre_intern(const char *g)
{
    if (!g[0]) return -1;
    for (int i = 0; i < g_genres_n; i++) {
        if (name_eq_ci(g_genres[i], g)) return (int16_t)i;
    }
    if (g_genres_n < LIB_MAX_GENRES) {
        int k = 0;
        for (; g[k] && k < LIB_GENRE_MAX - 1; k++) g_genres[g_genres_n][k] = g[k];
        g_genres[g_genres_n][k] = '\0';
        return (int16_t)g_genres_n++;
    }
    return -1;
}

/* Add (folder, cluster) to the index-derived album list, de-duped by cluster. */
static void album_intern(const char *folder, uint32_t clus)
{
    if (clus == 0) return;
    for (int i = 0; i < g_albums_n; i++) {
        if (g_albums[i].clus == clus) return;      /* already listed */
    }
    if (g_albums_n >= LIB_MAX_ALBUMS) return;
    int k = 0;
    for (; folder[k] && k < NAME_MAX; k++) g_albums[g_albums_n].folder[k] = folder[k];
    g_albums[g_albums_n].folder[k] = '\0';
    g_albums[g_albums_n].clus = clus;
    g_albums_n++;
}

static int scan_dirs_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir || is_junk_dir(e->name)) return 0;
    if (g_scan_dirs_n < BROWSE_MAX) g_scan_dirs[g_scan_dirs_n++] = e->first_clus;
    album_intern(e->name, e->first_clus);          /* scan fallback album list */
    return 0;
}

static int scan_files_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_scan_art_clus = e->first_clus;
        g_scan_art_size = e->size;
        return 0;
    }
    if (e->is_dir || classify_ext(e->name) < 0) return 0;   /* playable files */
    if (g_scan_files_n < BROWSE_MAX) {
        scan_file_t *f = &g_scan_files[g_scan_files_n++];
        copy_display_name(f->name, e->name, 1);
        f->clus = e->first_clus;
        f->size = e->size;
    }
    return 0;
}

/* Copy a fixed-length index field (NUL-terminated within it) to a bounded C
 * string, preserving UTF-8 (the atlas covers Latin-1 + smart punctuation); only
 * C0 control bytes become spaces. Byte-bounded — a split multibyte tail just
 * renders as one U+FFFD. */
static void field_copy(char *dst, int dcap, const uint8_t *src, int slen)
{
    int i = 0;
    for (; i < slen && i < dcap - 1 && src[i]; i++) {
        unsigned char c = src[i];
        dst[i] = (c >= 0x20) ? (char)c : ' ';   /* keep ASCII + all UTF-8 bytes */
    }
    dst[i] = '\0';
}

/* Root enumeration for the index path: capture CORELIB.IDX + a folder->cluster
 * map (so records resolve to a cluster with no per-album directory read). */
static int index_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (!e->is_dir && name_eq_ci(e->name, "CORELIB.IDX")) {
        g_idx_clus = e->first_clus;
        g_idx_size = e->size;
        return 0;
    }
    if (e->is_dir && !is_junk_dir(e->name) && g_folder_n < FOLDER_MAP_MAX) {
        copy_display_name(g_folder_map[g_folder_n].name, e->name, 0);
        /* The index's folder[] field is 64 bytes, so the host writer stores at
         * most 63 chars + NUL (build_index.py ascii_field). Cap the on-disk name
         * to the same 63 chars here, else a >63-char album folder never matches
         * (device kept 64, host kept 63) and all its tracks silently drop. */
        g_folder_map[g_folder_n].name[NAME_MAX - 1] = '\0';
        g_folder_map[g_folder_n].clus = e->first_clus;
        /* Locator hash over the FULL on-disk name (untruncated, folded), so it
         * matches the record's folder_hash even for >63-char folders. */
        g_folder_map[g_folder_n].hash = name_hash(e->name);
        g_folder_n++;
    }
    return 0;
}

/* Resolve an index record's album folder to a cluster. Primary: match the
 * record's precomputed folder_hash (quote/case-folded) against the on-disk
 * folder hashes. Fallback: the legacy case-insensitive name compare, so a
 * hash mismatch can never regress below the old behaviour. */
static uint32_t folder_clus_h(uint32_t hash, const char *name)
{
    for (int i = 0; i < g_folder_n; i++) {
        if (g_folder_map[i].hash == hash) return g_folder_map[i].clus;
    }
    for (int i = 0; i < g_folder_n; i++) {
        if (name_eq_ci(g_folder_map[i].name, name)) return g_folder_map[i].clus;
    }
    return 0;
}

/* A titled loading screen with a determinate progress bar (0..100%). Rendered
 * from the library load phases so a multi-second first-load shows real progress
 * instead of a frozen splash. */
static void load_bar(const char *title, int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    console_clear(LINEN_SURFACE);
    ui_text_centered(112, title, FONT_TITLE, LINEN_INK);
    int bx = 60, by = 138, bw = LCD_WIDTH - 120, bh = 6;
    fill_round_rect(bx, by, bw, bh, 3, LINEN_BORDER);
    if (pct > 0) {
        int fw = bw * pct / 100;
        if (fw < bh) fw = bh;                 /* keep the rounded cap visible */
        fill_round_rect(bx, by, fw, bh, 3, LINEN_ACCENT);
    }
    lcd_present_fb(console_framebuffer());
}

/* Album index by folder cluster, or -1. Used to attach each shuffled song's
 * cover (and elsewhere a track needs its album without a scan). */
static int album_by_clus(uint32_t dc)
{
    for (int i = 0; i < g_albums_n; i++) {
        if (g_albums[i].clus == dc) return i;
    }
    return -1;
}

/* Resolve pass: ONE readdir per album at load time captures both the folder's
 * cover clusters (folder.art/.thm) AND every track file's own cluster into
 * g_songs — so later cover loads, playing a song, and shuffling the WHOLE
 * library are all direct reads with no per-use directory scan. */
static uint32_t g_res_art_clus, g_res_art_size, g_res_thm_clus, g_res_thm_size;
static uint32_t g_res_album_clus;
static int resolve_art_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir) return 0;
    if (name_eq_ci(e->name, "folder.thm")) {
        g_res_thm_clus = e->first_clus; g_res_thm_size = e->size; return 0;
    }
    if (name_eq_ci(e->name, "folder.art")) {
        g_res_art_clus = e->first_clus; g_res_art_size = e->size; return 0;
    }
    if (classify_ext(e->name) < 0) return 0;      /* a playable track: bind its cluster */
    uint32_t fh = name_hash(e->name);             /* over the FULL name incl ext */
    char nm[NAME_MAX + 1];
    copy_display_name(nm, e->name, 1);            /* ext-trimmed, matches s->file */
    for (int s = 0; s < g_songs_n; s++) {
        if (g_songs[s].file_clus || g_songs[s].dir_clus != g_res_album_clus) continue;
        /* Primary: the record's file_hash (index path). Fallback: ext-trimmed
         * name compare (covers the scan path, whose file_hash is 0). */
        if ((g_songs[s].file_hash && g_songs[s].file_hash == fh) ||
            name_eq_ci(g_songs[s].file, nm)) {
            g_songs[s].file_clus = e->first_clus;
            g_songs[s].file_size = e->size;
            break;
        }
    }
    return 0;
}
static void library_resolve_art(fat32_t *fs)
{
    for (int s = 0; s < g_songs_n; s++) g_songs[s].file_clus = 0;
    for (int i = 0; i < g_albums_n; i++) {
        if ((i & 3) == 0) {                   /* second half of the bar: 75->100% */
            load_bar("Loading Library",
                     75 + (g_albums_n ? i * 25 / g_albums_n : 25));
        }
        g_res_art_clus = g_res_art_size = g_res_thm_clus = g_res_thm_size = 0;
        g_res_album_clus = g_albums[i].clus;
        fat32_readdir(fs, g_albums[i].clus, resolve_art_cb, 0);
        g_albums[i].art_clus = g_res_art_clus;
        g_albums[i].art_size = g_res_art_size;
        g_albums[i].thm_clus = g_res_thm_clus;
        g_albums[i].thm_size = g_res_thm_size;
    }
}

/* Load the whole library from the host-built CORELIB.IDX in ONE streamed pass
 * (no per-file tag reads) — instant Songs/Genres/durations/disc. Returns 1 on
 * success, 0 if the index is absent/bad (caller falls back to a scan).
 * Record (256B, LE): u32 dur, u16 track, u16 disc, folder[64], file[64],
 * title[48], artist[40], genre[24], pad[8]. */
static void library_finish(void);        /* sort + genre counts (shared)        */

/* The library root: the "Music" folder if present, else the volume root (kept
 * for back-compat). Music/ holds the album folders + CORELIB.IDX, so the FAT
 * root stays clean (core.ipod / loader.cfg / Apple system folders only). */
static uint32_t g_lib_root_clus;
static int lib_root_cb(void *ud, const fat32_dirent_t *e)
{
    (void)ud;
    if (e->is_dir && name_eq_ci(e->name, "Music")) {
        g_lib_root_clus = e->first_clus;
    }
    return 0;
}
static uint32_t lib_root(fat32_t *fs)
{
    g_lib_root_clus = 0;
    fat32_readdir(fs, fs->root_clus, lib_root_cb, 0);
    return g_lib_root_clus ? g_lib_root_clus : fs->root_clus;
}

static int library_load_index(fat32_t *fs)
{
    g_idx_clus = 0;
    g_folder_n = 0;
    fat32_readdir(fs, lib_root(fs), index_root_cb, 0);
    if (g_idx_clus == 0) return 0;

    fat32_stream_t st;
    fat32_stream_open(&st, fs, g_idx_clus, g_idx_size);
    uint8_t hdr[12];
    if (fat32_stream_read(&st, hdr, 12) != 12) return 0;
    if (hdr[0] != 'C' || hdr[1] != 'I' || hdr[2] != 'D' || hdr[3] != 'X') return 0;
    int rec = hdr[6] | (hdr[7] << 8);
    if (rec != 256) return 0;
    uint32_t count = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                     ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);

    g_songs_n = g_genres_n = g_albums_n = 0;
    /* Read the index in 16 KB batches (64 records) rather than 256 B at a time:
     * a 256 B stream read pulls a whole 2048 B FS-sector and hands back 256 B, so
     * per-record reads re-fetched each sector 8x. Batching is ~14 big reads for
     * the whole index instead of ~900 tiny ones — the bulk of the load time. */
    static uint8_t idxbuf[64 * 256];
    uint32_t n = 0;
    while (n < count && g_songs_n < LIB_MAX_SONGS) {
        uint32_t batch = count - n;
        if (batch > 64) batch = 64;
        int32_t got = fat32_stream_read(&st, idxbuf, batch * 256u);
        if (got <= 0) break;
        uint32_t recs = (uint32_t)got / 256u;
        if (recs == 0) break;
        for (uint32_t k = 0; k < recs && g_songs_n < LIB_MAX_SONGS; k++) {
            const uint8_t *r = idxbuf + k * 256u;
            char folder[NAME_MAX + 1];
            field_copy(folder, sizeof folder, r + 8, 64);
            uint32_t folder_hash = (uint32_t)r[248] | ((uint32_t)r[249] << 8) |
                                   ((uint32_t)r[250] << 16) | ((uint32_t)r[251] << 24);
            uint32_t file_hash   = (uint32_t)r[252] | ((uint32_t)r[253] << 8) |
                                   ((uint32_t)r[254] << 16) | ((uint32_t)r[255] << 24);
            uint32_t dc = folder_clus_h(folder_hash, folder);
            if (dc == 0) continue;             /* album not present on disk */
            lib_song_t *s = &g_songs[g_songs_n];
            s->dir_clus   = dc;
            s->file_hash  = file_hash;
            s->duration_s = (uint32_t)r[0] | ((uint32_t)r[1] << 8) |
                            ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
            s->track = (uint16_t)(r[4] | (r[5] << 8));
            s->disc  = (uint16_t)(r[6] | (r[7] << 8));
            field_copy(s->title,  LIB_TITLE_MAX,  r + 136, 48);
            field_copy(s->artist, LIB_ARTIST_MAX, r + 184, 40);
            field_copy(s->file,   LIB_FILE_MAX,   r + 72,  64);
            /* trim the extension (LAST '.') so it matches g_browse display names,
             * which drop the ".flac" — but keep dots inside the title. */
            { int dot = -1;
              for (int j = 0; s->file[j]; j++) if (s->file[j] == '.') dot = j;
              if (dot > 0) s->file[dot] = '\0'; }
            char genre[LIB_GENRE_MAX];
            field_copy(genre, LIB_GENRE_MAX, r + 224, 24);
            s->genre = genre_intern(genre);
            g_songs_n++;
            album_intern(folder, dc);          /* album has >=1 indexed song */
        }
        n += recs;
        load_bar("Loading Library",            /* first ~75% = reading the index */
                 count ? (int)(n * 75u / count) : 0);
    }
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_indexed = 1;
    g_lib_scanned = 1;
    return 1;
}

/* Walk the library root (Music/ or the volume root) → album folders → FLAC
 * files, probing each file's tags. */
static void library_scan(fat32_t *fs)
{
    if (g_lib_scanned) return;
    g_songs_n = g_genres_n = g_scan_dirs_n = g_albums_n = 0;
    fat32_readdir(fs, lib_root(fs), scan_dirs_cb, 0);
    for (int d = 0; d < g_scan_dirs_n && g_songs_n < LIB_MAX_SONGS; d++) {
        g_scan_files_n = 0;
        g_scan_art_clus = g_scan_art_size = 0;
        fat32_readdir(fs, g_scan_dirs[d], scan_files_cb, 0);
        for (int i = 0; i < g_scan_files_n && g_songs_n < LIB_MAX_SONGS; i++) {
            player_pump();                 /* keep audio fed during the scan     */
            if ((g_songs_n & 31) == 0) {   /* live progress every 32 tracks       */
                console_clear(LINEN_SURFACE);
                ui_text_centered(112, "Building Library", FONT_TITLE, LINEN_INK);
                char pg[16];
                int pl = u32_to_dec(pg, (unsigned)g_songs_n);
                for (const char *p = " songs"; *p; p++) pg[pl++] = *p;
                pg[pl] = '\0';
                ui_text_centered(134, pg, FONT_SUB, LINEN_MUTED);
                lcd_present_fb(console_framebuffer());
            }
            flac_meta_t m;
            int ok = (player_probe_meta(g_scan_files[i].clus,
                                        g_scan_files[i].size, &m) == 0);
            lib_song_t *s = &g_songs[g_songs_n++];
            const char *t = (ok && m.have && m.title[0]) ? m.title
                                                         : g_scan_files[i].name;
            field_copy(s->title, LIB_TITLE_MAX, (const uint8_t *)t,
                       (int)sizeof s->title);
            if (ok && m.have && m.artist[0])
                field_copy(s->artist, LIB_ARTIST_MAX, (const uint8_t *)m.artist, 64);
            else
                s->artist[0] = '\0';
            field_copy(s->file, LIB_FILE_MAX,
                       (const uint8_t *)g_scan_files[i].name, NAME_MAX);
            s->file_hash  = 0;                 /* scan path resolves by name */
            s->dir_clus   = g_scan_dirs[d];
            s->duration_s = (ok && m.have) ? m.duration_s : 0;
            s->track      = (ok && m.have) ? (uint16_t)m.track : 0;
            s->disc       = 0;
            s->genre      = (ok && m.have) ? genre_intern(m.genre) : -1;
        }
    }
    library_finish();
    library_resolve_art(fs);               /* index each album's cover clusters */
    g_lib_scanned = 1;
}

/* Shared post-load: title-sort the index array + precompute per-genre counts. */
static void library_finish(void)
{
    for (int i = 0; i < g_songs_n; i++) g_song_sorted[i] = (uint16_t)i;
    for (int i = 1; i < g_songs_n; i++) {   /* insertion sort by title */
        uint16_t v = g_song_sorted[i];
        int j = i - 1;
        while (j >= 0 && title_cmp(g_songs[g_song_sorted[j]].title,
                                   g_songs[v].title) > 0) {
            g_song_sorted[j + 1] = g_song_sorted[j];
            j--;
        }
        g_song_sorted[j + 1] = v;
    }
    for (int i = 0; i < g_genres_n; i++) g_genre_count[i] = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int g = g_songs[i].genre;
        if (g >= 0 && g < g_genres_n) g_genre_count[g]++;
    }

    /* Alphabetise the album list A->Z by album title (insertion sort; the list
     * is small). g_albumview is derived from this, so the menu is sorted too. */
    for (int i = 1; i < g_albums_n; i++) {
        lib_album_t v = g_albums[i];
        char va[NAME_MAX + 1], vk[NAME_MAX + 1];
        split_artist_album(v.folder, va, vk);
        int j = i - 1;
        while (j >= 0) {
            char ja[NAME_MAX + 1], jk[NAME_MAX + 1];
            split_artist_album(g_albums[j].folder, ja, jk);
            if (title_cmp(jk, vk) <= 0) break;
            g_albums[j + 1] = g_albums[j];
            j--;
        }
        g_albums[j + 1] = v;
    }
}

/* Show a "building library" splash then scan (blocks; anti-skip covers audio). */
static void library_ensure(fat32_t *fs)
{
    if (g_lib_scanned) return;
    load_bar("Loading Library", 0);       /* the load phases fill this in */
    if (!library_load_index(fs))          /* host-built CORELIB.IDX */
        library_scan(fs);                  /* fallback: per-file tag scan     */
    build_artists();                       /* so About/Artists count is live */
}

/* Populate g_songview with the songs to show (genre < 0 = all), title-ordered. */
static void songview_build(int genre)
{
    g_songview_n = 0;
    for (int i = 0; i < g_songs_n; i++) {
        int si = g_song_sorted[i];
        if (genre < 0 || g_songs[si].genre == genre) {
            g_songview[g_songview_n++] = (uint16_t)si;
        }
    }
    g_song_sel = g_song_accum = 0;
}

/* Launch a library song in its album's queue (so Next/Prev walk the album). */
/* Play a song picked on the Songs list: the queue is the ENTIRE current song
 * view (all songs, in the displayed order), started at the picked track — so
 * "N of M" is the song's position in the whole library and Prev/Next walk every
 * song, not just the one album. (Same full-queue build as Shuffle Songs, minus
 * the shuffle.) */
static void library_play_song(fat32_t *fs, int songview_idx)
{
    (void)fs;
    if (songview_idx < 0 || songview_idx >= g_songview_n) return;
    uint16_t sel_song = g_songview[songview_idx];

    load_bar("Loading Songs", 0);
    player_set_shuffle(0);
    player_queue_begin();
    int start = 0, added = 0;
    for (int i = 0; i < g_songview_n; i++) {
        if ((i & 63) == 0) load_bar("Loading Songs", i * 100 / g_songview_n);
        lib_song_t *s = &g_songs[g_songview[i]];
        if (!s->file_clus) continue;              /* unresolved on disk — skip */
        if (g_songview[i] == sel_song) start = added;
        browse_entry_t e;
        int k = 0;
        for (; s->file[k] && k < NAME_MAX; k++) e.name[k] = s->file[k];
        e.name[k]  = '\0';
        e.clus     = s->file_clus;
        e.size     = s->file_size;
        e.fmt      = 0;
        e.is_dir   = 0;
        int ai = album_by_clus(s->dir_clus);
        e.art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
        e.art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
        player_queue_add(&e);
        added++;
    }
    load_bar("Loading Songs", 100);
    player_queue_commit(start);
    hal_volume_set(g_volume);
}

/* Tiny LCG for the shuffle pick (no libc; seeded from the free-running timer so
 * each invocation differs). */
static uint32_t g_rng;
static uint32_t rng_next(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 1;                       /* drop the low bit (poor LCG entropy) */
}

/* "Shuffle Songs": fill the play queue with tracks drawn from randomly-ordered
 * albums (bounded by the queue capacity) and start in shuffle mode, so the
 * order is randomised too. A fresh random draw each time it's chosen. Mixed
 * albums => no single cover, so the now-playing art is left empty. */
static void shuffle_songs_play(fat32_t *fs)
{
    library_ensure(fs);
    if (g_songs_n == 0) return;

    load_bar("Shuffling Songs", 0);

    /* Shuffle ALL song indices (Fisher-Yates) — the WHOLE library, not a sample. */
    static uint16_t ord[LIB_MAX_SONGS];
    int ns = g_songs_n;
    for (int i = 0; i < ns; i++) ord[i] = (uint16_t)i;
    g_rng = mmio_read32(USEC_TIMER_ADDR) | 1u;
    for (int i = ns - 1; i > 0; i--) {
        int j = (int)(rng_next() % (uint32_t)(i + 1));
        uint16_t t = ord[i]; ord[i] = ord[j]; ord[j] = t;
    }

    /* Build the full queue from the resolved song index (file cluster + per-track
     * album cover) — every song, in the shuffled order. The Queue view then shows
     * that order, and Now Playing shows each song's own art. */
    player_set_shuffle(0);                 /* already shuffled; play in order */
    player_queue_begin();
    for (int i = 0; i < ns; i++) {
        if ((i & 63) == 0) load_bar("Shuffling Songs", i * 100 / ns);
        lib_song_t *s = &g_songs[ord[i]];
        if (!s->file_clus) continue;       /* unresolved (missing on disk) — skip */
        browse_entry_t e;
        int k = 0;
        for (; s->file[k] && k < NAME_MAX; k++) e.name[k] = s->file[k];
        e.name[k]  = '\0';
        e.clus     = s->file_clus;
        e.size     = s->file_size;
        e.fmt      = 0;                     /* library is FLAC */
        e.is_dir   = 0;
        int ai = album_by_clus(s->dir_clus);
        e.art_clus = (ai >= 0) ? g_albums[ai].art_clus : 0;
        e.art_size = (ai >= 0) ? g_albums[ai].art_size : 0;
        player_queue_add(&e);
    }
    load_bar("Shuffling Songs", 100);
    player_queue_commit(0);
    hal_volume_set(g_volume);
}

static void songs_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_songview_n > 0) fmt_count(right, sel + 1, g_songview_n);
    else                  right[0] = '\0';
    header_render("Songs", right, 1);
    if (g_songview_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No songs", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_songview_n, LIST_ROWS2);
    for (int r = 0; r < LIST_ROWS2; r++) {
        int idx = top + r;
        if (idx >= g_songview_n) break;
        lib_song_t *sg = &g_songs[g_songview[idx]];
        char dur[8];
        if (sg->duration_s) fmt_time(dur, sg->duration_s); else dur[0] = '\0';
        list_row_titled(r, sg->title, sg->artist[0] ? sg->artist : 0,
                        dur[0] ? dur : 0, idx == sel, 0);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS2, g_songview_n);
}

static void genres_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    char right[12];
    if (g_genres_n > 0) fmt_count(right, sel + 1, g_genres_n);
    else                right[0] = '\0';
    header_render("Genres", right, 1);
    if (g_genres_n == 0) {
        ui_text(14, LIST_Y0 + 20, "No genres", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, g_genres_n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_genres_n) break;
        char cnt[8];
        u32_to_dec(cnt, (unsigned)g_genre_count[idx]);
        list_row(r, g_genres[idx], 0, cnt, 0, idx == sel, 0, 0);
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS, g_genres_n);
}

/* ---------------------------------------------------------------------------
 * Queue view: the tracks queued in the current playback (the folder a track was
 * launched from). The playing track carries the animated now-playing bars;
 * SELECT jumps to a track. Reached via SELECT on the Now Playing screen.
 * ------------------------------------------------------------------------- */
static int g_queue_sel, g_queue_accum;

static void queue_render(int sel)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    int n   = player_queue_len();
    int cur = player_queue_current();
    char right[12];
    if (n > 0) fmt_count(right, cur + 1, n);
    else       right[0] = '\0';
    header_render("Now Playing", right, 1);

    if (n == 0) {
        ui_text(14, LIST_Y0 + 20, "Queue empty", FONT_ROW, LINEN_MUTED);
        return;
    }
    int top = scroll_window(sel, n, LIST_ROWS);
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= n) break;
        int is_sel = (idx == sel);
        list_row(r, track_display(player_queue_name(idx)), 0, 0, 0, is_sel,
                 player_queue_is_dir(idx), 0);
        if (idx == cur) {                 /* the currently-playing track */
            int ry = LIST_Y0 + r * ROW_H;
            nowplaying_bars(LCD_WIDTH - 22, ry + 7,
                            is_sel ? LINEN_SEL_FG : LINEN_INK,
                            mmio_read32(USEC_TIMER_ADDR));
        }
    }
    scrollbar_render(LIST_Y0, top, LIST_ROWS, n);
}

/* ---------------------------------------------------------------------------
 * Settings (core/ui/settings.h data-driven model). g_settings holds the state;
 * g_set_screen tracks the current sub-screen within the one SCR_SETTINGS stack
 * entry. Slider rows use a brief edit mode (SELECT toggles it, then the wheel
 * adjusts) so the wheel can still move the selection otherwise.
 * ------------------------------------------------------------------------- */
static settings_t g_settings;
static int g_set_screen;                  /* current settings_screen_t          */
static int g_set_sel;                     /* selection within g_set_screen      */
static int g_set_root_sel;                /* saved ROOT selection               */
static int g_set_accum;                   /* wheel accumulator                  */
static int g_set_editing;                 /* editing a slider row               */

/* Push the FUNCTIONAL settings out to the subsystems. Cosmetic fields are a
 * no-op. The backlight timeout/brightness are read live by the loop. */
static void settings_apply(void)
{
    player_set_shuffle(g_settings.shuffle);
    player_set_repeat((int)g_settings.repeat);
    g_volume = g_settings.volume;
    hal_volume_set(g_volume);
    hal_balance_set(g_settings.balance);
    hal_tone_set(g_settings.bass, g_settings.treble);
    theme_set(g_settings.theme);           /* Linen / Onyx -> live palette swap */
}

static void settings_render_cur(void)
{
    if (g_set_screen == SETTINGS_ABOUT) {
        settings_about_render(g_bat_pct, g_bat_mv, g_total_mb, g_free_mb,
                              g_songs_n, g_albums_n, g_artists_n);
    } else {
        settings_render(g_set_screen, &g_settings, g_set_sel);
    }
}

/* Boot splash: Nunito CORE branding on the Linen surface the moment the panel
 * is ours, so the disk-spin-up / mount delay reads as "loading" rather than the
 * leftover chainloader framebuffer. */
static void boot_splash(void)
{
    console_clear(LINEN_SURFACE);
    ui_text_centered(120, "Core Player", FONT_TITLE, LINEN_INK);
    ui_text_centered(142, "loading",     FONT_SUB,   LINEN_MUTED);
    lcd_present_fb(console_framebuffer());
}

/* Format `s` seconds as "M:SS" (minutes uncapped). */
static void fmt_time(char *buf, uint32_t s)
{
    uint32_t m = s / 60, ss = s % 60;
    int i = 0;
    if (m >= 10) buf[i++] = (char)('0' + (m / 10) % 10);
    buf[i++] = (char)('0' + m % 10);
    buf[i++] = ':';
    buf[i++] = (char)('0' + ss / 10);
    buf[i++] = (char)('0' + ss % 10);
    buf[i]   = '\0';
}

/* Right-side circle arc (a ")" shape): radius R, +/- span rows tall, centred at
 * (cx, cy); ~2px thick. The speaker's sound waves + the lock shackle use it. */
/* A 1px-thin right-opening crescent (radius R, +/-span rows), anchored so its
 * near point sits by the speaker cone — the skinny sound wave. */
static void draw_arc_thin(int cx, int cy, int R, int span, uint16_t c)
{
    for (int dy = -span; dy <= span; dy++) {
        int dx = isqrt_i(R * R - dy * dy);
        console_fill_rect(cx + dx, cy + dy, 1, 1, c);
    }
}

/* Top half of an annulus (ring): outer radius Ro, inner Ri, centred (cx, cy) —
 * a padlock shackle arch. Rows above the inner hole are a solid cap. */
static void draw_ring_top(int cx, int cy, int Ro, int Ri, uint16_t c)
{
    for (int dy = -Ro; dy <= 0; dy++) {
        int xo = isqrt_i(Ro * Ro - dy * dy);
        if (-dy <= Ri) {
            int xi = isqrt_i(Ri * Ri - dy * dy);
            console_fill_rect(cx - xo, cy + dy, xo - xi + 1, 1, c);   /* left band  */
            console_fill_rect(cx + xi, cy + dy, xo - xi + 1, 1, c);   /* right band */
        } else {
            console_fill_rect(cx - xo, cy + dy, 2 * xo + 1, 1, c);    /* solid cap  */
        }
    }
}

/* A small right-pointing speaker: driver cabinet + cone, then sound waves that
 * scale with `vol` — two waves loud, one wave quiet, and a mute "X" at 0. */
static void draw_speaker(int sx, int sy, uint16_t c, int vol)
{
    fill_round_rect(sx - 8, sy - 3, 4, 6, 1, c);          /* cabinet             */
    for (int dx = 0; dx <= 4; dx++) {                     /* cone, opening right */
        int half = 2 + dx;
        console_fill_rect(sx - 4 + dx, sy - half, 1, 2 * half, c);
    }
    if (vol <= 0) {                                       /* muted: an X         */
        for (int i = 0; i < 6; i++) {
            console_fill_rect(sx + 3 + i, sy - 3 + i, 2, 1, c);   /* '\'         */
            console_fill_rect(sx + 3 + i, sy + 2 - i, 2, 1, c);   /* '/'         */
        }
        return;
    }
    /* Three skinny (1px) crescent waves anchored at the cone front, growing with
     * volume — closer + thinner than the old 2px arcs (chosen design "B"). */
    if (vol > 5)  draw_arc_thin(sx, sy, 3, 2, c);         /* wave 1: hairline    */
    if (vol > 40) draw_arc_thin(sx, sy, 6, 4, c);         /* wave 2              */
    if (vol > 72) draw_arc_thin(sx, sy, 9, 5, c);         /* wave 3              */
}

/* Centered volume overlay plate (volume-demo.jsx VolumeOverlay): speaker glyph +
 * ink fill bar + big percent, on a light near-surface plate. */
static void volume_overlay_render(int vol)
{
    const int PX = 60, PY = 101, PW = 200, PH = 32;
    fill_round_rect_aa(PX, PY, PW, PH, 8, LINEN_PLATE);    /* raised plate, AA r8  */

    draw_speaker(PX + 16, PY + PH / 2, LINEN_INK, vol);

    /* Fill bar. */
    int bx = PX + 34, by = PY + PH / 2 - 3, bw = PW - 34 - 42, bh = 6;
    console_fill_rect(bx, by, bw, bh, LINEN_TRK);          /* track rgba(ink,0.12) */
    int fw = bw * vol / 100;
    if (fw < 0) fw = 0;
    if (fw > bw) fw = bw;
    console_fill_rect(bx, by, fw, bh, LINEN_INK);

    /* Percent, right-aligned. */
    char p[5];
    u32_to_dec(p, (unsigned)vol);
    int w = text_width(p, text_font_bold_11());       /* percent is 11/700       */
    ui_text(PX + PW - 14 - w, PY + PH / 2 + 4, p, text_font_bold_11(), LINEN_INK);
}

/* Padlock for the lock/unlock plate: a rounded body with a keyhole (punched in
 * the plate colour `bg`) + a curved shackle. Closed = the shackle arch sits
 * latched on the body; open = it's swung up and left so the right end floats off
 * the body (the gap reads as unlatched). `cy` is the body's top edge. */
/* Padlock shackle: a thick semicircular arch (outer radius Ro, inner Ri) centred
 * at (sx, ay) on its diameter line, plus two straight prongs dropping from the
 * arch ends. lL/rL are the left/right prong lengths (independent so the open
 * state can raise one prong). Prong thickness matches the arch band so the join
 * is seamless. Drawn BEFORE the body so the body covers the seated prong feet. */
static void draw_shackle(int sx, int ay, int Ro, int Ri,
                         int lL, int rL, uint16_t c)
{
    int w = Ro - Ri + 1;                              /* band/prong thickness */
    draw_ring_top(sx, ay, Ro, Ri, c);                /* semicircular top     */
    console_fill_rect(sx - Ro, ay, w, lL, c);        /* left prong           */
    console_fill_rect(sx + Ri, ay, w, rL, c);        /* right prong          */
}

/* Keyhole punched in the plate colour `bg`, centred at (kx, ky): a round hole
 * with a tapered slot widening downward, symmetric about kx. */
/* Minimal keyhole: a small round hole + short slot, punched in the plate bg. */
static void draw_keyhole(int kx, int ky, uint16_t bg)
{
    fill_round_rect(kx - 3, ky - 3, 6, 6, 3, bg);    /* round hole (~d6)      */
    console_fill_rect(kx - 1, ky + 2, 2, 4, bg);     /* short slot            */
}

/* Padlock for the lock/unlock plate (design: "Minimal dot"). `cy` is the
 * VERTICAL CENTRE (~32 wide x ~40 tall). `c` = icon colour, `bg` = plate colour.
 * LOCKED = shackle latched, both prongs seated; UNLOCKED = shackle popped
 * straight UP, floating clear of the body (visible gap beneath). */
static void draw_lock_icon(int cx, int cy, int open, uint16_t c, uint16_t bg)
{
    if (open)
        draw_shackle(cx, cy - 14, 10, 6, 6, 6, c);   /* lifted straight up   */
    else
        draw_shackle(cx, cy - 9,  10, 6, 8, 8, c);   /* latched              */

    fill_round_rect(cx - 16, cy - 3, 32, 22, 4, c);  /* body, over prong feet */
    draw_keyhole(cx, cy + 7, bg);
}

/* Centered lock/unlock plate (system-screens.jsx LockedScreen/UnlockedScreen):
 * LOCKED = dark plate + light closed lock; UNLOCKED = light plate + dark open
 * lock. Drawn over whatever screen is currently in the framebuffer. */
static void lock_plate_render(int locked)
{
    const int PX = 70, PY = 65, PW = 180, PH = 110;
    uint16_t plate = locked ? LINEN_INK : LINEN_SURFACE;
    uint16_t fg    = locked ? LINEN_SURFACE : LINEN_INK;
    if (!locked) {                                    /* light plate: draw a ring */
        fill_round_rect_aa(PX - 1, PY - 1, PW + 2, PH + 2, 11, LINEN_BORDER);
    }
    fill_round_rect_aa(PX, PY, PW, PH, 10, plate);
    draw_lock_icon(PX + PW / 2, PY + 45, !locked, fg, plate);  /* cy = vertical centre */
    const char *label = locked ? "LOCKED" : "UNLOCKED";
    int w = text_width(label, FONT_HEADER);
    ui_text(PX + (PW - w) / 2, PY + 84, label, FONT_HEADER, fg);
}

/* Now-playing album art at 120x120 — the stored folder.art's native size, so it
 * blits straight through at full quality (no downscale) and fills the art-left
 * layout. Only art larger than this is box-downscaled (cached per track). */
#define NP_ART_DIM 120
static uint16_t g_np_art[NP_ART_DIM * NP_ART_DIM];
static int      g_np_art_key = -1;

static const uint16_t *np_art_120(void)
{
    if (!player_art_ok()) { g_np_art_key = -1; return 0; }
    int w = player_art_w(), h = player_art_h();
    if (w == NP_ART_DIM && h == NP_ART_DIM) {
        return player_art_pixels();        /* native size: blit as-is */
    }
    int key = player_queue_current();
    if (key != g_np_art_key) {
        thumb_downscale_rgb565(player_art_pixels(), w, h,
                               g_np_art, NP_ART_DIM, NP_ART_DIM);
        g_np_art_key = key;
    }
    return g_np_art;
}

/* Now-playing (Linen, interactive-ipod.jsx Theme1Live): a top status row
 * ("Now Playing"/"Paused" + shuffle/repeat + battery), then 88x88 art on the
 * LEFT with a metadata column to its right (SONG N OF M / title / artist /
 * album), and the elapsed / −remaining times + progress bar pinned to the
 * bottom. Art + metadata come from the PLAYING folder, not the browsed one. */
static void nowplaying_render(const char *name, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct)
{
    (void)buf_pct;
    console_clear(LINEN_SURFACE);

    /* --- top status row: context label left, state + battery right --------- */
    ui_text(12, 15, player_paused() ? "Paused" : "Now Playing",
            text_font_bold_11(), LINEN_INK);

    int bx = LCD_WIDTH - 12 - 19;                     /* battery block         */
    draw_battery(bx, 3, g_bat_pct);
    int rc = bx - (g_locked ? 18 : 6);                /* right edge for tokens */
    if (g_locked) {
        draw_lock_glyph(bx - 14, 3, LINEN_INK);
    }
    /* Compact shuffle / repeat tokens, right-aligned before the battery. */
    {
        char st[16]; int p = 0;
        if (g_settings.shuffle) { const char *s = "SHUF"; while (*s) st[p++] = *s++; }
        if (g_settings.repeat != REPEAT_OFF) {
            if (p) st[p++] = ' ';
            const char *s = (g_settings.repeat == REPEAT_ONE) ? "RPT1" : "RPT";
            while (*s) st[p++] = *s++;
        }
        st[p] = '\0';
        if (p) {
            int w = text_width(st, FONT_SMALL);
            ui_text(rc - w, 13, st, FONT_SMALL, LINEN_MUTED2);
        }
    }

    /* --- art on the left (native 120x120) ---------------------------------- */
    const int ax = 16, ay = 44;
    const uint16_t *art = np_art_120();
    if (art) {
        console_blit565(ax, ay, NP_ART_DIM, NP_ART_DIM, art);
    }

    /* --- metadata column, vertically centred beside the taller art --------- */
    const flac_meta_t *m = player_meta();
    const char *title = (m->have && m->title[0]) ? m->title : track_display(name);
    int mx = ax + NP_ART_DIM + 14;                    /* metadata left edge    */
    int mr = LCD_WIDTH - 14;                           /* metadata right edge   */

    /* "TRACK N OF M" eyebrow above the title (design's "Track 04 of 11"). */
    int tot = player_queue_len();
    if (tot > 0) {
        char num[24];
        int p = 0;
        for (const char *q = "TRACK   "; *q; q++) num[p++] = *q;   /* air after TRACK */
        p += u32_to_dec(num + p, (unsigned)(player_queue_current() + 1));
        for (const char *q = "     OF     "; *q; q++) num[p++] = *q;  /* extra air both sides */
        p += u32_to_dec(num + p, (unsigned)tot);
        num[p] = '\0';
        ui_text(mx, 72, num, FONT_SMALL, LINEN_MUTED2);
    }

    /* Title (bold 17): drawn at its live scroll offset + registered as the
     * marquee, so volume/lock full repaints never flash the truncated title. */
    mq_text(mx, 94, mr - mx, title, FONT_TITLE, LINEN_INK, LINEN_SURFACE,
            0, LCD_HEIGHT);   /* now-playing title has room — no row clip */

    if (m->have && m->artist[0]) {
        text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, mx, 114, m->artist,
                       FONT_SUB, LINEN_MUTED_D, mx, mr);
    }
    if (m->have && m->album[0]) {
        text_draw_clip(console_fb(), LCD_WIDTH, LCD_HEIGHT, mx, 130, m->album,
                       FONT_SUB, LINEN_MUTED2, mx, mr);
    }

    /* --- times + a bigger, rounded progress bar centred in the lower band --- */
    char te[12], tr[12];
    fmt_time(te, elapsed_s);
    uint32_t rem = (total_s > elapsed_s) ? total_s - elapsed_s : 0;
    tr[0] = '-';
    fmt_time(tr + 1, rem);                             /* "−M:SS" remaining     */
    ui_text(18, 198, te, FONT_SUB, LINEN_MUTED_D);
    int wtr = text_width(tr, FONT_SUB);
    ui_text(LCD_WIDTH - 18 - wtr, 198, tr, FONT_SUB, LINEN_MUTED_D);

    /* Taller rounded-cap bar (INK fill on a faint ink track, Theme1Live fg).
     * bh 8 with AA pill caps reads smoother than the old 6px integer-stepped
     * caps. */
    int pbx = 18, by = 209, bw = LCD_WIDTH - 36, bh = 8;
    fill_round_rect_aa(pbx, by, bw, bh, bh / 2, LINEN_TRK);
    int fw = (total_s > 0) ? (int)((elapsed_s * (uint32_t)bw) / total_s) : 0;
    if (fw > bw) fw = bw;
    if (fw >= bh) {
        fill_round_rect_aa(pbx, by, fw, bh, bh / 2, LINEN_INK);
    } else if (fw > 0) {
        console_fill_rect(pbx, by, fw, bh, LINEN_INK);
    }

    /* Volume overlay rides on top for ~1.5 s after a wheel adjustment. */
    if (mmio_read32(USEC_TIMER_ADDR) < g_vol_show_until) {
        volume_overlay_render(g_volume);
    }
}

/* Now-playing animated strip: the album art (y 8..128) is static for a track,
 * so we full-present it once then partial-present only this lower band each
 * second — shrinking the IRQ-masked pixel push (relies on the BCM persisting
 * the un-repainted art region across a partial LCD_UPDATE). */
#define NP_ANIM_Y 128
#define NP_ANIM_H 112

/* ---------------------------------------------------------------------------
 * Menus (main + Music sub-menu)
 *
 * Both are the same widget over a small {label, active} item list. Inactive
 * items render greyed and SELECT does nothing (features not yet built). The
 * renderer reuses the list row geometry (LIST_Y0/ROW_H); neither menu exceeds
 * LIST_ROWS, so no scrolling window is needed.
 * ------------------------------------------------------------------------- */
typedef struct { const char *label; uint8_t active; } menu_item_t;

/* Main menu. ACTIVE: Music, Now Playing (Now Playing greyed until something is
 * playing — its `active` flag is refreshed from the player before each paint). */
enum { MM_MUSIC, MM_PLAYLISTS, MM_PODCASTS, MM_AUDIOBOOKS, MM_SETTINGS,
       MM_NOWPLAYING, MM_COUNT };
static menu_item_t g_main_menu[MM_COUNT] = {
    { "Music",       1 },
    { "Playlists",   0 },
    { "Podcasts",    0 },
    { "Audiobooks",  0 },
    { "Settings",    1 },
    { "Now Playing", 1 },
};
static int g_main_sel;

/* Music sub-menu. ACTIVE: Albums (enters the junk-filtered folder browser). */
enum { MU_PLAYLISTS, MU_ARTISTS, MU_ALBUMS, MU_SONGS, MU_SHUFFLE, MU_GENRES,
       MU_COMPOSERS, MU_AUDIOBOOKS, MU_COUNT };
static const menu_item_t g_music_menu[MU_COUNT] = {
    { "Playlists",     0 },
    { "Artists",       1 },
    { "Albums",        1 },
    { "Songs",         1 },
    { "Shuffle Songs", 1 },
    { "Genres",        1 },
    { "Composers",     0 },
    { "Audiobooks",    0 },
};
static int g_music_sel;

/* Shared wheel accumulator for the menu screens. */
static int g_menu_accum;

/* Generic menu renderer over a {label, active} list. `back` draws the header
 * back chevron (off for the root menu). */
static void menu_render_list(const char *title, const menu_item_t *items,
                             int n, int sel, int back)
{
    console_clear(LINEN_SURFACE);
    status_strip_render();
    header_render(title, "", back);
    for (int i = 0; i < n && i < LIST_ROWS; i++) {
        list_row(i, items[i].label, 0, 0, 1 /*chevron*/, i == sel,
                 !items[i].active /*greyed*/, 0);
    }
}

/* Visible main-menu row count: "Now Playing" (the last item) only appears while
 * a track is loaded, so it's simply dropped from the count when idle. */
static int main_menu_count(void)
{
    return player_active() ? MM_COUNT : MM_COUNT - 1;
}

static void main_menu_render(void)
{
    int n = main_menu_count();
    if (g_main_sel >= n) g_main_sel = n - 1;   /* cursor was on a now-gone row */
    menu_render_list("Core", g_main_menu, n, g_main_sel, 0);
}

static void music_menu_render(void)
{
    menu_render_list("Music", g_music_menu, MU_COUNT, g_music_sel, 1);
}

/* ---------------------------------------------------------------------------
 * Screen stack
 * ------------------------------------------------------------------------- */
typedef enum { SCR_MENU, SCR_MUSIC, SCR_ARTISTS, SCR_SONGS, SCR_GENRES,
               SCR_BROWSER, SCR_NOWPLAYING, SCR_QUEUE, SCR_SETTINGS,
               SCR_CHARGING } screen_t;
#define SCR_STACK_MAX 8
static screen_t g_scr[SCR_STACK_MAX];
static int      g_scr_n;

static void      scr_push(screen_t s) { if (g_scr_n < SCR_STACK_MAX) g_scr[g_scr_n++] = s; }
static void      scr_pop(void)        { if (g_scr_n > 1) g_scr_n--; }
static screen_t  scr_cur(void)        { return g_scr[g_scr_n - 1]; }

/* Browser view state (was local to the old browse loop). The album LIST (depth
 * 0) and a single album's TRACKLIST (depth 1) keep SEPARATE selections, so
 * backing out of an album returns the cursor to that album in the list rather
 * than jumping to the top. */
static int g_br_sel, g_br_accum;      /* album list (depth 0)  */
static int g_det_sel, g_det_accum;    /* tracklist   (depth 1) */

/* Read an album's tracklist (the folder at `dir_clus`) into g_browse. Only ever
 * called at depth 1 now — the album LIST is the index-driven g_albums. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    fat32_readdir(fs, dir_clus, browse_collect, 0);
}

/* After entering an album folder: load its hero art, pull each track's
 * disc/duration/number from the index (matched by folder+filename), and build
 * the display view with "Disc N" section headers for a multi-disc album. */
static void detail_load_meta(fat32_t *fs)
{
    detail_art_load(fs);
    g_album_track_n = 0;
    int maxd = 0;
    for (int i = 0; i < g_browse_n; i++) {
        g_track_dur[i] = 0;
        g_track_disc[i] = 0;
        g_track_num[i] = 0;
        g_track_title[i] = 0;
        if (g_browse[i].is_dir) continue;
        g_album_track_n++;
        if (g_lib_indexed) {
            for (int s = 0; s < g_songs_n; s++) {
                if (g_songs[s].dir_clus == g_cur_dir &&
                    name_eq_ci(g_songs[s].file, g_browse[i].name)) {
                    g_track_dur[i]  = (uint16_t)g_songs[s].duration_s;
                    g_track_disc[i] = (uint8_t)g_songs[s].disc;
                    g_track_num[i]  = g_songs[s].track;
                    if (g_songs[s].title[0]) g_track_title[i] = g_songs[s].title;
                    if (g_songs[s].disc > maxd) maxd = g_songs[s].disc;
                    break;
                }
            }
        }
    }
    g_detail_multidisc = (maxd > 1);
    g_det_view_n = 0;
    int prev = -1;
    for (int i = 0; i < g_browse_n && g_det_view_n < DET_VIEW_MAX - 1; i++) {
        if (g_browse[i].is_dir) continue;
        int d = g_track_disc[i];
        if (g_detail_multidisc && d > 0 && d != prev) {
            g_det_view[g_det_view_n++] = (int16_t)(-(d + 1));   /* Disc header */
            prev = d;
        }
        g_det_view[g_det_view_n++] = (int16_t)i;
    }
}

/* Busy-wait `us` microseconds (PWM already off) — for the silent gap in the
 * multi-burst clicker profiles. Short enough not to underrun the decode buffer. */
static void ui_spin_us(uint32_t us)
{
    uint32_t t0 = mmio_read32(USEC_TIMER_ADDR);
    while ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - t0) < us) { /* spin */ }
}

/* Piezo navigation click. g_settings.clicker selects the sound profile (0 = Off;
 * 1..7 = Tick / Click / Pop / Blip / Tock / Double / Chirp). Single-tone
 * profiles are one (Hz, µs) burst; Double is two bursts with a gap; Chirp is a
 * rising three-tone sweep. Order matches settings.c CLICK_L. */
static void ui_click(void)
{
    switch (g_settings.clicker) {
        case 1: piezo_click_ex(3000, 3000); break;   /* Tick  — crisp mid tick   */
        case 2: piezo_click_ex(4500, 2000); break;   /* Click — higher, shorter  */
        case 3: piezo_click_ex(1800, 5000); break;   /* Pop   — lower, fuller    */
        case 4: piezo_click_ex(6000, 1500); break;   /* Blip  — high, ultra-short*/
        case 5: piezo_click_ex(1000, 4000); break;   /* Tock  — low, woody       */
        case 6:                                       /* Double — two quick taps  */
            piezo_click_ex(4200, 1500);
            ui_spin_us(22000);
            piezo_click_ex(4200, 1500);
            break;
        case 7:                                       /* Chirp — rising sweep     */
            piezo_click_ex(3000, 1100);
            piezo_click_ex(4000, 1100);
            piezo_click_ex(5200, 1200);
            break;
        default: break;                               /* 0 = Off                  */
    }
}

/* Apply a wheel event to a selection index in [0, count) with acceleration. */
static int wheel_move(int sel, int count, int8_t delta, int *accum)
{
    int wd = delta;
    if (wd >  WHEEL_MAX_DELTA) wd =  WHEEL_MAX_DELTA;
    if (wd < -WHEEL_MAX_DELTA) wd = -WHEEL_MAX_DELTA;
    *accum += wd;
    int move = *accum / WHEEL_CLICKS_PER_ITEM;
    *accum -= move * WHEEL_CLICKS_PER_ITEM;
    int old = sel;
    sel += move;
    if (sel < 0)          sel = 0;
    if (sel >= count)     sel = count - 1;
    if (sel != old) {
        ui_click();        /* click only when the cursor actually advances */
    }
    return sel;
}

/* Render whatever screen is on top of the stack into the framebuffer (no
 * present) — used to paint context behind the lock/unlock plate. */
static void paint_current_screen(void)
{
    g_mq.active = 0;                       /* a fresh paint re-registers any marquee */
    switch (scr_cur()) {
    case SCR_MENU:    main_menu_render();  break;
    case SCR_MUSIC:   music_menu_render(); break;
    case SCR_ARTISTS: artists_render(g_artist_sel); break;
    case SCR_SONGS:   songs_render(g_song_sel);     break;
    case SCR_GENRES:  genres_render(g_genre_sel);   break;
    case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel); break;
    case SCR_QUEUE:   queue_render(g_queue_sel); break;
    case SCR_SETTINGS: settings_render_cur(); break;
    case SCR_NOWPLAYING:
        nowplaying_render(player_track_name(), player_elapsed_s(),
                          player_total_s(), player_buf_pct());
        break;
    case SCR_CHARGING:
        screen_charging_render(g_bat_pct, power_is_charging(), power_is_external());
        break;
    }
}

/* Quiesce and enter PMU deep-sleep standby (triggered by holding PLAY). Stops
 * audio + the decode/disk feed, blanks the panel + backlight, then hands off to
 * the PMU. Never returns — a button press wakes the device by re-running the
 * boot path (a cold boot of the firmware, not a resume). */
_Noreturn static void enter_standby(void)
{
    player_stop();
    console_clear(0x0000);                /* blank BEFORE the power cut so no */
    lcd_present_fb(console_framebuffer()); /* stale colour lingers on the panel */
    cpu_wait_ms(80);                      /* let the BCM push the black frame  */
    backlight_set(0);
    power_standby();                      /* PMU cuts power — does not return */
    for (;;) {
    }
}

/*
 * Suspend: the seamless "off". Keeps the CPU + RAM alive so wake RESUMES the
 * running firmware instantly (no cold boot, so no ipl2 menu) — but actually
 * quiesces the power-hungry parts: audio paused, the hard drive spun DOWN (ATA
 * standby), backlight off, panel blanked to black. Any button wakes it: spin
 * the drive back up, restore the screen, resume playback. Holding the trigger
 * PLAY past ~5s escalates to a true PMU power-down (everything off, but wakes
 * via a cold boot). `play_down_us` is when the hold began, for that escalation.
 *
 * Caveat vs a true power-down: the LCD controller + CPU stay powered (the panel
 * is dark, not electrically off), so it draws more than deep-sleep — fine for
 * short off/on, which is what this is for.
 */
static void suspend_to_ram(uint32_t play_down_us)
{
    wheel_event_t drain;
    int was_playing = player_active() && !player_paused();
    if (was_playing) {
        player_pause();                   /* silence + stop feeding the disk */
    }
    ata_standby();                        /* spin the platters down (quiet, low-power) */
    /* Clear to black BEFORE cutting the backlight, so the transflective panel
     * doesn't faintly ghost the last UI in ambient light while asleep. Wake
     * repaints the real screen while the backlight is still off (below), so the
     * black is never seen as a flash. */
    console_clear(0x0000);
    lcd_present_fb(console_framebuffer());
    backlight_set(0);

    /* Wait for the trigger PLAY hold to release (so it can't instantly wake us).
     * Held past ~5s total => a real power-down instead. */
    while (clickwheel_buttons() & WHEEL_BTN_PLAY) {
        if ((uint32_t)(mmio_read32(USEC_TIMER_ADDR) - play_down_us) > 5000000u) {
            enter_standby();              /* true off (PMU) — does not return */
        }
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }   /* drop the trigger's latched events */

    /* Low-power idle until any button is pressed. The 100 Hz tick keeps sampling
     * the wheel into the latch through each cpu_wait, so a press is seen fast. */
    while (clickwheel_buttons() == 0) {
        cpu_wait_ms(30);
    }
    /* Swallow the wake press so it isn't also acted on as navigation. */
    while (clickwheel_buttons() != 0) {
        cpu_wait_ms(20);
    }
    while (clickwheel_get_event(&drain)) { }

    ata_wakeup();                         /* spin the drive back up before any read */
    paint_current_screen();               /* render the real screen while dark... */
    lcd_present_fb(console_framebuffer());
    backlight_set(g_settings.backlight_bright);  /* ...then light up straight to it */
    if (was_playing) {
        player_resume();
    }
}

/*
 * The UI: one event loop that pumps the background player every pass and
 * dispatches input to the current screen (Main menu / Music menu / Browser /
 * Now Playing) on a stack. MENU pops the screen WITHOUT stopping playback, so a
 * song keeps going while you navigate. Never returns.
 */
_Noreturn static void run_ui(fat32_t *fs)
{
    clickwheel_init();
    player_init(fs);
    chip_placeholder_init();
    library_ensure(fs);                   /* preload the index at boot (drive is
                                           * spinning) so Songs/Albums/Artists/
                                           * Genres open INSTANTLY, like Apple —
                                           * not a multi-second stall on first use */
    battery_refresh(1);                   /* prime the status-strip gauge         */
    g_volume = hal_volume_get();          /* reflect the codec's default gain      */
    g_dir_depth = 0;
    g_browse_n  = 0;
    g_br_sel = g_br_accum = 0;
    g_main_sel  = 0;
    g_music_sel = MU_ALBUMS;
    g_menu_accum = 0;
    g_artist_filter[0] = '\0';
    g_artist_sel = g_artist_accum = 0;
    settings_defaults(&g_settings);
    settings_apply();                     /* push shuffle/repeat/volume defaults  */
    g_scr_n = 0;
    scr_push(SCR_MENU);

    int      dirty = 1;
    uint32_t np_last = 0xFFFFFFFFu;
    int      np_first = 1;
    int      np_vol_prev = 0;            /* volume overlay was up last NP paint  */
    int      np_track = -1;              /* queue index shown last NP full paint */
    uint32_t last_present = 0;           /* rate-limit UI presents while playing */
    uint32_t last_bars = 0;              /* rate-limit the now-playing bar anim  */
    uint32_t last_chip = 0;              /* rate-limit album-cover chip loads    */
    uint32_t last_mq = 0;                /* rate-limit the marquee scroll        */
    int      hold_prev = clickwheel_hold() ? 1 : 0;  /* seed hold-edge detect    */
    int      ext_prev  = power_is_external() ? 1 : 0; /* seed plug-in edge detect */
    int      lock_flashing = 0;          /* a lock/unlock plate is on screen     */
    int      play_held = 0;              /* PLAY currently down (long-press off)  */
    uint32_t play_down_us = 0;           /* when PLAY went down                   */
    g_locked = hold_prev;

    /* Backlight inactivity: full -> dim -> off. Any input wakes to full; a press
     * that wakes from fully-OFF is swallowed (it just lights the screen, the way
     * a real iPod's first touch does). Playback keeps running the whole time. */
    enum { BL_OFF, BL_DIM, BL_FULL };
    int      bl_state   = BL_FULL;
    int      cpu_idled  = 0;              /* core dropped to 30 MHz for deep idle   */
    uint32_t last_input = mmio_read32(USEC_TIMER_ADDR);

    const char *last_tn = 0;              /* re-apply volume on track change       */
    int was_active = 0;                   /* detect the active->idle edge          */
    for (;;) {
        player_pump();

        /* Queue finished (last track of the album/playlist/song list ended and
         * Repeat is off) → drop back to the main menu instead of sitting on a
         * dead Now Playing screen — but ONLY if the user is actually ON that
         * screen. If they're browsing elsewhere when the queue ends, leave them
         * where they are (don't yank them out of a menu mid-navigation). */
        int now_active = player_active();
        if (was_active && !now_active && scr_cur() == SCR_NOWPLAYING) {
            g_scr_n = 1;
            g_scr[0] = SCR_MENU;
            g_main_sel = 0;
            dirty = 1;
        }
        was_active = now_active;
        if (battery_refresh(0) && scr_cur() == SCR_CHARGING) {
            dirty = 1;                    /* refresh the % on the charging screen  */
        }

        /* Charging screen: pop up on a plug-IN edge (not if already powered at
         * boot, so it won't spuriously appear), auto-dismiss when unplugged. Any
         * button also dismisses it (handled in the input switch below). */
        int ext = power_is_external() ? 1 : 0;
        if (ext && !ext_prev && scr_cur() != SCR_CHARGING) {
            scr_push(SCR_CHARGING);
            dirty = 1;
        } else if (!ext && scr_cur() == SCR_CHARGING) {
            scr_pop();
            dirty = 1;
        }
        ext_prev = ext;

        /* Long-press PLAY (~2s) sleeps the device (suspend: drive spun down,
         * screen dark, but CPU+RAM alive so wake is INSTANT and skips ipl2).
         * Holding on to ~5s escalates to a true PMU power-down. LIVE button
         * state tracks a continuous hold; a short PLAY tap is play/pause.
         * Locked out while the hold switch is on. */
        if (!g_locked && (clickwheel_buttons() & WHEEL_BTN_PLAY)) {
            uint32_t nowp = mmio_read32(USEC_TIMER_ADDR);
            if (!play_held) {
                play_held = 1;
                play_down_us = nowp;
            } else if ((uint32_t)(nowp - play_down_us) > 2000000u) {
                suspend_to_ram(play_down_us);   /* returns on wake */
                play_held  = 0;
                last_input = mmio_read32(USEC_TIMER_ADDR);
                bl_state   = BL_FULL;           /* backlight restored on resume */
                dirty      = 1;                 /* repaint the current screen */
            }
        } else {
            play_held = 0;
        }

        /* Auto-advance re-inits the codec (resetting its gain), so re-apply our
         * volume whenever the playing track changes. */
        const char *tn = player_active() ? player_track_name() : 0;
        if (tn && tn != last_tn) {
            hal_volume_set(g_volume);
        }
        last_tn = tn;

        /* Hold-switch edge (a cheap GPIO read, independent of the wheel block
         * which is gated off while held): flash the lock/unlock plate and toggle
         * the input lock. Playback is untouched. */
        int held = clickwheel_hold() ? 1 : 0;
        if (held != hold_prev) {
            hold_prev = held;
            g_locked  = held;
            g_lock_flash_until = mmio_read32(USEC_TIMER_ADDR) + 1000000u;
            last_input = mmio_read32(USEC_TIMER_ADDR);   /* wake the backlight    */
            if (bl_state != BL_FULL) {
                backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
            }
            dirty = 1;
        }

        /* Input is sampled on the 100 Hz tick and latched (clickwheel_service),
         * so a tap that lands while this loop is blocked in a disk read isn't
         * lost — we just drain the latch here. */
        wheel_event_t ev;
        if (!g_locked && clickwheel_get_event(&ev)) {
            last_input = mmio_read32(USEC_TIMER_ADDR);
            if (bl_state != BL_FULL) {
                int was_off = (bl_state == BL_OFF);
                backlight_set(g_settings.backlight_bright);
                bl_state = BL_FULL;
                dirty = 1;                    /* repaint anything drawn while off */
                if (was_off) {                /* swallow the wake press */
                    ev.buttons = 0;
                    ev.wheel_delta = 0;
                }
            }
            /* Menu click on a button press (one per down-edge; not on the
             * swallowed backlight-wake press above). Wheel clicks are emitted by
             * wheel_move itself, only when the cursor actually advances a row —
             * so a sub-threshold tick that doesn't move can't click. */
            if (ev.buttons) {
                ui_click();
            }
            /* Transport buttons are global (work from any screen while playing),
             * like a real iPod: PLAY toggles pause, RIGHT/LEFT skip track. */
            if ((ev.buttons & WHEEL_BTN_PLAY) && player_active()) {
                player_toggle_pause();
                dirty = 1;
            }
            if ((ev.buttons & WHEEL_BTN_RIGHT) && player_active()) {
                player_next();
                hal_volume_set(g_volume);         /* re-apply over codec re-init */
                dirty = 1;
            }
            if ((ev.buttons & WHEEL_BTN_LEFT) && player_active()) {
                player_prev();
                hal_volume_set(g_volume);
                dirty = 1;
            }
            switch (scr_cur()) {
            case SCR_MENU:
                if (ev.wheel_delta) {
                    g_main_sel = wheel_move(g_main_sel, main_menu_count(),
                                            ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_main_sel == MM_MUSIC) {
                        g_music_sel  = MU_ALBUMS;
                        g_menu_accum = 0;
                        scr_push(SCR_MUSIC);
                    } else if (g_main_sel == MM_NOWPLAYING && player_active()) {
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    } else if (g_main_sel == MM_SETTINGS) {
                        g_set_screen = SETTINGS_ROOT;
                        g_set_sel = g_set_root_sel = g_set_accum = 0;
                        g_set_editing = 0;
                        scr_push(SCR_SETTINGS);
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                break;

            case SCR_MUSIC:
                if (ev.wheel_delta) {
                    g_music_sel = wheel_move(g_music_sel, MU_COUNT,
                                             ev.wheel_delta, &g_menu_accum);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (g_music_sel == MU_ALBUMS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_dir_depth = 0;
                        g_artist_filter[0] = '\0';     /* all albums             */
                        albumview_build(0);
                        albumlist_queue_chips();       /* start loading covers   */
                        g_br_sel = g_br_accum = 0;
                        scr_push(SCR_BROWSER);
                    } else if (g_music_sel == MU_ARTISTS) {
                        library_ensure(fs);            /* index -> g_albums      */
                        g_artist_filter[0] = '\0';
                        build_artists();
                        g_artist_sel = g_artist_accum = 0;
                        scr_push(SCR_ARTISTS);
                    } else if (g_music_sel == MU_SONGS) {
                        library_ensure(fs);
                        songview_build(-1);            /* all songs             */
                        scr_push(SCR_SONGS);
                    } else if (g_music_sel == MU_SHUFFLE) {
                        shuffle_songs_play(fs);        /* random albums, shuffled */
                        if (player_active()) {
                            scr_push(SCR_NOWPLAYING);
                            np_first = 1;
                        }
                    } else if (g_music_sel == MU_GENRES) {
                        library_ensure(fs);
                        g_genre_sel = g_genre_accum = 0;
                        scr_push(SCR_GENRES);
                    }
                    /* other items are greyed: SELECT does nothing yet */
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to main menu */
                    g_menu_accum = 0;
                    dirty = 1;
                }
                break;

            case SCR_ARTISTS:
                if (ev.wheel_delta && g_artists_n > 0) {
                    g_artist_sel = wheel_move(g_artist_sel, g_artists_n,
                                              ev.wheel_delta, &g_artist_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_artists_n > 0) {
                    /* Filter the album list to the chosen artist's folders. */
                    int k = 0;
                    for (; g_artists[g_artist_sel].name[k] && k < NAME_MAX; k++) {
                        g_artist_filter[k] = g_artists[g_artist_sel].name[k];
                    }
                    g_artist_filter[k] = '\0';
                    g_dir_depth = 0;
                    albumview_build(g_artist_filter);
                    albumlist_queue_chips();
                    g_br_sel = g_br_accum = 0;
                    scr_push(SCR_BROWSER);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_SONGS:
                if (ev.wheel_delta && g_songview_n > 0) {
                    g_song_sel = wheel_move(g_song_sel, g_songview_n,
                                            ev.wheel_delta, &g_song_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_songview_n > 0) {
                    library_play_song(fs, g_song_sel);
                    scr_push(SCR_NOWPLAYING);
                    np_first = 1;
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back (Music or Genres) */
                    dirty = 1;
                }
                break;

            case SCR_GENRES:
                if (ev.wheel_delta && g_genres_n > 0) {
                    g_genre_sel = wheel_move(g_genre_sel, g_genres_n,
                                             ev.wheel_delta, &g_genre_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && g_genres_n > 0) {
                    songview_build(g_genre_sel);        /* this genre's songs */
                    scr_push(SCR_SONGS);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back to Music menu */
                    dirty = 1;
                }
                break;

            case SCR_BROWSER: {
                /* Depth 0 scrolls the album list (g_br_sel); depth 1 the loaded
                 * tracklist (g_det_sel) — separate so backing out restores the
                 * album-list position. */
                int count = (g_dir_depth == 0) ? g_albumview_n : g_browse_n;
                int *sel  = (g_dir_depth == 0) ? &g_br_sel   : &g_det_sel;
                int *acc  = (g_dir_depth == 0) ? &g_br_accum : &g_det_accum;
                if (ev.wheel_delta && count > 0) {
                    *sel = wheel_move(*sel, count, ev.wheel_delta, acc);
                    dirty = 1;                    /* window derived at paint time  */
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && count > 0) {
                    if (g_dir_depth == 0) {
                        /* Enter the selected album: load its tracklist + art. Keep
                         * g_br_sel (the album) so backing out lands back on it. */
                        lib_album_t *al = &g_albums[g_albumview[g_br_sel]];
                        split_artist_album(al->folder,
                                           g_album_artist, g_album_title);
                        g_dir_depth = 1;
                        browse_load(fs, al->clus);
                        detail_load_meta(fs);
                        g_det_sel = g_det_accum = 0;
                    } else {
                        player_play_queue(g_browse, g_browse_n, g_det_sel,
                                          g_art_clus, g_art_size);
                        hal_volume_set(g_volume);  /* re-apply over codec re-init */
                        scr_push(SCR_NOWPLAYING);
                        np_first = 1;
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_dir_depth > 0) {              /* tracklist -> album list */
                        g_dir_depth = 0;
                        albumlist_queue_chips();        /* g_br_sel kept (the album) */
                    } else {                            /* album list -> Music menu */
                        scr_pop();
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_NOWPLAYING:
                if (ev.wheel_delta) {                    /* wheel = volume        */
                    /* The wheel reports raw position units (~CW_WHEEL_SENSITIVITY
                     * per detent), which is why volume used to leap ~10 at a time.
                     * Quantise to whole detents: a single slow detent nudges the
                     * volume by exactly ±1, and spinning faster (more detents per
                     * event) accelerates so you can still sweep the whole range. */
                    int units = ev.wheel_delta / CW_WHEEL_SENSITIVITY;
                    if (units == 0) {                    /* sub-detent motion: ±1  */
                        units = (ev.wheel_delta > 0) ? 1 : -1;
                    }
                    int sign = (units < 0) ? -1 : 1;
                    int mag  = (units < 0) ? -units : units;
                    int step = (mag <= 1) ? sign : sign * mag * 2;  /* 1->1, 2->4, 3->6 */
                    if (step >  12) step =  12;
                    if (step < -12) step = -12;
                    int prev_vol = g_volume;
                    g_volume += step;
                    if (g_volume < 0)   g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    (void)prev_vol;   /* no click on volume — it's a slider, not nav */
                    hal_volume_set(g_volume);
                    g_settings.volume = g_volume;         /* keep Settings in sync */
                    g_vol_show_until = mmio_read32(USEC_TIMER_ADDR) + 1500000u;
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_active()) {
                    g_queue_sel = player_queue_current();  /* open the queue view */
                    g_queue_accum = 0;
                    scr_push(SCR_QUEUE);
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();                          /* back, keep playing */
                    dirty = 1;
                }
                break;

            case SCR_QUEUE:
                if (ev.wheel_delta && player_queue_len() > 0) {
                    g_queue_sel = wheel_move(g_queue_sel, player_queue_len(),
                                             ev.wheel_delta, &g_queue_accum);
                    dirty = 1;
                }
                if ((ev.buttons & WHEEL_BTN_SELECT) && player_queue_len() > 0) {
                    if (!player_queue_is_dir(g_queue_sel)) {
                        player_jump(g_queue_sel);          /* play the chosen track */
                        hal_volume_set(g_volume);          /* re-apply over re-init */
                        scr_pop();                          /* back to Now Playing  */
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    scr_pop();
                    dirty = 1;
                }
                break;

            case SCR_SETTINGS: {
                int scount = settings_count(g_set_screen);
                int slider = (settings_kind(g_set_screen, g_set_sel)
                              == SETTINGS_KIND_SLIDER);
                if (ev.wheel_delta) {
                    if (g_set_editing && slider) {         /* adjust the value  */
                        int dd = ev.wheel_delta;
                        if (dd >  4) dd =  4;
                        if (dd < -4) dd = -4;
                        settings_adjust(g_set_screen, &g_settings, g_set_sel, dd);
                        settings_apply();                  /* live volume/etc.  */
                        /* Brightness slider: light up to the new level as you
                         * turn, so the wheel drives the panel in real time. */
                        if (g_set_screen == SETTINGS_DISPLAY && g_set_sel == 1) {
                            backlight_set(g_settings.backlight_bright);
                            bl_state = BL_FULL;
                        }
                    } else if (scount > 0) {               /* move selection    */
                        g_set_sel = wheel_move(g_set_sel, scount,
                                               ev.wheel_delta, &g_set_accum);
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_SELECT) {
                    if (slider) {
                        g_set_editing = !g_set_editing;    /* enter/exit edit   */
                    } else {
                        int act = settings_activate(g_set_screen, &g_settings,
                                                    g_set_sel);
                        int target = -1;
                        switch (act) {
                        case SETTINGS_ENTER_PLAYBACK: target = SETTINGS_PLAYBACK; break;
                        case SETTINGS_ENTER_SOUND:    target = SETTINGS_SOUND;    break;
                        case SETTINGS_ENTER_DISPLAY:  target = SETTINGS_DISPLAY;  break;
                        case SETTINGS_ENTER_ABOUT:    target = SETTINGS_ABOUT;    break;
                        case SETTINGS_ENTER_THEME:    target = SETTINGS_THEME;    break;
                        case SETTINGS_ENTER_CLICKER:  target = SETTINGS_CLICKER;  break;
                        default: break;
                        }
                        if (target >= 0) {                 /* descend a screen  */
                            g_set_root_sel = g_set_sel;
                            g_set_screen = target;
                            g_set_sel = g_set_accum = 0;
                            g_set_editing = 0;
                        } else if (act == SETTINGS_ACTION_RESET) {
                            settings_defaults(&g_settings);
                            settings_apply();
                        } else {                           /* toggled/cycled    */
                            settings_apply();
                        }
                    }
                    dirty = 1;
                }
                if (ev.buttons & WHEEL_BTN_MENU) {
                    if (g_set_editing) {
                        g_set_editing = 0;                 /* exit edit, stay   */
                    } else if (g_set_screen != SETTINGS_ROOT) {
                        g_set_screen = SETTINGS_ROOT;      /* back to root      */
                        g_set_sel = g_set_root_sel;
                        g_set_accum = 0;
                    } else {
                        scr_pop();                          /* leave Settings    */
                    }
                    dirty = 1;
                }
                break;
            }

            case SCR_CHARGING:
                if (ev.buttons) {                       /* any press dismisses */
                    scr_pop();
                    dirty = 1;
                }
                break;
            }
        }

        /* Idle-timeout backlight: dim, then off — timeout from Settings (0 =
         * never), brightness from Settings. Playback keeps running. */
        uint32_t dim_us, off_us;
        if (g_settings.backlight_secs <= 0) {
            dim_us = off_us = 0xFFFFFFFFu;      /* never dim/off               */
        } else {
            dim_us = (uint32_t)g_settings.backlight_secs * 1000000u;
            off_us = dim_us + 15u * 1000000u;
        }
        int dim_level = g_settings.backlight_bright / 4;
        if (dim_level < 1) dim_level = 1;
        uint32_t idle = mmio_read32(USEC_TIMER_ADDR) - last_input;
        if (bl_state == BL_FULL && idle > dim_us) {
            backlight_set(dim_level);
            bl_state = BL_DIM;
        } else if (bl_state == BL_DIM && idle > off_us) {
            backlight_set(0);
            bl_state = BL_OFF;
        }

        /* Idle CPU-clock scaling: once the screen has timed fully off AND nothing
         * is playing, drop the core 80->30 MHz — only the disk/decode path needs
         * 80. Restore the instant there's life again: any wake back to BL_FULL, or
         * playback starting. Rides the same refcounted boost the boot path took, so
         * it stays balanced (idle unboost 1->0, wake boost 0->1) and re-boosts
         * before the render/decode work later in this same iteration. */
        if (cpu_idled && (bl_state != BL_OFF || player_active())) {
            cpu_boost();
            cpu_idled = 0;
        } else if (!cpu_idled && bl_state == BL_OFF && !player_active()) {
            cpu_unboost();
            cpu_idled = 1;
        }

        /* Lock/unlock plate takes over the screen for ~1s on a Hold edge. Paint
         * the context + plate once, hold it, then repaint underneath when it
         * fades. Suppresses the normal render while up. */
        uint32_t now_us = mmio_read32(USEC_TIMER_ADDR);
        if (now_us < g_lock_flash_until) {
            if (!lock_flashing && bl_state != BL_OFF) {
                paint_current_screen();
                lock_plate_render(g_locked);
                lcd_present_fb(console_framebuffer());
                dirty = 0;
            }
            lock_flashing = 1;
            /* Only throttle when idle; keep audio paced while playing. Halt the
             * core (self-waking ~10 ms, one tick) instead of a busy-spin. */
            if (!player_active()) {
                cpu_wait_ms(10);
            }
            continue;                     /* skip the normal render this pass      */
        }
        if (lock_flashing) {              /* plate just faded: repaint underneath  */
            lock_flashing = 0;
            dirty = 1;
        }

        /* Render the current screen (skipped entirely when the screen is off —
         * saves the IRQ-masked present while music plays dark). Now Playing
         * repaints ~1/s for the clock; menus/browser only on change. */
        if (bl_state == BL_OFF) {
            /* nothing to draw */
        } else if (scr_cur() == SCR_NOWPLAYING) {
            uint32_t nowv    = mmio_read32(USEC_TIMER_ADDR);
            uint32_t elapsed = player_elapsed_s();
            int vol_active = nowv < g_vol_show_until;
            int expiring   = np_vol_prev && !vol_active;   /* overlay just faded */

            /* Auto-advance changes the track WITHOUT a button event; the art +
             * metadata live above the partial-present band, so force one full
             * present when the queue position moves or they'd show the previous
             * song until the next tap. */
            int cur_track = player_queue_current();
            if (cur_track != np_track) {
                np_track = cur_track;
                dirty = 1;
            }

            /* A FULL present is needed only on a real change (dirty) or to erase
             * the fading volume overlay (it straddles the static-art band, so a
             * partial present can't clear it). Cap those to ~6fps while playing:
             * back-to-back full-frame, IRQ-masked pixel pushes were starving the
             * audio DMA ISR (BUF dropping into the red on a volume sweep). The
             * once-a-second clock rides the cheap partial present, unthrottled. */
            int want_full = dirty || expiring;
            if (want_full) {
                if (np_first || (uint32_t)(nowv - last_present) >= 150000u) {
                    g_mq.active = 0;
                    nowplaying_render(player_track_name(), elapsed,
                                      player_total_s(), player_buf_pct());
                    lcd_present_fb(console_framebuffer());
                    np_first = 0;
                    np_last  = elapsed;
                    np_vol_prev  = vol_active;
                    last_present = nowv;
                    player_note_presented();
                    dirty = 0;
                }
                /* else: throttled — keep dirty set, present in the next window */
            } else if (elapsed != np_last) {
                g_mq.active = 0;
                nowplaying_render(player_track_name(), elapsed,
                                  player_total_s(), player_buf_pct());
                lcd_present_rect(console_framebuffer(),
                                 0, NP_ANIM_Y, LCD_WIDTH, NP_ANIM_H);
                np_last = elapsed;
                np_vol_prev = vol_active;
                player_note_presented();
            }
        } else if (dirty) {
            /* While a song plays, cap menu/browser repaints (~6 fps) so
             * back-to-back full-frame presents during rapid scrolling don't keep
             * IRQs masked long enough to starve the audio DMA ISR. Idle → present
             * immediately for a snappy UI. `dirty` stays set until we present, so
             * the latest scroll position is what lands. */
            uint32_t now = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active() || (now - last_present) >= 150000u) {
                g_mq.active = 0;
                switch (scr_cur()) {
                case SCR_MENU:    main_menu_render();            break;
                case SCR_MUSIC:   music_menu_render();           break;
                case SCR_ARTISTS: artists_render(g_artist_sel);  break;
                case SCR_SONGS:   songs_render(g_song_sel);      break;
                case SCR_GENRES:  genres_render(g_genre_sel);    break;
                case SCR_BROWSER: browse_render(g_dir_depth ? g_det_sel : g_br_sel);       break;
                case SCR_QUEUE:   queue_render(g_queue_sel);     break;
                case SCR_SETTINGS: settings_render_cur();        break;
                case SCR_CHARGING:
                    screen_charging_render(g_bat_pct, power_is_charging(),
                                           power_is_external());
                    break;
                default: break;                 /* NOWPLAYING handled above */
                }
                lcd_present_fb(console_framebuffer());
                dirty = 0;
                last_present = now;
            }
        }

        /* Animate the now-playing 3-bar indicator in the album detail: redraw
         * only its ~10px box and partial-present just that, ~9fps. A tiny pixel
         * push (unlike a full repaint) so it can't starve the audio DMA. */
        if (bl_state != BL_OFF && scr_cur() == SCR_BROWSER && g_dir_depth > 0
            && player_active()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int sv = 0;
                for (int i = 0; i < g_det_view_n; i++)
                    if (g_det_view[i] == (int16_t)g_det_sel) { sv = i; break; }
                int top = scroll_window(sv, g_det_view_n, DET_ROWS);
                const char *pn = player_track_name();
                for (int r = 0; r < DET_ROWS; r++) {
                    int vi = top + r;
                    if (vi >= g_det_view_n) break;
                    int16_t v = g_det_view[vi];
                    if (v < 0) continue;
                    const browse_entry_t *e = &g_browse[v];
                    if (e->is_dir || !name_eq_ci(e->name, pn)) continue;
                    int ry = DET_LIST_Y0 + r * ROW_H;
                    int is_sel = (v == g_det_sel);
                    console_fill_rect(15, ry + 6, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(15, ry + 6, is_sel ? LINEN_SEL_FG : LINEN_INK, nowb);
                    lcd_present_rect(console_framebuffer(),
                                     15, ry + 6, NP_BARS_W + 1, NP_BARS_H);
                    break;
                }
                last_bars = nowb;
            }
        }

        /* Same animated bars on the queue view (the currently-playing row). */
        if (bl_state != BL_OFF && scr_cur() == SCR_QUEUE && player_active()) {
            uint32_t nowb = mmio_read32(USEC_TIMER_ADDR);
            if ((uint32_t)(nowb - last_bars) >= 110000u) {
                int n = player_queue_len();
                int cur = player_queue_current();
                int top = scroll_window(g_queue_sel, n, LIST_ROWS);
                int r = cur - top;
                if (cur < n && r >= 0 && r < LIST_ROWS) {
                    int ry = LIST_Y0 + r * ROW_H;
                    int bx = LCD_WIDTH - 22, by = ry + 7;
                    int is_sel = (cur == g_queue_sel);
                    console_fill_rect(bx, by, NP_BARS_W, NP_BARS_H,
                                      is_sel ? LINEN_SEL_BG : LINEN_SURFACE);
                    nowplaying_bars(bx, by, is_sel ? LINEN_SEL_FG : LINEN_INK, nowb);
                    lcd_present_rect(console_framebuffer(),
                                     bx, by, NP_BARS_W + 1, NP_BARS_H);
                }
                last_bars = nowb;
            }
        }

        /* Scroll the marquee target (a selected long row, or the now-playing
         * title) in place — redraw just its band + partial-present it, ~16fps. */
        if (bl_state != BL_OFF && g_mq.active) {
            uint32_t nowm = mmio_read32(USEC_TIMER_ADDR);
            /* Phase clock (g_mq.t0) + target-change detection live in mq_set, so
             * this just paces the redraw at the shared live offset. */
            if ((uint32_t)(nowm - last_mq) >= 33000u) {   /* ~30fps               */
                draw_marquee(g_mq.x, g_mq.y, g_mq.w, g_mq.text, g_mq.font,
                             g_mq.ink, g_mq.bg, nowm - g_mq.t0, g_mq.cy0, g_mq.cy1);
                int ptop = g_mq.y - text_ascent(g_mq.font);
                int pbot = g_mq.y + text_descent(g_mq.font);
                if (ptop < 0) ptop = 0;
                lcd_present_rect(console_framebuffer(),
                                 g_mq.x, ptop, g_mq.w, pbot - ptop);
                last_mq = nowm;
            }
        }

        /* Load album covers while on the album list. Idle → blast several per
         * pass (covers fill near-instantly). While a song plays we still spread
         * the I/O so it can't starve decode, but a folder.thm is only ~1KB and
         * the anti-skip diskbuf is 8MB (~73s), so a small BATCH every ~60ms
         * (~50 covers/s) is safe and stops the old one-line-at-a-time trickle. */
        if (scr_cur() == SCR_BROWSER && g_dir_depth == 0) {
            uint32_t nowc = mmio_read32(USEC_TIMER_ADDR);
            if (!player_active()) {
                for (int k = 0; k < 6 && artcache_pump(fs); k++) {
                    dirty = 1;                /* several covers per pass when idle */
                }
                last_chip = nowc;
            } else if ((uint32_t)(nowc - last_chip) >= 60000u) {
                for (int k = 0; k < 3 && artcache_pump(fs); k++) {
                    dirty = 1;                /* small batch/tick while playing    */
                }
                last_chip = nowc;
            }
        }

        /* Only throttle when idle; while playing, player_pump's decode_step
         * paces the loop and the wheel stays responsive. Halt the core until the
         * next tick (self-waking ~10 ms) instead of a busy-spin: input is latched
         * by the 100 Hz timer ISR, so this costs no responsiveness. */
        if (!player_active()) {
            cpu_wait_ms(10);
        }
    }
}

/* Per-task stack for the idle task (carved from .bss). Only reached if the LCD
 * is unpowered or the disk won't mount — the browser above never returns. */
#define TASK_STACK_SIZE 1024
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs). */
static uint16_t mbr_sector[1024];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then yield.
 * The scheduler is only started on the no-LCD / mount-failure fallback path.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

_Noreturn void kernel_main(void) {
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"), sampled at the very top so later milestones can report
     * elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");

    /* Hex-path self-test: if this doesn't read 1234ABCD on the terminal,
     * distrust every register dump that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(PROCESSOR_ID);
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz) before the
     * rest of bring-up so it runs at a sane speed. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the DMA
     * reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

    /* 100 Hz system tick + core IRQ unmask BEFORE audio: continuous playback is
     * DMA-driven and needs its completion interrupt delivered, and sleep_ms
     * spins on the IRQ-fed tick. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (pre-sleep)\n");
    sleep_ms(100);
    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (post-sleep, ~+10)\n");

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    if (lcd_init()) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Backlight to full (GPIO charge-pump dimmer) so the splash + UI are lit;
         * run_ui dims then turns it off after inactivity. */
        backlight_init();

        /* Bring up the I2C control bus now (not just at first-song hal_audio_init)
         * so the status strip can read the PCF50605 battery gauge from the menu.
         * Bounded/idempotent — hal_audio_init re-inits it harmlessly per track. */
        i2c_init();
        battery_init();
        hal_volume_init();               /* codec output gain -> safe default      */
        piezo_init();                    /* PWM click for menu navigation          */

        /* Paint the boot splash immediately, so the panel shows CORE branding
         * instead of the chainloader's leftover framebuffer (a blue field with
         * a green stripe) while the disk spins up and the volume mounts. */
        boot_splash();

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
        if (ata_init() == 0 && ata_read_sectors(0, 1, mbr) == 0) {
            sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &mbr[0x1BE + 16 * p];
                if (e[4] == 0x0B || e[4] == 0x0C) {
                    fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                              ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                    break;
                }
            }
        }
        if (sig == 0xAA55u && fat_lba != 0) {
            /* The MBR start-LBA may be in native 512-byte units OR (stock 80 GB,
             * 2048-byte FAT sectors) in 2048-byte units. Try as-is, then x4;
             * fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, player_disk_read, 0, fat_lba * 4u);
            }
        }

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
            /* Capacity / free (MB) for the About screen (64-bit to avoid the
             * clusters*bytes overflow on a big volume). */
            g_total_mb = (uint32_t)(((uint64_t)fs.total_clus * fs.clus_bytes) >> 20);
            g_free_mb  = (fs.free_clus == 0xFFFFFFFFu) ? 0xFFFFFFFFu
                       : (uint32_t)(((uint64_t)fs.free_clus * fs.clus_bytes) >> 20);
            uart_puts("core: entering browser\n");
            run_ui(&fs);                       /* never returns */
        }

        /* Mount failed: show a diagnostic error screen and fall through to the
         * idle scheduler so the device isn't a black brick. */
        console_clear(CON_RED);
        console_str  (2, 3, "MOUNT FAILED", CON_WHITE, CON_RED);
        console_str  (2, 6, "SIG",  CON_WHITE, CON_RED);
        console_hex32(8, 6, sig,               CON_WHITE, CON_RED);
        console_str  (2, 8, "MNT",  CON_WHITE, CON_RED);
        console_hex32(8, 8, (uint32_t)mnt,     CON_WHITE, CON_RED);
        console_str  (2, 10, "PART", CON_WHITE, CON_RED);
        console_hex32(8, 10, fat_lba,          CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        cpu_unboost();
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping disk + UI\n");
    }

    /* Fallback only (no LCD or mount failure): idle the core under the
     * cooperative scheduler. sched_start never returns. */
    uart_puts("core: sched init (idle fallback)\n");
    sched_init();
    if (sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
