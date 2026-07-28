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
static void list_render(int screen, const settings_t *s, int sel)
{
    int n = settings_count(screen);
    for (int r = 0; r < n; r++) {
        int ry = LIST_Y0 + r * ROW_H;
        int is_sel = (r == sel);
        if (is_sel) {
            sel_bar(LIST_Y0, ROW_H, r);
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

/* Format milliseconds as "W.Fs" (one decimal), or "--" for an unmeasured 0. */
static void fmt_ms(char *d, uint32_t ms)
{
    if (ms == 0) { su_copy(d, "--"); return; }
    int i = su_to_str(d, ms / 1000u);
    d[i++] = '.';
    d[i++] = (char)('0' + (ms / 100u) % 10u);
    d[i++] = 's';
    d[i]   = '\0';
}

/*
 * Boot Details — the cold-boot phase breakdown plus the settings-file locator.
 *
 * Split off About deliberately. About is the "what is this device" dashboard;
 * these are diagnostics, and the CFG rows had already been squeezed into a gap
 * there (an earlier attempt at y=92/102 landed on top of the stat columns).
 *
 * The phases are shown as measured, and TOTAL is measured independently rather
 * than summed — so a phase nobody instrumented shows up as the difference
 * between TOTAL and the rows above it, instead of quietly disappearing.
 */
void settings_diag_render(uint32_t total_ms, uint32_t lcd_ms, uint32_t disk_ms,
                          uint32_t lib_ms, uint32_t resume_ms,
                          uint32_t res_dir_ms, uint32_t res_open_ms,
                          uint32_t res_seek_ms,
                          int cfg_writable, uint32_t cfg_seq,
                          uint32_t lba0, uint32_t lba1)
{
    char v[48];
    console_clear(S_SURFACE);
    header_render("Boot Details", "", 1);

    /* --- total, as the hero number --- */
    fmt_ms(v, total_ms);
    st_text((LCD_WIDTH - text_width(v, F_BIG)) / 2, 66, v, F_BIG, S_INK);
    st_text((LCD_WIDTH - text_width("COLD BOOT", F_SMALL)) / 2, 82,
            "COLD BOOT", F_SMALL, S_MUTED);
    console_fill_rect(16, 94, LCD_WIDTH - 32, 1, S_BORDER);

    /* --- per-phase rows --- */
    static const char *const PH[4] = { "LCD / BCM", "DISK + MOUNT",
                                       "LIBRARY", "RESUME" };
    uint32_t phv[4] = { lcd_ms, disk_ms, lib_ms, resume_ms };
    uint32_t known  = 0;
    for (int i = 0; i < 4; i++) {
        int y = 112 + i * 18;
        st_text(16, y, PH[i], F_SMALL, S_MUTED);
        fmt_ms(v, phv[i]);
        st_text_right(16, y, v, F_SUB, S_INK);
        known += phv[i];
    }

    /* Resume dominates a warm boot, so it gets its own split: directory read,
     * open (incl. album art + decoder prime), and the seek. Indented and
     * dimmer — these are a breakdown OF the RESUME row above, not additions
     * to it, and must not read as separate phases. */
    {
        int y = 112 + 4 * 18;
        static const char *const SUB[3] = { "dir", "open", "seek" };
        uint32_t subv[3] = { res_dir_ms, res_open_ms, res_seek_ms };
        int      x = 28;
        for (int i = 0; i < 3; i++) {
            char b[24];
            su_copy(b, SUB[i]);
            st_text(x, y, b, F_SMALL, S_MUTED2);
            x += text_width(b, F_SMALL) + 4;
            fmt_ms(b, subv[i]);
            st_text(x, y, b, F_SMALL, S_MUTED_D);
            x += text_width(b, F_SMALL) + 10;
        }
    }

    /* "OTHER" is the honest remainder: clock/cache/timer bring-up, i2c, the
     * splash, and anything we forgot to time. If this row is large, that is
     * where the next optimisation lives. */
    {
        int y = 112 + 5 * 18;
        uint32_t other = (total_ms > known) ? total_ms - known : 0;
        st_text(16, y, "OTHER", F_SMALL, S_MUTED2);
        fmt_ms(v, other);
        st_text_right(16, y, v, F_SUB, S_MUTED_D);
    }

    console_fill_rect(16, 196, LCD_WIDTH - 32, 1, S_BORDER);

    /*
     * Settings-file state and the two absolute LBAs config_save() writes to.
     * This is the ONLY way to run the mandatory pre-write safety check on this
     * device: tools/make_config.py says to boot, read the LBA the firmware
     * reports, and confirm it matches the host's independently computed address
     * BEFORE anything is written — "a mismatch there is the bug that overwrites
     * somebody's music library". That procedure assumes a serial cable, and
     * there is none here, so the number has to reach the user's eyes on the
     * panel. Read-only: nothing on this screen writes.
     */
    st_text(16, 212, "CONFIG", F_SMALL, S_MUTED);
    if (!cfg_writable) {
        st_text_right(16, 212, "not writable", F_SUB, S_MUTED_D);
    } else {
        su_copy(v, "seq ");
        su_to_str(v + 4, cfg_seq);
        st_text_right(16, 212, v, F_SUB, S_INK);
        int i = su_to_str(v, lba0);
        su_copy(v + i, " / ");
        su_to_str(v + i + 3, lba1);
        st_text_right(16, 228, v, F_SMALL, S_MUTED_D);
    }
}
