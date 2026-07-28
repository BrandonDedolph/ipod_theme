/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/ui/settings.h — data-driven Settings model + render API.
 *
 * A self-contained Settings subsystem for the Linen menu UI
 * (design_reference/menus.jsx SettingsMenu/SettingsPlayback/SettingsSound/
 * SettingsAbout + system-screens.jsx ThemePicker). The state lives in a plain
 * settings_t owned by the caller (kernel/main.c); this module is a pure,
 * freestanding model over it plus a matching renderer:
 *
 *   - settings_count / settings_label / settings_kind / settings_value give
 *     main.c a GENERIC per-row view of each screen, so navigation can be driven
 *     without hardcoding any one screen.
 *   - settings_activate handles SELECT on a row: it either mutates *s in place
 *     (toggle a bool, cycle a select) and returns SETTINGS_ACTION_NONE, or — for
 *     a submenu/reset row — returns an action code for main.c to switch on.
 *   - settings_adjust handles a wheel tick on a slider row (Sound / Display),
 *     nudging the value clamped to its range.
 *   - settings_render / settings_about_render draw the full 320x240 panel into
 *     the console framebuffer, matching the jsx.
 *
 * FUNCTIONAL fields drive real hardware once main.c wires them: shuffle, repeat
 * (player), volume (mirrors hal_volume), backlight_secs + backlight_bright
 * (backlight HAL). COSMETIC fields render + store but nothing consumes them yet
 * (crossfade, resume_on_startup, bass, treble, balance, theme>0) — flagged at
 * their declarations below.
 *
 * Freestanding: integer-only, no libc/libm/malloc, no allocation. The model
 * half (settings.c) has no hardware or framebuffer dependency at all, so it
 * host-compiles into the unit test unchanged.
 */

#ifndef CORE_UI_SETTINGS_H
#define CORE_UI_SETTINGS_H

#include <stdint.h>

/* Repeat policy (FUNCTIONAL — consumed by the player's auto-advance). */
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_mode_t;

/*
 * The whole persisted settings state. Ranges + which fields are live are noted
 * per field; keep this in sync with settings_defaults() and the clamps in
 * settings_adjust().
 */
typedef struct {
    int  shuffle;            /* 0/1 — FUNCTIONAL (player queue order)          */
    repeat_mode_t repeat;    /* FUNCTIONAL (player auto-advance)               */
    int  resume_on_startup;  /* 0/1 — COSMETIC for now                        */
    int  crossfade;          /* 0/1 — COSMETIC (no crossfade mixer yet)       */
    int  volume;             /* 0..100 — FUNCTIONAL (mirrors hal_volume)      */
    int  bass, treble;       /* -12..12 dB — COSMETIC (no EQ wired yet)       */
    int  balance;            /* -100..100 — COSMETIC                          */
    int  backlight_secs;     /* 0=never / 5/10/15/30/60 — FUNCTIONAL          */
    int  backlight_bright;   /* 1..32 — FUNCTIONAL                            */
    int  theme;              /* 0..3; only 0 (Linen) renders — COSMETIC >0    */
    int  clicker;            /* 0/1 — FUNCTIONAL (piezo click on navigation)  */
} settings_t;

/* Populate `s` with sensible defaults (shuffle off, repeat off, volume 70,
 * backlight 15 s at full brightness, Linen theme). */
void settings_defaults(settings_t *s);

/*
 * The Settings screens. main.c keeps the current screen + a selection index and
 * calls the generic accessors below against them. SETTINGS_SCREEN_COUNT is the
 * count sentinel, not a real screen.
 */
typedef enum {
    SETTINGS_ROOT,       /* the top Settings menu                             */
    SETTINGS_PLAYBACK,   /* Shuffle / Repeat / Crossfade / … toggles+selects  */
    SETTINGS_SOUND,      /* Volume / Bass / Treble / Balance / Width sliders  */
    SETTINGS_DISPLAY,    /* Backlight timeout (select) + Brightness (slider)  */
    SETTINGS_ABOUT,      /* device info key/value rows                        */
    SETTINGS_THEME,      /* theme picker (swatch rows)                        */
    SETTINGS_CLICKER,    /* clicker profile picker (Off / sound profiles)     */
    SETTINGS_SCREEN_COUNT
} settings_screen_t;

/*
 * Return codes from settings_activate(). NONE means "handled in place (value
 * mutated), stay on this screen"; the ENTER_* codes ask main.c to push the
 * named sub-screen; RESET asks it to restore defaults.
 */
typedef enum {
    SETTINGS_ACTION_NONE = 0,
    SETTINGS_ENTER_PLAYBACK,
    SETTINGS_ENTER_SOUND,
    SETTINGS_ENTER_DISPLAY,
    SETTINGS_ENTER_ABOUT,
    SETTINGS_ENTER_THEME,
    SETTINGS_ENTER_CLICKER,
    SETTINGS_ACTION_RESET,
    /* Reboot into the Apple boot ROM's USB mass-storage mode. Lives under
     * Settings rather than the main menu: it is a maintenance action, not a
     * place you browse to. */
    SETTINGS_ACTION_DISKMODE
} settings_action_t;

/*
 * Per-row kind, so a caller (and the renderer) can treat any screen generically:
 *   SUBMENU  a row that enters another screen (chevron, or a right value)
 *   ACTION   a row that fires an action on SELECT (Reset Settings)
 *   TOGGLE   an on/off pill (SELECT flips it)
 *   SELECT   a right-aligned text value (SELECT cycles it)
 *   SLIDER   a label + fill bar (the wheel adjusts it)
 *   THEME    a theme-picker swatch row
 *   INFO     an About key/value row
 */
typedef enum {
    SETTINGS_KIND_SUBMENU,
    SETTINGS_KIND_ACTION,
    SETTINGS_KIND_TOGGLE,
    SETTINGS_KIND_SELECT,
    SETTINGS_KIND_SLIDER,
    SETTINGS_KIND_THEME,
    SETTINGS_KIND_INFO
} settings_kind_t;

/* Number of selectable rows on `screen` (About returns 1 — it is a non-
 * interactive info page). Out-of-range screens return 0. */
int settings_count(int screen);

/* The row label for (screen, idx), or "" if out of range. Stable .rodata. */
const char *settings_label(int screen, int idx);

/* The display name of theme index `theme` ("Linen"/"Onyx"). */
const char *settings_theme_name(int theme);

/* The display name of clicker profile `profile` ("Off"/"Tick"/"Click"/"Pop"). */
const char *settings_clicker_name(int profile);

/* The settings_kind_t of row (screen, idx). */
int settings_kind(int screen, int idx);

/*
 * Fill in the render-facing value of row (screen, idx):
 *   buf         (>= 24 bytes) receives the display text for SELECT/SLIDER/
 *               submenu-with-value rows ("" otherwise).
 *   *is_toggle  set to 1 for a TOGGLE row (then *toggle_on = its state).
 *   *num,*den   for a SLIDER row, the fill fraction num/den (den>0); 0 else.
 * Every out-param is always written. Pure — no side effects on *s.
 */
void settings_value(int screen, const settings_t *s, int idx,
                    char *buf, int *is_toggle, int *toggle_on,
                    int *num, int *den);

/*
 * Apply SELECT to row (screen, idx): mutate *s in place for a toggle/select and
 * return SETTINGS_ACTION_NONE, or return a settings_action_t for main.c to act
 * on (enter a sub-screen / reset). Out-of-range rows return NONE.
 */
int settings_activate(int screen, settings_t *s, int idx);

/*
 * Apply a wheel tick of `delta` to a SLIDER row (Sound values, Display
 * Brightness) or step a discrete SELECT (Display Backlight). Clamped to range;
 * a no-op on rows that are not adjustable.
 */
void settings_adjust(int screen, settings_t *s, int idx, int delta);

/*
 * Render the full 320x240 panel for `screen` into the console framebuffer
 * (matches the jsx). For SETTINGS_ABOUT this draws placeholder dashes — main.c
 * should call settings_about_render() directly with the live device values.
 */
void settings_render(int screen, const settings_t *s, int sel);

/*
 * Render the About screen from live values (main.c owns these — do not
 * fabricate). free/total are whole megabytes; pct<0 or mv<=0 render as "--".
 */
void settings_about_render(int battery_pct, int battery_mv,
                           uint32_t total_mb, uint32_t free_mb,
                           int n_songs, int n_albums, int n_artists);

#endif /* CORE_UI_SETTINGS_H */
