/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/ui/settings_test.c — host test for the Settings state model.
 *
 * settings.c (ui/settings.c) is the pure half of the Settings subsystem: it
 * touches nothing but settings_t + stdint, so the on-device path and this test
 * compile the SAME source. This proves the model contract main.c relies on:
 *   1. Defaults: the documented starting values.
 *   2. activate() SELECT: toggles Shuffle, cycles Repeat OFF->ALL->ONE->OFF,
 *      flips Crossfade / Resume, sets a theme.
 *   3. adjust() wheel: clamps Volume/Bass and Display Brightness to range.
 *   4. Navigation: Root rows return the right ENTER_* / RESET action codes;
 *      value/kind reporting for a toggle and a slider row.
 * No MMIO, no framebuffer — plain cc.
 */

#include "settings.h"

#include <stdio.h>

static int g_fail = 0;
static int check(const char *label, int cond)
{
    printf("[%s] %s\n", label, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_fail = 1;
    }
    return cond;
}

int main(void)
{
    settings_t s;

    /* --- Test 1: defaults --- */
    settings_defaults(&s);
    check("def-shuffle",   s.shuffle == 0);
    check("def-repeat",    s.repeat == REPEAT_OFF);
    check("def-volume",    s.volume == 70);
    check("def-bl-secs",   s.backlight_secs == 15);
    check("def-bl-bright", s.backlight_bright == 32);
    check("def-theme",     s.theme == 0);
    check("def-bass-0",    s.bass == 0 && s.treble == 0 && s.balance == 0);

    /* --- Test 2: activate() toggles Shuffle --- */
    check("shuffle-0", s.shuffle == 0);
    check("act-shuffle-none",
          settings_activate(SETTINGS_PLAYBACK, &s, 0) == SETTINGS_ACTION_NONE);
    check("shuffle-1", s.shuffle == 1);
    settings_activate(SETTINGS_PLAYBACK, &s, 0);
    check("shuffle-back-0", s.shuffle == 0);

    /* --- Test 3: activate() cycles Repeat OFF -> ALL -> ONE -> OFF --- */
    check("repeat-off", s.repeat == REPEAT_OFF);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-all", s.repeat == REPEAT_ALL);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-one", s.repeat == REPEAT_ONE);
    settings_activate(SETTINGS_PLAYBACK, &s, 1);
    check("repeat-wrap-off", s.repeat == REPEAT_OFF);

    /* --- Test 4: Balance adjust clamps to [-100,100] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 3, +10);
    check("balance-right", s.balance == 10);
    settings_adjust(SETTINGS_SOUND, &s, 3, -1000);
    check("balance-clamp-lo", s.balance == -100);

    /* --- Test 5: adjust() clamps Volume to [0,100] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 0, +5);
    check("vol-up", s.volume == 75);
    settings_adjust(SETTINGS_SOUND, &s, 0, +1000);
    check("vol-clamp-hi", s.volume == 100);
    settings_adjust(SETTINGS_SOUND, &s, 0, -1000);
    check("vol-clamp-lo", s.volume == 0);

    /* --- Test 6: adjust() clamps Bass to [-12,12] --- */
    settings_defaults(&s);
    settings_adjust(SETTINGS_SOUND, &s, 1, +100);
    check("bass-clamp-hi", s.bass == 12);
    settings_adjust(SETTINGS_SOUND, &s, 1, -100);
    check("bass-clamp-lo", s.bass == -12);

    /* --- Test 7: adjust() clamps Display Brightness to [1,32] --- */
    settings_defaults(&s);                     /* bright = 32 */
    settings_adjust(SETTINGS_DISPLAY, &s, 1, +5);
    check("bright-clamp-hi", s.backlight_bright == 32);
    settings_adjust(SETTINGS_DISPLAY, &s, 1, -1000);
    check("bright-clamp-lo", s.backlight_bright == 1);

    /* --- Test 8: Display Backlight cycles the discrete list (activate wraps) */
    settings_defaults(&s);                     /* secs = 15 */
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-15->30", s.backlight_secs == 30);
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-30->60", s.backlight_secs == 60);
    settings_activate(SETTINGS_DISPLAY, &s, 0);
    check("bl-60->0(wrap)", s.backlight_secs == 0);
    /* adjust clamps at the ends (no wrap). */
    settings_adjust(SETTINGS_DISPLAY, &s, 0, -1);
    check("bl-0-clamp", s.backlight_secs == 0);

    /* --- Test 9: Theme select sets the theme index (Linen=0 / Onyx=1) --- */
    settings_defaults(&s);
    settings_activate(SETTINGS_THEME, &s, 1);
    check("theme-set-onyx", s.theme == 1);
    settings_activate(SETTINGS_THEME, &s, 0);
    check("theme-set-linen", s.theme == 0);

    /* --- Test 10: Root rows return the right action codes --- */
    check("enter-playback",
          settings_activate(SETTINGS_ROOT, &s, 0) == SETTINGS_ENTER_PLAYBACK);
    check("enter-sound",
          settings_activate(SETTINGS_ROOT, &s, 1) == SETTINGS_ENTER_SOUND);
    check("enter-theme",
          settings_activate(SETTINGS_ROOT, &s, 2) == SETTINGS_ENTER_THEME);
    check("enter-display",
          settings_activate(SETTINGS_ROOT, &s, 3) == SETTINGS_ENTER_DISPLAY);
    check("enter-about",
          settings_activate(SETTINGS_ROOT, &s, 5) == SETTINGS_ENTER_ABOUT);
    check("diskmode-action",
          settings_activate(SETTINGS_ROOT, &s, 6) == SETTINGS_ACTION_DISKMODE);
    check("reset-action",
          settings_activate(SETTINGS_ROOT, &s, 7) == SETTINGS_ACTION_RESET);
    /* Both fire on SELECT rather than descending — a SUBMENU kind here would
     * make the UI push a screen that does not exist. */
    check("diskmode-is-action",
          settings_kind(SETTINGS_ROOT, 6) == SETTINGS_KIND_ACTION);
    check("reset-is-action",
          settings_kind(SETTINGS_ROOT, 7) == SETTINGS_KIND_ACTION);
    check("enter-clicker",
          settings_activate(SETTINGS_ROOT, &s, 4) == SETTINGS_ENTER_CLICKER);
    check("count-clicker", settings_count(SETTINGS_CLICKER) == 8);
    settings_activate(SETTINGS_CLICKER, &s, 2);      /* pick "Click" */
    check("clicker-pick", s.clicker == 2);
    settings_activate(SETTINGS_CLICKER, &s, 0);      /* pick "Off"   */
    check("clicker-off", s.clicker == 0);

    /* --- Test 11: counts + generic value/kind reporting --- */
    check("count-root",  settings_count(SETTINGS_ROOT) == 8);
    check("count-play",  settings_count(SETTINGS_PLAYBACK) == 2);
    check("count-sound", settings_count(SETTINGS_SOUND) == 4);
    check("count-theme", settings_count(SETTINGS_THEME) == 2);

    settings_defaults(&s);
    {
        char buf[24];
        int is_toggle = 0, on = 0, num = 0, den = 0;

        /* Volume slider reports fraction 70/100 and "70%". */
        settings_value(SETTINGS_SOUND, &s, 0, buf, &is_toggle, &on, &num, &den);
        check("vol-slider-frac", is_toggle == 0 && num == 70 && den == 100);
        check("vol-slider-text",
              buf[0] == '7' && buf[1] == '0' && buf[2] == '%' && buf[3] == '\0');

        /* Balance slider reports "Center" at 0 and the mid fraction. */
        settings_value(SETTINGS_SOUND, &s, 3, buf, &is_toggle, &on, &num, &den);
        check("bal-slider-mid", is_toggle == 0 && num == 100 && den == 200);
        check("bal-text-center",
              buf[0] == 'C' && buf[1] == 'e' && buf[2] == 'n');

        /* Repeat select reports "Off" initially. */
        settings_value(SETTINGS_PLAYBACK, &s, 1, buf, &is_toggle, &on,
                       &num, &den);
        check("repeat-text-off",
              buf[0] == 'O' && buf[1] == 'f' && buf[2] == 'f');
        check("repeat-kind-select",
              settings_kind(SETTINGS_PLAYBACK, 1) == SETTINGS_KIND_SELECT);
    }

    printf("settings_test: %s\n", g_fail ? "FAIL" : "OK");
    return g_fail ? 1 : 0;
}
