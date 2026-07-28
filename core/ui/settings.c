/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settings.c — the Settings state model (no framebuffer, no hardware).
 *
 * The pure half of the Settings subsystem: defaults, the generic per-row view
 * (count / label / kind / value) that lets main.c drive navigation without
 * hardcoding a screen, and the SELECT/wheel mutators (activate / adjust). It
 * touches nothing but settings_t and stdint, so the host unit test compiles this
 * exact source. The renderer lives in screen_settings.c.
 *
 * Freestanding, integer-only: no libc/libm/malloc. Small string formatting is
 * done by hand into caller buffers.
 */

#include "settings.h"

/* ---------------------------------------------------------------------------
 * Small freestanding helpers
 * ------------------------------------------------------------------------- */

/* Copy a NUL-terminated string (no libc). Caller guarantees the destination is
 * large enough (all uses below write <= 20 bytes into a >= 24-byte buffer). */
static void scopy(char *d, const char *s)
{
    while (*s) {
        *d++ = *s++;
    }
    *d = '\0';
}

/* Write unsigned `v` as decimal at `d`; returns the digit count. */
static int u_to_str(char *d, unsigned v)
{
    char tmp[10];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && t < 10);
    int i = 0;
    while (t > 0) {
        d[i++] = tmp[--t];
    }
    d[i] = '\0';
    return i;
}

/* Clamp `v` into [lo, hi]. */
static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Format a signed dB value: "0 dB" / "+N dB" / "-N dB". */
static void fmt_db(char *d, int v)
{
    int i = 0;
    if (v > 0) {
        d[i++] = '+';
    } else if (v < 0) {
        d[i++] = '-';
        v = -v;
    }
    i += u_to_str(d + i, (unsigned)v);
    scopy(d + i, " dB");
}

/* Format a percent: "NN%". */
static void fmt_pct(char *d, int v)
{
    int i = u_to_str(d, (unsigned)(v < 0 ? 0 : v));
    d[i++] = '%';
    d[i] = '\0';
}

/* Format a balance value: "Center" / "Left N" / "Right N". */
static void fmt_balance(char *d, int v)
{
    if (v == 0) {
        scopy(d, "Center");
        return;
    }
    const char *side = v < 0 ? "Left " : "Right ";
    int i = 0;
    while (side[i]) { d[i] = side[i]; i++; }
    if (v < 0) v = -v;
    u_to_str(d + i, (unsigned)v);
}

/* ---------------------------------------------------------------------------
 * Backlight-timeout discrete steps (0=never / 5 / 10 / 15 / 30 / 60 seconds)
 * ------------------------------------------------------------------------- */
static const int BL_OPTS[6] = { 0, 5, 10, 15, 30, 60 };

static int bl_index(int secs)
{
    for (int i = 0; i < 6; i++) {
        if (BL_OPTS[i] == secs) {
            return i;
        }
    }
    return 3;                                  /* default to 15 s if unknown */
}

/* Step to the next backlight option in direction `dir` (+/-). `wrap` wraps the
 * ends (used by SELECT); otherwise it clamps (used by the wheel). */
static int bl_step(int secs, int dir, int wrap)
{
    int i = bl_index(secs) + (dir > 0 ? 1 : -1);
    if (wrap) {
        i = (i + 6) % 6;
    } else {
        i = clampi(i, 0, 5);
    }
    return BL_OPTS[i];
}

/* ---------------------------------------------------------------------------
 * Row labels (stable .rodata tables, one per list screen)
 * ------------------------------------------------------------------------- */
/* Only rows that actually do something are listed — the cosmetic placeholders
 * (Crossfade, Replaygain, Skip Length, Stereo Width, Shortcuts, Language) were
 * removed so the menu never presents a control that has no effect. */
static const char *const ROOT_L[8] = {
    "Playback", "Sound", "Theme", "Display", "Clicker", "About",
    "Disk Mode", "Reset Settings",
};
/* Resume is back on this list: it was pulled with the other placeholders while
 * nothing could persist it, and it is now the switch that decides whether boot
 * re-opens the track you left off on (kernel/main.c resume_restore). */
static const char *const PLAY_L[3] = { "Shuffle", "Repeat", "Resume" };
static const char *const SOUND_L[4] = { "Volume", "Bass", "Treble", "Balance" };
static const char *const DISP_L[2] = { "Backlight", "Brightness" };
static const char *const THEME_L[2] = { "Linen", "Onyx" };
/* Clicker: index 0 = Off, 1..N = sound profiles (main.c maps to piezo tones). */
static const char *const CLICK_L[8] = {
    "Off", "Tick", "Click", "Pop", "Blip", "Tock", "Double", "Chirp",
};
#define CLICK_N ((int)(sizeof CLICK_L / sizeof CLICK_L[0]))

/* ---------------------------------------------------------------------------
 * Public model API
 * ------------------------------------------------------------------------- */

void settings_defaults(settings_t *s)
{
    s->shuffle           = 0;
    s->repeat            = REPEAT_OFF;
    s->resume_on_startup = 1;
    s->crossfade         = 0;
    s->volume            = 70;
    s->bass              = 0;
    s->treble            = 0;
    s->balance           = 0;
    s->backlight_secs    = 15;
    s->backlight_bright  = 32;
    s->theme             = 0;
    s->clicker           = 1;
    /* Nothing to resume from a fresh install — and "Reset Settings" routes
     * through here too, so it also forgets where you were. */
    s->resume_hash       = 0;
    s->resume_secs       = 0;
    s->resume_total      = 0;
}

int settings_count(int screen)
{
    switch (screen) {
    case SETTINGS_ROOT:     return 8;
    case SETTINGS_PLAYBACK: return 3;
    case SETTINGS_SOUND:    return 4;
    case SETTINGS_DISPLAY:  return 2;
    case SETTINGS_ABOUT:    return 1;   /* non-interactive info page */
    case SETTINGS_THEME:    return 2;
    case SETTINGS_CLICKER:  return CLICK_N;   /* Off + sound profiles */
    default:                return 0;
    }
}

const char *settings_label(int screen, int idx)
{
    if (idx < 0 || idx >= settings_count(screen)) {
        return "";
    }
    switch (screen) {
    case SETTINGS_ROOT:     return ROOT_L[idx];
    case SETTINGS_PLAYBACK: return PLAY_L[idx];
    case SETTINGS_SOUND:    return SOUND_L[idx];
    case SETTINGS_DISPLAY:  return DISP_L[idx];
    case SETTINGS_THEME:    return THEME_L[idx];
    case SETTINGS_CLICKER:  return CLICK_L[idx];
    default:                return "";
    }
}

const char *settings_theme_name(int theme)
{
    if (theme < 0 || theme > 1) {
        return "Linen";
    }
    return THEME_L[theme];
}

const char *settings_clicker_name(int profile)
{
    if (profile < 0 || profile >= CLICK_N) {
        return "Off";
    }
    return CLICK_L[profile];
}

int settings_kind(int screen, int idx)
{
    switch (screen) {
    case SETTINGS_ROOT:
        /* Disk Mode + Reset Settings both fire on SELECT; the rest descend. */
        if (idx == 6 || idx == 7) return SETTINGS_KIND_ACTION;
        return SETTINGS_KIND_SUBMENU;                 /* incl. Clicker submenu */
    case SETTINGS_CLICKER:
        return SETTINGS_KIND_SELECT;                  /* radio pick, marked active */
    case SETTINGS_PLAYBACK:
        return SETTINGS_KIND_SELECT;       /* Shuffle + Repeat: cycling selects */
    case SETTINGS_SOUND:
        return SETTINGS_KIND_SLIDER;       /* Volume / Bass / Treble / Balance  */
    case SETTINGS_DISPLAY:
        return (idx == 1) ? SETTINGS_KIND_SLIDER : SETTINGS_KIND_SELECT;
    case SETTINGS_THEME:
        return SETTINGS_KIND_THEME;
    case SETTINGS_ABOUT:
        return SETTINGS_KIND_INFO;
    default:
        return SETTINGS_KIND_SELECT;
    }
}

void settings_value(int screen, const settings_t *s, int idx,
                    char *buf, int *is_toggle, int *toggle_on,
                    int *num, int *den)
{
    buf[0] = '\0';
    *is_toggle = 0;
    *toggle_on = 0;
    *num = 0;
    *den = 0;

    switch (screen) {
    case SETTINGS_ROOT:
        /* Theme + Clicker carry a right value (the current choice); rest chevrons. */
        if (idx == 2) {
            scopy(buf, settings_theme_name(s->theme));
        } else if (idx == 4) {
            scopy(buf, settings_clicker_name(s->clicker));
        }
        break;

    case SETTINGS_CLICKER:
        if (idx == s->clicker) {
            scopy(buf, "\x03");                 /* middot marks the active profile */
        }
        break;

    case SETTINGS_PLAYBACK:
        switch (idx) {
        case 0: scopy(buf, s->shuffle ? "On" : "Off"); break;
        case 1: scopy(buf, s->repeat == REPEAT_OFF ? "Off"
                         : s->repeat == REPEAT_ALL ? "All" : "One"); break;
        case 2: scopy(buf, s->resume_on_startup ? "On" : "Off"); break;
        default: break;
        }
        break;

    case SETTINGS_SOUND:
        switch (idx) {
        case 0: fmt_pct(buf, s->volume); *num = s->volume;      *den = 100; break;
        case 1: fmt_db(buf, s->bass);    *num = s->bass + 12;   *den = 24;  break;
        case 2: fmt_db(buf, s->treble);  *num = s->treble + 12; *den = 24;  break;
        case 3: fmt_balance(buf, s->balance);
                *num = s->balance + 100; *den = 200; break;
        default: break;
        }
        break;

    case SETTINGS_DISPLAY:
        if (idx == 0) {
            if (s->backlight_secs == 0) {
                scopy(buf, "Never");
            } else {
                int i = u_to_str(buf, (unsigned)s->backlight_secs);
                scopy(buf + i, " sec");
            }
        } else if (idx == 1) {
            int pc = s->backlight_bright * 100 / 32;
            fmt_pct(buf, pc);
            *num = s->backlight_bright;
            *den = 32;
        }
        break;

    default:
        break;                                 /* Theme/About draw their own */
    }
}

int settings_activate(int screen, settings_t *s, int idx)
{
    switch (screen) {
    case SETTINGS_ROOT:
        switch (idx) {
        case 0: return SETTINGS_ENTER_PLAYBACK;
        case 1: return SETTINGS_ENTER_SOUND;
        case 2: return SETTINGS_ENTER_THEME;
        case 3: return SETTINGS_ENTER_DISPLAY;
        case 4: return SETTINGS_ENTER_CLICKER;
        case 5: return SETTINGS_ENTER_ABOUT;
        case 6: return SETTINGS_ACTION_DISKMODE;
        case 7: return SETTINGS_ACTION_RESET;
        default: return SETTINGS_ACTION_NONE;
        }

    case SETTINGS_PLAYBACK:
        switch (idx) {
        case 0: s->shuffle = !s->shuffle; break;
        case 1: s->repeat = (repeat_mode_t)((s->repeat + 1) % 3); break;
        /* Turning Resume OFF does not clear the stored locator here — this
         * module is pure, and main.c owns that (it drops it on the next pass,
         * so the record on disk stops carrying a position you asked it to
         * forget). */
        case 2: s->resume_on_startup = !s->resume_on_startup; break;
        default: break;
        }
        return SETTINGS_ACTION_NONE;

    case SETTINGS_DISPLAY:
        if (idx == 0) {
            s->backlight_secs = bl_step(s->backlight_secs, +1, 1 /*wrap*/);
        }
        return SETTINGS_ACTION_NONE;

    case SETTINGS_THEME:
        if (idx >= 0 && idx < 2) {
            s->theme = idx;
        }
        return SETTINGS_ACTION_NONE;

    case SETTINGS_CLICKER:
        if (idx >= 0 && idx < CLICK_N) {
            s->clicker = idx;
        }
        return SETTINGS_ACTION_NONE;

    default:
        return SETTINGS_ACTION_NONE;
    }
}

void settings_adjust(int screen, settings_t *s, int idx, int delta)
{
    if (delta == 0) {
        return;
    }
    switch (screen) {
    case SETTINGS_SOUND:
        switch (idx) {
        case 0: s->volume  = clampi(s->volume + delta, 0, 100);   break;
        case 1: s->bass    = clampi(s->bass + delta, -12, 12);    break;
        case 2: s->treble  = clampi(s->treble + delta, -12, 12);  break;
        case 3: s->balance = clampi(s->balance + delta, -100, 100); break;
        default: break;
        }
        break;

    case SETTINGS_DISPLAY:
        if (idx == 1) {
            s->backlight_bright = clampi(s->backlight_bright + delta, 1, 32);
        } else if (idx == 0) {
            s->backlight_secs = bl_step(s->backlight_secs, delta, 0 /*clamp*/);
        }
        break;

    default:
        break;
    }
}
