/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/screen_settings.c — Settings screens rendered in the Linen system.
 *
 * Draws the data-driven model in settings.c (settings_count / _label / _kind /
 * _value) into the 320x240 RGB565 console framebuffer, matching
 * design_reference/menus.jsx (SettingsMenu / SettingsPlayback / SettingsSound /
 * SettingsAbout) and system-screens.jsx (ThemePicker). Uses the same list
 * chrome, palette, fonts and geometry as kernel/main.c (LIST_Y0 42, ROW_H 24,
 * dark-ink selection bar with light text), replicated locally as tiny helpers
 * so this stays a self-contained module (main.c is not edited).
 *
 * The jsx Settings screens carry no status strip (unlike the browser), so this
 * leaves the top band clear and places the header at main.c's header baseline —
 * the header stays put when you cross from the main menu into Settings.
 *
 * Pure rendering: integer-only, no libc/libm/malloc, no hardware access, no
 * present. The caller hands the framebuffer to lcd_present_fb().
 */

#include "settings.h"
#include "palette.h"                          /* live theme palette (g_pal[]) */

#include "text.h"
#include "../kernel/console.h"
#include "../hal/hal.h"                        /* LCD_WIDTH / LCD_HEIGHT */

/* ---------------------------------------------------------------------------
 * Palette tokens — now resolve to the live, theme-selected g_pal[] (shared with
 * kernel/main.c via ui/palette.h) so the Settings screens follow Linen / Onyx
 * like the rest of the UI. SEL_BG/SEL_FG stay derived from INK/SURFACE.
 * ------------------------------------------------------------------------- */
#define S_SURFACE  g_pal[PAL_SURFACE]
#define S_INK      g_pal[PAL_INK]
#define S_MUTED    g_pal[PAL_MUTED]
#define S_MUTED2   g_pal[PAL_MUTED2]
#define S_MUTED_D  g_pal[PAL_MUTED_D]
#define S_ACCENT   g_pal[PAL_ACCENT]
#define S_BORDER   g_pal[PAL_BORDER]
#define S_SEL_BG   g_pal[PAL_INK]      /* selection bar = ink (inverts w/ theme) */
#define S_SEL_FG   g_pal[PAL_SURFACE]  /* selection text = surface               */
#define S_SEL_SUB  g_pal[PAL_SEL_SUB]
#define S_CHEVRON  g_pal[PAL_CHEVRON]
#define S_TRK      g_pal[PAL_TRK]      /* slider track                           */
#define S_SB_TRK   g_pal[PAL_SB_TRK]   /* scrollbar track                        */
#define S_SB_THMB  g_pal[PAL_SB_THMB]  /* scrollbar thumb                        */
/* Warning red. Same literal as kernel/main.c's BATT_LOW_RED — deliberately a
 * fixed colour and not a palette slot: it must stay legible as a WARNING in
 * every theme, which is exactly what a themed palette entry would not. */
#define S_WARN     0xE125u
#define S_SEL_TRK  g_pal[PAL_SEL_TRK]  /* slider track on a selected row         */
#define S_PILL_OFF g_pal[PAL_PILL_OFF] /* toggle pill OFF fill                   */

/* Nunito faces (see ui/text.h). */
#define F_BIG    text_font_bold_17()
#define F_HEADER text_font_bold_13()
#define F_ROW    text_font_regular_13()
#define F_SUB    text_font_regular_11()
#define F_SMALL  text_font_regular_9()

/* Geometry — identical to kernel/main.c so screens line up across the UI. */
#define HDR_BASE   30
#define HDR_DIV_Y  38
#define LIST_Y0    42
#define ROW_H      24
#define TH_ROW_H   40                          /* taller theme-picker rows    */

/* ---------------------------------------------------------------------------
 * Tiny draw helpers (matched copies of main.c's, kept local on purpose)
 * ------------------------------------------------------------------------- */

/* Draw a NUL-terminated string at baseline `y`; returns the advance. */
static int st_text(int x, int y, const char *s, const text_font_t *font,
                   uint16_t ink)
{
    return text_draw(console_fb(), LCD_WIDTH, LCD_HEIGHT, x, y, s, font, ink);
}

/* Right-align `s` so it ends at x = LCD_WIDTH - `pad`. */
static void st_text_right(int pad, int y, const char *s,
                          const text_font_t *font, uint16_t ink)
{
    int w = text_width(s, font);
    st_text(LCD_WIDTH - pad - w, y, s, font, ink);
}

/* Titled header with an optional back chevron and right-aligned value, plus the
 * divider (menus.jsx ScreenHeader). */
static void header_render(const char *title, const char *right, int back)
{
    int x = 12;
    if (back) {
        x = st_text(x, HDR_BASE, UI_GLYPH_LAQUO, F_HEADER, S_MUTED2) + 4;
    }
    st_text(x, HDR_BASE, title, F_HEADER, S_INK);
    if (right && right[0]) {
        st_text_right(12, HDR_BASE - 1, right, F_SMALL, S_MUTED2);
    }
    console_fill_rect(12, HDR_DIV_Y, LCD_WIDTH - 24, 1, S_BORDER);
}

/* Filled rounded rect (radius rr) — the design rounds selection bars (4) and
 * pills (end-caps). Each corner row is inset along a quarter-circle. */
static int st_isqrt(int v) { int q = 0; while ((q + 1) * (q + 1) <= v) q++; return q; }
static void st_round_rect(int x, int y, int w, int h, int rr, uint16_t c)
{
    if (rr < 1) { console_fill_rect(x, y, w, h, c); return; }
    if (2 * rr > w) rr = w / 2;
    if (2 * rr > h) rr = h / 2;
    for (int yy = 0; yy < h; yy++) {
        int inset = 0, k = -1;
        if (yy < rr)           k = yy;
        else if (yy >= h - rr) k = h - 1 - yy;
        if (k >= 0) { int dy = rr - k; inset = rr - st_isqrt(rr * rr - dy * dy); }
        console_fill_rect(x + inset, y + yy, w - 2 * inset, 1, c);
    }
}

/* The dark-ink selection bar behind row `r` (menus.jsx selected Row, radius 4). */
static void sel_bar(int y0, int rowh, int r)
{
    int ry = y0 + r * rowh;
    st_round_rect(6, ry + 1, LCD_WIDTH - 16, rowh - 2, 4, S_SEL_BG);
}

/* Toggle pill (menus.jsx SettingsPlayback toggle): a 22x12 track with a 9x9
 * knob that sits right when on, left when off. Colours flip on a selected row. */
static void draw_toggle(int ry, int selected, int on)
{
    const int pw = 22, ph = 12;
    int px = LCD_WIDTH - 16 - pw;
    int py = ry + (ROW_H - ph) / 2;

    uint16_t track = on ? (selected ? S_SEL_FG : S_INK)
                        : (selected ? S_SEL_SUB : S_PILL_OFF);
    uint16_t knob  = on ? (selected ? S_INK : S_SURFACE)
                        : (selected ? S_SURFACE : S_SURFACE);
    st_round_rect(px, py, pw, ph, ph / 2, track);      /* rounded pill end-caps */
    int kx = on ? px + pw - 2 - 9 : px + 2;
    st_round_rect(kx, py + 2, 9, 9, 4, knob);          /* round knob            */
}

/* Slider fill bar for a value fraction num/den (menus.jsx SettingsSound bar):
 * a full-width track with a proportional fill, colours flipping when selected. */
static void draw_slider(int ry, int selected, int num, int den)
{
    int bx = 14, bw = LCD_WIDTH - 16 - bx, by = ry + 17, bh = 3;
    uint16_t trackc = selected ? S_SEL_TRK : S_TRK;
    uint16_t fillc  = selected ? S_SEL_FG  : S_INK;
    console_fill_rect(bx, by, bw, bh, trackc);
    if (den > 0) {
        int fw = bw * num / den;
        if (fw < 0) fw = 0;
        if (fw > bw) fw = bw;
        console_fill_rect(bx, by, fw, bh, fillc);
    }
}

/* ---------------------------------------------------------------------------
 * Generic list screens (Root / Playback / Sound / Display)
 * ------------------------------------------------------------------------- */
/*
 * Windowed-list scroll origin and scrollbar. Deliberately the same geometry
 * and the same palette entries as the browser's in kernel/main.c (which owns
 * the originals and cannot be called from here — this module is freestanding
 * and has no dependency on main.c). Keep the two in step: a scrollbar that
 * sits 1px off, or a window that keeps the selection somewhere else, reads as
 * a different list widget on the same device.
 */
#define LIST_ROWS  8                   /* visible rows: (240-42)/24 ~= 8 */

static int st_scroll_window(int sel, int total, int visible)
{
    if (total <= visible) return 0;
    int start = sel - visible / 3;     /* selection sits ~1/3 down */
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible;
    return start;
}

static void st_scrollbar(int y0, int top, int visible, int total)
{
    if (total <= visible) {
        return;                        /* everything fits: no bar at all */
    }
    int track_y = y0;
    int track_h = LCD_HEIGHT - y0 - 4;
    console_fill_rect(LCD_WIDTH - 4, track_y, 3, track_h, S_SB_TRK);
    int thumb_h = (visible * track_h) / total;
    if (thumb_h < 16) thumb_h = 16;
    int denom = total - visible;
    if (denom < 1) denom = 1;
    int thumb_y = track_y + (top * (track_h - thumb_h)) / denom;
    console_fill_rect(LCD_WIDTH - 4, thumb_y, 3, thumb_h, S_SB_THMB);
}

static void list_render(int screen, const settings_t *s, int sel)
{
    int n   = settings_count(screen);
    /* Sliders are double-height, so a slider screen shows fewer rows. Only
     * Sound is all-sliders and it has 4 rows, which fits either way; the
     * window maths below is row-count based and stays correct regardless. */
    int top = st_scroll_window(sel, n, LIST_ROWS);
    int end = top + LIST_ROWS;
    if (end > n) end = n;
    st_scrollbar(LIST_Y0, top, LIST_ROWS, n);
    for (int r = top; r < end; r++) {
        int vr = r - top;                      /* visible row index */
        int ry = LIST_Y0 + vr * ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, ROW_H, vr);
        }

        int kind = settings_kind(screen, r);
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;
        settings_value(screen, s, r, buf, &is_toggle, &on, &num, &den);

        uint16_t fg     = is_sel ? S_SEL_FG  : S_INK;
        uint16_t rightc = is_sel ? S_SEL_SUB : S_MUTED_D;
        uint16_t chevc  = is_sel ? S_SEL_SUB : S_CHEVRON;

        if (kind == SETTINGS_KIND_SLIDER) {
            /* label + right value lifted to make room for the bar below. */
            st_text(14, ry + 11, settings_label(screen, r),
                    is_sel ? F_HEADER : F_ROW, fg);
            if (buf[0]) {
                st_text_right(16, ry + 11, buf, F_SUB, rightc);
            }
            draw_slider(ry, is_sel, num, den);
            continue;
        }

        /* Non-slider rows: label centred in the row height. */
        st_text(14, ry + 15, settings_label(screen, r),
                is_sel ? F_HEADER : F_ROW, fg);

        if (is_toggle) {
            draw_toggle(ry, is_sel, on);
        } else if (buf[0]) {
            st_text_right(16, ry + 15, buf, F_SUB, rightc);
        } else {
            /* submenu / action with no value -> chevron. */
            st_text(LCD_WIDTH - 18, ry + 15, UI_GLYPH_RAQUO, F_ROW, chevc);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Theme picker (system-screens.jsx ThemePicker): swatch + name + sub, with a
 * "CURRENT" tag on the active theme.
 * ------------------------------------------------------------------------- */
/* Preview tones are each theme's OWN colours (fixed literals, NOT the live
 * palette) so a row previews the theme it would switch to. Order matches the
 * theme ids: 0 = Linen (light), 1 = Onyx (dark). */
static const uint16_t TH_SWATCH[2] = { 0xF79Du, 0x18C2u };  /* surface tone   */
static const uint16_t TH_INK[2]    = { 0x18A2u, 0xEF3Cu };  /* ink hint bar   */
static const char *const TH_SUB[2] = {
    "Warm light - text-forward",
    "Warm dark - terracotta",
};

static void theme_render(const settings_t *s, int sel)
{
    int n = settings_count(SETTINGS_THEME);
    for (int r = 0; r < n; r++) {
        int ry = LIST_Y0 + r * TH_ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, TH_ROW_H, r);
        }

        /* Swatch tile with a border + a small "text" hint bar. */
        int sw = 26, sx = 14, sy = ry + (TH_ROW_H - sw) / 2;
        console_fill_rect(sx - 1, sy - 1, sw + 2, sw + 2,
                          is_sel ? S_SEL_SUB : S_BORDER);
        console_fill_rect(sx, sy, sw, sw, TH_SWATCH[r]);
        console_fill_rect(sx + 6, sy + 10, 14, 3, TH_INK[r]);

        int tx = sx + sw + 10;
        uint16_t fg   = is_sel ? S_SEL_FG  : S_INK;
        uint16_t subc = is_sel ? S_SEL_SUB : S_MUTED;
        st_text(tx, ry + 17, settings_theme_name(r), F_HEADER, fg);
        st_text(tx, ry + 31, TH_SUB[r], F_SMALL, subc);

        if (r == s->theme) {
            st_text_right(14, ry + 20, "CURRENT", F_SMALL,
                          is_sel ? S_SEL_FG : S_MUTED2);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public renderers
 * ------------------------------------------------------------------------- */
void settings_render(int screen, const settings_t *s, int sel)
{
    if (screen == SETTINGS_ABOUT) {
        /* main.c should call settings_about_render() with live values; this
         * placeholder path keeps settings_render total over every screen. */
        settings_about_render(-1, -1, 0, 0xFFFFFFFFu, 0, 0, 0);
        return;
    }

    console_clear(S_SURFACE);

    const char *title;
    const char *right = "";
    switch (screen) {
    case SETTINGS_PLAYBACK: title = "Playback"; break;
    case SETTINGS_SOUND:    title = "Sound";    break;
    case SETTINGS_DISPLAY:  title = "Display";  break;
    case SETTINGS_THEME:    title = "Theme"; right = "2 themes"; break;
    case SETTINGS_CLICKER:  title = "Clicker"; break;
    default:                title = "Settings"; break;
    }
    header_render(title, right, 1);

    if (screen == SETTINGS_THEME) {
        theme_render(s, sel);
    } else {
        list_render(screen, s, sel);
    }
}

/* ---------------------------------------------------------------------------
 * About (SettingsAbout): device info key/value rows from live values.
 * ------------------------------------------------------------------------- */

/* Local string copy + decimal writer (a separate TU from settings.c). */
static void su_copy(char *d, const char *s)
{
    while (*s) *d++ = *s++;
    *d = '\0';
}
static int su_to_str(char *d, unsigned v)
{
    char tmp[10];
    int t = 0;
    do { tmp[t++] = (char)('0' + v % 10u); v /= 10u; } while (v && t < 10);
    int i = 0;
    while (t > 0) d[i++] = tmp[--t];
    d[i] = '\0';
    return i;
}

/* Format whole megabytes as "W.F GB" (one decimal). */
static void fmt_gb(char *d, uint32_t mb)
{
    uint32_t whole = mb / 1024u;
    uint32_t frac  = (mb % 1024u) * 10u / 1024u;
    int i = su_to_str(d, whole);
    d[i++] = '.';
    d[i++] = (char)('0' + frac);
    d[i++] = ' ';
    d[i++] = 'G';
    d[i++] = 'B';
    d[i]   = '\0';
}

/* Append " word" to the NUL-terminated string in `d`. */
static void su_append(char *d, const char *w)
{
    int i = 0; while (d[i]) i++;
    su_copy(d + i, w);
}

/* About — a little device dashboard: three big library stats, a storage bar
 * (used vs free), and a device footer, instead of a plain key/value list. */
void settings_about_render(int battery_pct, int battery_mv,
                           uint32_t total_mb, uint32_t free_mb,
                           int n_songs, int n_albums, int n_artists)
{
    (void)battery_mv;
    console_clear(S_SURFACE);
    header_render("About", "", 1);

    char v[48];

    /* --- device name hero, centred at the top --- */
    st_text((LCD_WIDTH - text_width("iPod 5.5G", F_BIG)) / 2, 62,
            "iPod 5.5G", F_BIG, S_INK);

    /* --- three big stat columns: Songs / Albums / Artists --- */
    const char *lbl[3] = { "SONGS", "ALBUMS", "ARTISTS" };
    int         val[3] = { n_songs, n_albums, n_artists };
    int colw = LCD_WIDTH / 3;
    for (int i = 0; i < 3; i++) {
        int cx = colw * i + colw / 2;
        su_to_str(v, (unsigned)(val[i] < 0 ? 0 : val[i]));
        st_text(cx - text_width(v, F_BIG) / 2, 100, v, F_BIG, S_INK);
        st_text(cx - text_width(lbl[i], F_SMALL) / 2, 116, lbl[i], F_SMALL, S_MUTED);
        if (i) console_fill_rect(colw * i, 86, 1, 36, S_BORDER);   /* column rule */
    }
    console_fill_rect(16, 130, LCD_WIDTH - 32, 1, S_BORDER);

    /* --- firmware row: label left, "Core" chip on the right --- */
    st_text(16, 150, "FIRMWARE", F_SMALL, S_MUTED);
    {
        int cw = text_width("Core", F_SUB);
        int chw = cw + 16, chx = LCD_WIDTH - 16 - chw, chy = 140;
        st_round_rect(chx, chy, chw, 15, 7, S_ACCENT);
        st_text(chx + 8, 151, "Core", F_SUB, S_SURFACE);
    }

    /* --- storage bar (used = accent fill on a faint track) --- */
    st_text(16, 176, "STORAGE", F_SMALL, S_MUTED);
    if (free_mb != 0xFFFFFFFFu) {
        fmt_gb(v, free_mb); su_append(v, " free");
        st_text_right(16, 176, v, F_SUB, S_MUTED_D);
    }
    int bx = 16, by = 182, bw = LCD_WIDTH - 32, bh = 8;
    st_round_rect(bx, by, bw, bh, 4, S_TRK);
    if (total_mb > 0 && free_mb != 0xFFFFFFFFu) {
        uint32_t used = (total_mb > free_mb) ? total_mb - free_mb : 0;
        int fw = (int)(((unsigned long long)used * bw) / total_mb);
        if (fw < bh && used > 0) fw = bh;
        if (fw > bw) fw = bw;
        st_round_rect(bx, by, fw, bh, 4, S_ACCENT);
    }

    /* --- battery: a little battery pictogram with proportional fill --- */
    st_text(16, 212, "BATTERY", F_SMALL, S_MUTED);
    if (battery_pct >= 0) {
        su_to_str(v, (unsigned)battery_pct); su_append(v, "%");
        st_text_right(16, 212, v, F_SUB, S_INK);
    }
    {
        int gx = 16, gy = 218, gw = LCD_WIDTH - 32 - 5, gh = 12;   /* body */
        st_round_rect(gx, gy, gw, gh, 3, S_TRK);                   /* shell */
        console_fill_rect(gx + gw, gy + 3, 4, gh - 6, S_TRK);      /* + nub */
        if (battery_pct >= 0) {
            int pct = battery_pct > 100 ? 100 : battery_pct;
            int fw  = (gw - 4) * pct / 100;
            if (fw < 2 && pct > 0) fw = 2;
            st_round_rect(gx + 2, gy + 2, fw, gh - 4, 2, S_ACCENT);
        }
    }
}


/* ---------------------------------------------------------------------------
 * Boot Details (SETTINGS_DIAG)
 * ------------------------------------------------------------------------- */

/*
 * "This phase did not run", distinct from "this phase ran and took no
 * measurable time". The first version conflated them — both rendered "--" —
 * which made the screen actively misleading the moment it mattered: after the
 * FLAC seek fix, a seek that had become instant was indistinguishable from a
 * seek that had been skipped.
 */
#define MS_NA  0xFFFFFFFFu

/*
 * Sub-second times are the interesting ones now, so render them in ms rather
 * than rounding a 40 ms seek to "0.0s". Over a second, one decimal is plenty.
 */
static void fmt_ms(char *d, uint32_t ms)
{
    if (ms == MS_NA) { su_copy(d, "--"); return; }
    if (ms < 1000u) {
        int i = su_to_str(d, ms);
        su_copy(d + i, "ms");
        return;
    }
    int i = su_to_str(d, ms / 1000u);
    d[i++] = '.';
    d[i++] = (char)('0' + (ms / 100u) % 10u);
    d[i++] = 's';
    d[i]   = '\0';
}

/* Draw `s` so it ends at x = right. */
static void st_text_at_right(int right, int y, const char *s,
                             const text_font_t *font, uint16_t ink)
{
    st_text(right - text_width(s, font), y, s, font, ink);
}

/*
 * Phase colours. Fixed literals rather than palette slots, on purpose: this is
 * a diagnostic chart, and its segments have to stay distinguishable from each
 * other in any theme — which is exactly what themed tokens would not
 * guarantee. Ordered to match the legend.
 */
#define C_LCD   0x3BDBu    /* blue      */
#define C_DISK  0x3DADu    /* green     */
#define C_LIB   0xE546u    /* amber     */
#define C_RES   0xE125u    /* red       */
#define C_OTHER 0x94B2u    /* grey      */

void settings_diag_render(uint32_t total_ms, uint32_t lcd_ms, uint32_t disk_ms,
                          uint32_t lib_ms, uint32_t resume_ms,
                          uint32_t res_dir_ms, uint32_t res_open_ms,
                          uint32_t res_seek_ms,
                          uint32_t decode_us_kframe, uint32_t underruns,
                          int cfg_writable, uint32_t cfg_seq,
                          uint32_t lba0, uint32_t lba1)
{
    char v[48];
    console_clear(S_SURFACE);
    header_render("Boot Details", "", 1);

    /* --- headline: label left, total right, on one line --- */
    st_text(16, 60, "COLD BOOT", F_SMALL, S_MUTED);
    fmt_ms(v, total_ms);
    st_text_right(16, 63, v, F_BIG, S_INK);

    /*
     * --- stacked proportional bar ---
     * Where the time went, at a glance. Widths come from the SAME numbers as
     * the legend below, so the picture cannot disagree with the figures. The
     * remainder segment is drawn last and simply takes what is left, which is
     * why unattributed time shows up as grey rather than silently rescaling
     * the others.
     */
    {
        int bx = 16, by = 74, bw = LCD_WIDTH - 32, bh = 10;
        st_round_rect(bx, by, bw, bh, 5, S_TRK);
        if (total_ms > 0) {
            uint32_t seg[4] = { lcd_ms, disk_ms, lib_ms, resume_ms };
            uint16_t col[4] = { C_LCD, C_DISK, C_LIB, C_RES };
            int x = bx;
            for (int i = 0; i < 4; i++) {
                int w = (int)(((uint64_t)seg[i] * (uint32_t)bw) / total_ms);
                if (w <= 0) continue;                  /* too small to see */
                if (x + w > bx + bw) w = bx + bw - x;
                console_fill_rect(x, by, w, bh, col[i]);
                x += w;
            }
            if (x < bx + bw) {
                console_fill_rect(x, by, bx + bw - x, bh, C_OTHER);
            }
        }
    }

    /*
     * --- legend, two columns ---
     * Six entries in two columns of three rather than six stacked rows: the
     * panel is 240 px tall and the single-column version ran into the config
     * block at the bottom.
     */
    {
        static const char *const NM[6] = { "LCD", "DISK", "LIBRARY",
                                           "RESUME", "OTHER", "DECODE" };
        uint16_t cl[6] = { C_LCD, C_DISK, C_LIB, C_RES, C_OTHER, 0 };
        uint32_t known = lcd_ms + disk_ms + lib_ms + resume_ms;
        uint32_t val[5] = { lcd_ms, disk_ms, lib_ms, resume_ms,
                            (total_ms > known) ? total_ms - known : 0 };
        int colx[2] = { 16, 168 };
        int colw    = 136;
        for (int i = 0; i < 6; i++) {
            int cx = colx[i / 3];
            int y  = 104 + (i % 3) * 18;
            console_fill_rect(cx, y - 7, 6, 6, cl[i] ? cl[i] : S_MUTED2);
            st_text(cx + 11, y, NM[i], F_SMALL, S_MUTED);
            if (i < 5) {
                fmt_ms(v, val[i]);
                st_text_at_right(cx + colw, y, v, F_SUB, S_INK);
            } else {
                /*
                 * Decode headroom against the 22676 us/kframe that 44.1 kHz
                 * real time allows — the number that says whether re-enabling
                 * FLAC CRC (which is what buys O(log n) seeks) cost us
                 * margin. Red inside 20% of the budget: past that a slow disk
                 * refill becomes an audible underrun instead of a near miss.
                 */
                if (decode_us_kframe == 0) {
                    st_text_at_right(cx + colw, y, "--", F_SUB, S_MUTED_D);
                } else {
                    int pct = (int)((decode_us_kframe * 100u) / 22676u);
                    su_to_str(v, (unsigned)pct);
                    su_append(v, "%");
                    st_text_at_right(cx + colw, y, v, F_SUB,
                                     (pct >= 80) ? S_WARN : S_INK);
                }
            }
        }
    }

    console_fill_rect(16, 152, LCD_WIDTH - 32, 1, S_BORDER);

    /* --- the resume split, as one line: it is a breakdown OF the RESUME
     * entry above, not three more phases, so it is indented and dimmer. --- */
    {
        static const char *const SUB[3] = { "dir", "open", "seek" };
        uint32_t sv[3] = { res_dir_ms, res_open_ms, res_seek_ms };
        int x = 16;
        st_text(x, 168, "RESUME", F_SMALL, S_MUTED2);
        x += text_width("RESUME", F_SMALL) + 10;
        for (int i = 0; i < 3; i++) {
            st_text(x, 168, SUB[i], F_SMALL, S_MUTED2);
            x += text_width(SUB[i], F_SMALL) + 4;
            fmt_ms(v, sv[i]);
            st_text(x, 168, v, F_SMALL, S_INK);
            x += text_width(v, F_SMALL) + 12;
        }
    }

    if (underruns) {
        su_to_str(v, underruns);
        su_append(v, " audio underrun");
        st_text(16, 186, v, F_SMALL, S_WARN);
    }

    console_fill_rect(16, 196, LCD_WIDTH - 32, 1, S_BORDER);

    /*
     * --- settings-file locator ---
     * The two absolute LBAs config_save() writes to. This is the ONLY way to
     * run the mandatory pre-write safety check on this device:
     * tools/make_config.py says to boot, read the LBA the firmware reports,
     * and confirm it matches the host's independently computed address BEFORE
     * anything is written — "a mismatch there is the bug that overwrites
     * somebody's music library". That procedure assumes a serial cable, and
     * there is none here, so the number has to reach the panel. Read-only.
     */
    st_text(16, 214, "CONFIG", F_SMALL, S_MUTED);
    if (!cfg_writable) {
        st_text_right(16, 214, "not writable", F_SUB, S_MUTED_D);
    } else {
        su_copy(v, "seq ");
        su_to_str(v + 4, cfg_seq);
        st_text_right(16, 214, v, F_SUB, S_INK);
        int i = su_to_str(v, lba0);
        su_copy(v + i, " / ");
        su_to_str(v + i + 3, lba1);
        st_text_right(16, 230, v, F_SMALL, S_MUTED_D);
    }
}
