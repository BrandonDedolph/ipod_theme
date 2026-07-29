# Design reference — iPod Classic Video (5G/5.5G)

A clean, original UI designed at the iPod's native **320×240** resolution. Rounded geometric type (Nunito), warm-light + true-dark palettes, and a complete set of menu, system, and Now Playing screens.

This package is a design reference — the canonical layouts, tokens, and behaviors the firmware UI is built from.

> **This JSX prototype predates the project's pivot, and its name is a
> fossil.** It began as a Rockbox *theme*; `core/` is now **from-scratch
> firmware that replaces Rockbox entirely** — no Rockbox code, no `.wps`, no
> theme engine. What survives is the design: the palette, chrome, screen
> layouts, and interaction model that `core/kernel/main.c` and `core/ui/`
> implement directly in C.
>
> Read this folder as **intent**, not inventory. Several screens here have no
> firmware counterpart yet; see "What the firmware actually implements" below
> before assuming a screen exists.

---

## Open the design

Open `Rockbox Theme.html` in a browser. It loads a pan/zoom canvas with every screen + an interactive iPod prototype (drag the click wheel; toggle the orange Hold switch on top).

## Files

| File | Contains |
|---|---|
| `Rockbox Theme.html` | Entry point — wires the canvas + all screens |
| `themes.jsx` | The 4 Now Playing themes (Linen, Paper, Ink, Card) and shared atoms |
| `menus.jsx` | Main menu, music sub-menus, settings, list helpers, status strip |
| `collection-detail.jsx` | Album & Playlist detail views |
| `system-screens.jsx` | Boot, shutdown, file browser, EQ, theme picker, WPS info pages, charging, locked, unlocked |
| `interactive-ipod.jsx` | Live prototype — stack-based nav, click wheel, volume, hold |
| `ipod-frame.jsx` | iPod 5G hardware frame — bezel, click wheel, hold switch |
| `volume-demo.jsx` | Volume overlay + slider demo |
| `design-canvas.jsx` | Pan/zoom canvas component |

---

## Design tokens

### Type
- **Family:** `Nunito` (400, 500, 600, 700, 800)
- **Mono:** `JetBrains Mono` (file paths, track info, peak meter)

### Color — light (Linen / Paper / Card)
- Surface: `#f4f1ec` (Linen), `#faf8f4` (Paper), `#eeeae3` (Card)
- Ink: `#1a1714`
- Muted: `#5a5048`, `#7a7068`, `#9a8e80`
- Border: `rgba(26,23,20,0.08)`
- Accent: `oklch(0.7 0.12 40)` (warm terracotta)

### Color — dark (Ink)
- Surface: `#0e0d0c`
- Text: `#e8e4dd`, `#f4ede2`
- Muted: `#7a736a`, `#a89e92`, `#6a635a`
- Accent: `oklch(0.7 0.12 40)`

### Sizes (at 320×240 native)
- Status strip: 9px caps, 0.5 letter-spacing
- Header: 12.5px, weight 700
- List row: 12px primary, 10px secondary, height 22–24px
- Now Playing title: 17px (Linen), 14px (Paper), 16px (Ink/Card)
- Progress bar: 2px (thin) or 4px (Linen)

---

## Screen inventory

### Now Playing (4 directions)
- **Theme 1 — Linen.** Warm light, text-forward. Album art + title/artist/album block, generous progress bar. Status row: "Now Playing/Paused" + battery (with hold lock when engaged).
- **Theme 2 — Paper.** Minimal, art-centered. Big art top, title/artist below, slim inline progress. Tiny play/pause glyph + "PLAYING" caps top-left.
- **Theme 3 — Ink.** True dark with terracotta accent. Smaller art, text-rich, thin accent progress.
- **Theme 4 — Card.** Light with floating card around the metadata.

All themes share a status strip (battery, hold) and respect playing/paused state.

### WPS info pages (cycle on center button)
- Page 1: Big art
- Page 2: Peak meter (real-time L/R bars)
- Page 3: Track info (key/value: codec, sample rate, bit depth, path, etc.)

### Volume overlay
Centered transient overlay shown when the user spins the wheel during playback. Speaker icon + fill bar + percentage. Light + dark variants.

### Menus & browsing
- **Main menu:** Music, Playlists, Podcasts, Audiobooks, Settings, Now Playing
- **Music sub-menu:** Artists, Albums, Songs, Genres, Composers
- **Lists:** Artists, Albums (with art chips), Songs, Genres (with track count), Playlists, Podcasts (shows), Podcast Episodes, Audiobooks
- **Detail views:** Album (art header + tracklist), Playlist (summary + tracks). Now-playing track marked.

### Settings
- Settings main, Playback, Sound, About
- Theme picker (with current selection check)
- 5-band Equalizer (60Hz / 230Hz / 910Hz / 3.6k / 14k, ±12dB)

### System
- Boot splash with progress bar
- Shutdown / sleep
- File browser (raw filesystem)

### Power & Lock states
- Charging — full-screen battery, big %, time-to-full estimate (charging vs unplugged variants)
- Locked — dim Now Playing context + centered black plate ("LOCKED") — flashes ~1s then dismisses; persistent small lock indicator stays in status bar near battery
- Unlocked — light plate ("UNLOCKED") — flashes ~1s then dismisses; corner lock disappears

---

## Interaction model

The interactive prototype treats navigation as a stack of frames:

```
{ type: "main" | "music" | "artists" | ... | "playing", sel: number }
```

- **Center button:** activate selection / drill in
- **Menu button:** pop frame (back)
- **Prev/Next:** decrement/increment selection (or scrub on Now Playing)
- **Play/Pause:** toggle playback or jump to Now Playing
- **Wheel rotation:**
  - On Now Playing → volume (briefly shows overlay)
  - On lists → moves selection
- **Hold switch (top of device):** toggles a global lock. While locked, all wheel input is blocked and shows a 1s "LOCKED" plate. Status bars across all screens render a small lock glyph next to the battery.

Lists scroll automatically so the selection stays visible (~1/3 from the top of the viewport).

---

## What the firmware actually implements

The prototype draws the whole product; the firmware is partway through it.
As of this writing, on device:

**Built and working** — main menu, Music sub-menu, Artists / Albums / Songs /
Genres / Shuffle Songs, the folder browser, album detail with an art header,
album-art chips in lists, Now Playing (120×120 cover, scrolling marquee,
progress), the play queue, Settings (with a scrolling list + scrollbar),
About / Boot Details, the volume and lock/unlock modals, the charging screen,
and the boot splash.

**In the design but not the firmware** — Playlists, Podcasts, Audiobooks and
Composers exist as *greyed-out* menu entries (`active = 0` in
`core/kernel/main.c`) and lead nowhere. An M3U8 parser exists in
`core/fs/m3u.c` but is wired to nothing, and playlist *writing* is impossible
today because the FAT driver is read-only. There is no EQ: the Sound settings
expose bass / treble / balance / crossfade as **cosmetic** controls with no
DSP behind them. The WPS info pages (big art / peak meter / track info) are
not built.

**Themes: two, not four.** Settings → Theme offers **Linen** and **Onyx** —
Onyx being this design's Ink dark palette, renamed. They are a live palette
swap, not separate screens: `core/ui/palette.c` holds `PAL_LINEN` and
`PAL_ONYX` as parallel token blocks and `theme_set()` swaps the whole block
at once. Paper and Card were not implemented; the four-Now-Playing-themes
idea was dropped in favour of one layout that reads correctly in both a light
and a dark palette.

**Type is baked, not loaded.** Nunito is pre-rasterized on the host into
static C glyph atlases (`core/ui/atlas/*.h`) at regular 9/11/13 px and bold
9/11/13/17 px — there is no font file and no font engine on the device. The
renderer is a gamma-correct AA blitter carrying a 26.6 fractional pen,
per-pair kerning, and per-atlas tracking. The `.fnt` bitmap fonts under
`tools/fonts-out/`, and the `convbdf` conversion advice this README used to
carry, are a **dead leftover of the Rockbox-theme era** — nothing reads them.
See [`../tools/README.md`](../tools/README.md). `JetBrains Mono` is not
shipped either; everything on the device is Nunito.

---

## What to keep, what to invent

This design is a reference, not a spec to trace pixel for pixel. Some
flourishes here (animated peak meter, smooth progress) buy little on a 30 MHz
ARM7 driving a framebuffer through a video coprocessor, and get approximated
or dropped. Stay faithful to:

- Type hierarchy and weights
- Color palette
- Spacing rhythm of status / header / list
- Lock + battery sitting together in the status row

…and let the rest bend to what the hardware renders well.

---

## Credits

Design built from scratch — no proprietary Apple or Rockbox-stock assets used. Free to adapt.
