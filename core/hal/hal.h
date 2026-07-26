/*
 * core/hal/hal.h — Hardware Abstraction Layer contract.
 *
 * This is the only header the kernel + apps + codecs include to talk to
 * "the hardware." Two backends implement it:
 *
 *   hal/hw/   — touches real PP5022 registers; runs on the iPod
 *   hal/sim/  — backed by SDL2 + a file-backed disk image; runs on the host
 *
 * The contract is deliberately small. Everything above the HAL is
 * portable; everything below is target-specific.
 *
 * Convention: functions return 0 on success, negative on failure. Output
 * pointers are written only on success.
 */

#ifndef CORE_HAL_H
#define CORE_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- Generic ------------------------------------------------- */

/*
 * hal_init initializes the entire HAL: LCD, button, clock, log.
 * Must be called once before any other hal_* function.
 * Returns 0 on success, negative on init failure.
 */
int hal_init(void);

/*
 * hal_shutdown tears down the HAL cleanly. Should be called before
 * exit so the sim window closes nicely. Has no effect on hw target.
 */
void hal_shutdown(void);

/* ---------- LCD ----------------------------------------------------- */

#define LCD_WIDTH   320
#define LCD_HEIGHT  240
#define LCD_BPP     16    /* RGB565 */

/* Pixel type. RGB565 little-endian both targets. */
typedef uint16_t lcd_pixel_t;

/*
 * lcd_framebuffer returns a pointer to the host-side back buffer.
 * Writes to this buffer don't appear on the LCD until lcd_present().
 * Buffer is LCD_WIDTH * LCD_HEIGHT * sizeof(lcd_pixel_t) = 153,600 bytes.
 */
lcd_pixel_t *lcd_framebuffer(void);

/*
 * lcd_present pushes the back buffer to the display. On hw this issues
 * BCMCMD_LCD_UPDATE; on sim it copies into the SDL texture and presents.
 */
void lcd_present(void);

/*
 * Convenience: pack a 24-bit (R8G8B8) color into RGB565.
 */
static inline lcd_pixel_t lcd_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (lcd_pixel_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * Convenience: fill the entire framebuffer with one color. Doesn't
 * present; caller must lcd_present() to make it visible.
 */
void lcd_fill(lcd_pixel_t color);

/* ---------- Button -------------------------------------------------- */

/*
 * Button bitmap. Multiple bits can be set (e.g. SELECT held while
 * SCROLL_FWD).
 *
 * These are *logical* button identifiers, NOT the iPod's wheel-packet
 * bit positions (which live at bits 8–12 per
 * core/docs/hw/03-clickwheel.md). The hw backend translates between
 * the two; sim and any future test fixtures emit logical bits
 * directly. We diverge from the wheel-packet layout deliberately so
 * we can fit BUTTON_HOLD / BUTTON_QUIT / future synthetic events into
 * the same bitmap without colliding with the wheel-packet wire format.
 */
typedef enum {
    BUTTON_NONE        = 0,
    BUTTON_SELECT      = 1u << 0,
    BUTTON_RIGHT       = 1u << 1,
    BUTTON_LEFT        = 1u << 2,
    BUTTON_PLAY        = 1u << 3,
    BUTTON_MENU        = 1u << 4,
    BUTTON_SCROLL_FWD  = 1u << 5,
    BUTTON_SCROLL_BACK = 1u << 6,
    BUTTON_HOLD        = 1u << 7,
    BUTTON_QUIT        = 1u << 8,   /* sim only: window-close */
} button_t;

/*
 * button_get blocks for up to timeout_ms milliseconds and returns a
 * button bitmap (or BUTTON_NONE on timeout). timeout_ms == 0 means
 * "non-blocking poll"; timeout_ms == -1 means "block until an event".
 */
button_t button_get(int timeout_ms);

/* ---------- Clock --------------------------------------------------- */

/*
 * clock_ms returns monotonic milliseconds since hal_init().
 * Wraps after ~49.7 days (uint32_t).
 */
uint32_t clock_ms(void);

/*
 * clock_us returns monotonic microseconds since hal_init(). Used for
 * fine-grained timing; wraps after ~71 minutes (uint32_t). For longer
 * intervals use clock_ms.
 */
uint32_t clock_us(void);

/*
 * sleep_ms blocks the current thread for at least ms milliseconds.
 * Coarse — minimum granularity is the OS / kernel tick.
 */
void sleep_ms(uint32_t ms);

/*
 * Sim-only: dump the current framebuffer to a 24-bit BMP at `path`.
 * Returns 0 on success, negative on I/O failure. Stub on hw target.
 *
 * Used by the headless test harness and ad-hoc screenshot capture.
 */
int lcd_screenshot_bmp(const char *path);

/* ---------- Audio --------------------------------------------------- */

/*
 * Audio output uses a pull/callback model. The HAL clocks samples
 * out at the rate set in hal_audio_init(); when its internal buffer
 * empties, it calls the registered audio_source_fn to refill.
 *
 * Sim:  SDL2 audio callback runs on SDL's audio thread.
 * Hw:   I²S DMA-empty IRQ schedules a callback on the audio task.
 *
 * The callback model maps cleanly to both ends. The audio engine
 * later sits between decoder_t (pull from a codec) and the HAL
 * (push to this fill function); see core/apps/audio/.
 *
 * Output format is fixed at 16-bit signed interleaved PCM. Only the
 * sample rate and channel count are configurable per stream.
 */

/*
 * audio_source_fn — invoked by the HAL when it needs samples.
 *
 * Fill `buf` with up to `frames` frames of interleaved s16 PCM.
 * Return the number of frames actually written. If you write fewer
 * than `frames`, the HAL pads the remainder with silence and assumes
 * the source is starved (sim) / underrun (hw).
 *
 * Runs in a real-time context (SDL audio thread on sim; IRQ-driven
 * task on hw). Don't block, don't malloc, don't call back into the
 * HAL. Pull from a lock-free ring buffer.
 *
 * `userdata` is the pointer registered in hal_audio_set_source.
 */
typedef int (*audio_source_fn)(void *userdata, int16_t *buf, int frames);

/*
 * hal_audio_init configures the output device.
 *
 * sample_rate is in Hz (typical: 44100, 48000). channels must be 1 or 2.
 * Returns 0 on success, negative on failure:
 *   -1  unsupported sample rate or channel count
 *   -2  the output device came up but the CODEC did not respond
 *
 * The -2 case matters on hw: a codec that never answers gives a UI with a
 * moving progress bar and total silence, so it must not be reported as
 * success. (The signal is coarse — the PP502x I2C controller exposes no
 * per-byte NAK, so a wedged bus is the only detectable failure.)
 *
 * On the hw backend the reachable rates are the codec PLL presets: 44100,
 * 48000, 32000, 24000 and 22050. Mono (channels == 1) is accepted and
 * expanded to the stereo link inside the HAL — the caller supplies one
 * sample per frame and does not pre-duplicate.
 *
 * hw ALSO re-applies the user's volume/balance/bass/treble as the last step
 * of init, because init issues a full codec reset. Callers may still re-apply
 * them afterwards; that is redundant but harmless.
 *
 * Strict on the device's capabilities: if the underlying audio system
 * cannot give us *exactly* the requested rate and channel count, we
 * return failure rather than silently resampling — wrong-pitch audio
 * would be a worse failure mode than "device unavailable, fall back."
 * Callers (the audio engine) are expected to retry with a different
 * rate if the first try fails.
 *
 * Idempotent — calling twice with the same parameters is a no-op;
 * calling with different parameters tears down and re-opens.
 *
 * After init, no audio plays until hal_audio_set_source + hal_audio_start.
 */
int hal_audio_init(uint32_t sample_rate, uint16_t channels);

/*
 * Register the source callback. Safe to call from any thread, at any
 * time after hal_audio_init.
 *
 * Quiescence guarantee: when this function returns, the previously
 * registered fn/userdata are no longer in flight in any callback.
 * It is therefore safe to free `userdata` immediately after
 * set_source returns with a different fn (or NULL).
 *
 * Pass fn = NULL to clear (silence on next pull).
 */
void hal_audio_set_source(audio_source_fn fn, void *userdata);

/*
 * hal_audio_start unpauses the output. The HAL begins pulling from the
 * source. Safe to call repeatedly.
 */
void hal_audio_start(void);

/*
 * hal_audio_stop pauses output. The internal buffer is not cleared —
 * a subsequent hal_audio_start resumes from where we left off.
 *
 * hw: the codec is soft-muted BEFORE the transfer is cut, so stop/skip does
 * not pop. Resume picks up inside the buffer that was mid-transfer (timed
 * against the free-running microsecond counter, since the DMA engine exposes
 * no residual byte count), so no already-decoded audio is discarded.
 *
 * Stop does NOT power the codec down — PLL, VMID and the DAC stay live, which
 * is the full analog budget. A device left paused indefinitely should be shut
 * down with hal_audio_close() after a timeout and re-opened with
 * hal_audio_init() on resume; init is idempotent and per-track anyway.
 */
void hal_audio_stop(void);

/*
 * hal_audio_close releases the output device. After this hal_audio_init
 * must be called again before further audio is possible.
 */
void hal_audio_close(void);

/* ---------- Volume / Backlight ------------------------------------- */

/*
 * Volume: 0..100 (% of full output gain). Sim doesn't actually
 * attenuate SDL audio — the value is held + logged on change. Hw will
 * scale the I²S stream (or drive the codec's gain register).
 *
 * Out-of-range values are clamped, not rejected.
 */
void hal_volume_set(int percent);
int  hal_volume_get(void);

/*
 * Backlight: 0..100. Sim logs only; hw drives the LCD backlight PWM
 * line. 0 = off, 100 = full brightness.
 *
 * Out-of-range values are clamped, not rejected.
 */
void hal_backlight_set(int percent);
int  hal_backlight_get(void);

/* ---------- Log ----------------------------------------------------- */

/*
 * log_printf writes a printf-style formatted message to the debug log.
 * On hw, this goes to the dock-connector UART. On sim, to stdout.
 *
 * In a release build (-Drelease=true) this compiles out entirely.
 */
#ifdef CORE_RELEASE
#  define log_printf(...) ((void)0)
#else
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

#endif /* CORE_HAL_H */
