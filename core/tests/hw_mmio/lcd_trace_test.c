/*
 * tests/hw_mmio/lcd_trace_test.c — golden-trace test for the BCM LCD
 * driver (hal/hw/lcd.c), compiled host-side against the recording mock
 * bus (-DMMIO_MOCK).
 *
 * This is the ONLY automated check of the BCM transaction grammar: the
 * clicky emulator models no BCM, and on the device it would take a
 * logic analyzer. It proves:
 *   - lcd_init's host-side port-init sequence and power probe, and that
 *     it touches NO BCM port (0x30xxxxxx) — the dead-BCM / emulator
 *     safety gate the kernel depends on;
 *   - lcd_fill streams exactly (W*H)/2 = 38400 32-bit data words, each
 *     the two-pixel packing (rgb<<16)|rgb (count + endianness), then
 *     the LCD_UPDATE command (0xFFFF0000) and the 0x31 strobe;
 *   - on a non-first frame, the frame streams FIRST, then the
 *     wait-for-idle read handshake runs until BCMA_COMMAND reads idle,
 *     then the command + strobe issue. The wait lives in the commit
 *     (after the stream), not the begin (before it) — the ordering that
 *     fixes the "second present stalls" hang.
 *
 * The lcd_first_frame flag is a file-static that the first fill flips,
 * so the fill cases run first-frame-then-subsequent in main() on
 * purpose.
 */

#include "pp5022.h"
#include "hal.h"
#include "lcd.h"
#include "mmio_mock.h"
#include "trace_expect.h"

#define BCM_WINDOW_LO 0x30000000u
#define BCM_WINDOW_HI 0x30080000u

static int log_touches_bcm(void)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    for (size_t i = 0; i < len; i++) {
        if (log[i].addr >= BCM_WINDOW_LO && log[i].addr < BCM_WINDOW_HI) {
            return 1;
        }
    }
    return 0;
}

/* Host-side port init + power probe. RMW sources read 0; GPO32_VAL is
 * programmed powered so the probe returns 1. Must touch no BCM port. */
static int test_lcd_init_powered(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(GPO32_ENABLE_ADDR,    0x00000000);
    mmio_mock_set_read(GPIOC_ENABLE_ADDR,    0x00000000);
    mmio_mock_set_read(GPIOC_OUTPUT_EN_ADDR, 0x00000000);
    mmio_mock_set_read(GPO32_VAL_ADDR,       GPO32_BCM_POWER);

    int powered = lcd_init();

    trace_cursor tc = trace_begin("lcd_init_powered");
    expect_r(&tc, 32, GPO32_ENABLE_ADDR);
    expect_w(&tc, 32, GPO32_ENABLE_ADDR, 0xC000);          /* power+companion */
    expect_r(&tc, 32, GPIOC_ENABLE_ADDR);
    expect_w(&tc, 32, GPIOC_ENABLE_ADDR, 0x00);            /* clear bit 7 */
    expect_r(&tc, 32, GPIOC_ENABLE_ADDR);
    expect_w(&tc, 32, GPIOC_ENABLE_ADDR, 0x40);            /* set bit 6 (input) */
    expect_r(&tc, 32, GPIOC_OUTPUT_EN_ADDR);
    expect_w(&tc, 32, GPIOC_OUTPUT_EN_ADDR, 0x00);         /* bit 6 not output */
    expect_r(&tc, 32, GPO32_ENABLE_ADDR);
    expect_w(&tc, 32, GPO32_ENABLE_ADDR, 0x00);            /* release GPO bit 0 */
    expect_r(&tc, 32, GPO32_VAL_ADDR);                     /* power probe */
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (!powered) {
        fprintf(stderr, "[lcd_init_powered] expected powered=1\n");
        fails++;
    }
    if (log_touches_bcm()) {
        fprintf(stderr, "[lcd_init_powered] FAIL: touched a BCM port "
                        "(0x30xxxxxx) during init — breaks dead-BCM/"
                        "emulator gate\n");
        fails++;
    }
    return fails;
}

/* Same init, BCM unpowered: probe must return 0. */
static int test_lcd_init_unpowered(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(GPO32_VAL_ADDR, 0x00000000);

    int powered = lcd_init();
    if (powered != 0) {
        fprintf(stderr, "[lcd_init_unpowered] FAIL: expected 0, got %d\n",
                powered);
        return 1;
    }
    printf("[lcd_init_unpowered] PASS\n");
    return 0;
}

/* Expect one bcm_write_addr: 32-bit address store then a single
 * WR_READY poll (BCM_CONTROL programmed ready). */
static void expect_write_addr(trace_cursor *tc, uint32_t addr)
{
    expect_w(tc, 32, BCM_WR_ADDR_ADDR, addr);
    expect_r(tc, 16, BCM_CONTROL_ADDR);
}

/*
 * One wall-clock absorb (bcm_wait_idle_wall): the USEC_TIMER baseline read
 * followed by a single bcm_read32(BCMA_COMMAND) that reads idle, and NO
 * re-kick. Used by the first-frame and post-wake commits.
 */
static void expect_absorb_idle(trace_cursor *tc)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);             /* wall-clock baseline */
    expect_r(tc, 16, BCM_RD_ADDR_ADDR);            /* RD_ADDR_READY poll  */
    expect_w(tc, 32, BCM_RD_ADDR_ADDR, BCMA_COMMAND);
    expect_r(tc, 16, BCM_CONTROL_ADDR);            /* RD_READY poll       */
    expect_r(tc, 32, BCM_DATA_ADDR);               /* status word: idle   */
}

/*
 * First frame. Streams the frame, then ABSORBS.
 *
 * CHANGED: this case used to assert that the first frame skipped the
 * wait-for-idle entirely, on the strength of ipodloader2 finishing its final
 * frame synchronously (02-lcd.md, "Chainload handoff state"). Booting our
 * image directly from the firmware partition as OSOS removes ipodloader2 from
 * the picture and nothing else establishes that guarantee — the Apple ROM is
 * documented to power/init the BCM but never to hand over idle, and its panel
 * init alone can run ~500 ms. So the first commit now runs the same
 * wall-clock-bounded, re-kick-free absorb the post-wake commit uses. On a
 * healthy device (BCM_DATA reads idle, as here) it costs one read handshake.
 */
static int test_lcd_fill_first_frame(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);

    const uint16_t rgb  = 0xF800;                      /* red */
    const uint32_t pair = ((uint32_t)rgb << 16) | rgb; /* 0xF800F800 */
    lcd_fill(rgb);

    trace_cursor tc = trace_begin("lcd_fill_first");
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (uint32_t i = 0; i < (LCD_WIDTH * LCD_HEIGHT) / 2u; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, pair);
    }
    expect_absorb_idle(&tc);
    expect_write_addr(&tc, BCMA_COMMAND);
    expect_w(&tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);   /* 0xFFFF0000 */
    expect_w(&tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE); /* 0x31 */
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* Non-first frame: the frame streams FIRST, then wait-for-idle polls
 * BCMA_COMMAND via bcm_read32 until it reads idle (programmed busy,
 * busy-remnant, then idle) — AFTER the pixel stream, BEFORE the command
 * + strobe. This ordering is the "second present stalls" fix: the wait
 * moved out of bcm_frame_begin and into bcm_frame_commit. */
static int test_lcd_fill_subsequent(void)
{
    mmio_mock_reset();
    /* Both WR_READY (0x02) and RD_READY (0x10) satisfied in one value. */
    mmio_mock_set_read(BCM_CONTROL_ADDR, BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);
    const uint32_t idle_seq[] = { BCMCMD_LCD_UPDATE, 0xFFFF, 0x00000000 };
    mmio_mock_queue_read(BCM_DATA_ADDR, idle_seq, 3);

    const uint16_t rgb  = 0x07E0;                      /* green */
    const uint32_t pair = ((uint32_t)rgb << 16) | rgb; /* 0x07E007E0 */
    lcd_fill(rgb);

    trace_cursor tc = trace_begin("lcd_fill_subsequent");
    /* the stream comes first, identical to the first-frame path */
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (uint32_t i = 0; i < (LCD_WIDTH * LCD_HEIGHT) / 2u; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, pair);
    }
    /* then three wait-for-idle bcm_read32(BCMA_COMMAND) iterations */
    for (int it = 0; it < 3; it++) {
        expect_r(&tc, 16, BCM_RD_ADDR_ADDR);           /* RD_ADDR_READY poll */
        expect_w(&tc, 32, BCM_RD_ADDR_ADDR, BCMA_COMMAND);
        expect_r(&tc, 16, BCM_CONTROL_ADDR);           /* RD_READY poll */
        expect_r(&tc, 32, BCM_DATA_ADDR);              /* status word */
    }
    /* finally the command + strobe */
    expect_write_addr(&tc, BCMA_COMMAND);
    expect_w(&tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);
    expect_w(&tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

int main(void)
{
    int fails = 0;
    fails += test_lcd_init_powered();
    fails += test_lcd_init_unpowered();
    /* order matters: first_frame flips the file-static flag */
    fails += test_lcd_fill_first_frame();
    fails += test_lcd_fill_subsequent();
    return fails == 0 ? 0 : 1;
}
