/*
 * tests/hw_mmio/lcd_present_test.c — golden-trace test for
 * lcd_present_fb() in hal/hw/lcd.c, compiled host-side against the
 * recording mock bus (-DMMIO_MOCK). Mirrors lcd_trace_test.c.
 *
 * lcd_present_fb streams an arbitrary host framebuffer through the same
 * BCM full-frame fast path lcd_fill uses. This test proves:
 *   - the exact BCM grammar: point the write port at BCMA_CMDPARAM
 *     (write-addr + WR_READY poll), stream EXACTLY (W*H)/2 = 38400
 *     32-bit data words, then the LCD_UPDATE command (0xFFFF0000) and
 *     the 0x31 strobe;
 *   - the two-pixel packing: each 32-bit word carries two consecutive
 *     framebuffer pixels with fb[2*i] (even x, first push) in the LOW
 *     half and fb[2*i+1] (odd x, second push) in the HIGH half — the
 *     ordering that collapses to lcd_fill's (rgb<<16)|rgb for a solid
 *     frame. A distinct per-pixel pattern (fb[i] = i & 0xFFFF) makes
 *     the two halves and their order observable.
 *
 * lcd_first_frame is a file-static shared with lcd_fill; this binary
 * calls lcd_present_fb first, so the first case hits the first-frame
 * path, exactly like lcd_trace_test orders its fill cases.
 *
 * NOTE (changed): the first frame no longer SKIPS the wait-for-idle. It
 * used to, on the strength of ipodloader2's synchronous final frame
 * (02-lcd.md, "Chainload handoff state"). Booting directly out of the
 * firmware partition as OSOS removes ipodloader2, and nothing else
 * establishes that idle guarantee — so the first frame now runs the
 * wall-clock absorb (single bcm_read32, no re-kick) that the post-wake
 * path uses. That is the one existing expectation this file changes.
 */

#include "pp5022.h"
#include "hal.h"
#include "lcd.h"
#include "mmio_mock.h"
#include "trace_expect.h"

#define FRAME_PIXELS  (LCD_WIDTH * LCD_HEIGHT)
#define FRAME_WORDS   ((LCD_WIDTH * LCD_HEIGHT) / 2u)   /* 38400 */

/* Large enough to keep off the stack (153,600 bytes). */
static uint16_t g_fb[FRAME_PIXELS];

/* Distinct per-pixel pattern so the packing/half-order is observable:
 * every pixel differs from its neighbour and from the other half. */
static void fill_pattern(void)
{
    for (unsigned i = 0; i < FRAME_PIXELS; i++) {
        g_fb[i] = (uint16_t)(i & 0xFFFF);
    }
}

/* The word lcd_present_fb must emit for pixel-pair i: high half = odd
 * pixel fb[2*i+1], low half = even pixel fb[2*i]. */
static uint32_t expected_pair(unsigned i)
{
    return ((uint32_t)g_fb[2u * i + 1u] << 16) | g_fb[2u * i];
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
 * followed by a single bcm_read32(BCMA_COMMAND) that reads idle. No re-kick
 * — that is the whole point of the absorb. Used by the first-frame and
 * post-wake commits.
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
 * First frame. Streams the frame, then ABSORBS: we no longer assume the
 * previous stage left the BCM idle (see this file's header note), so the
 * commit does one wall-clock-bounded, re-kick-free idle read before issuing
 * the command. BCM_DATA is left unprogrammed so that read returns 0 = idle,
 * i.e. the healthy case costs exactly one read handshake.
 */
static int test_lcd_present_first_frame(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);

    fill_pattern();
    lcd_present_fb(g_fb);

    /* Golden trace: this asserts the exact grammar AND the exact count
     * (trace_expect_end fails on any missing/extra event). */
    trace_cursor tc = trace_begin("lcd_present_first");
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (unsigned i = 0; i < FRAME_WORDS; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, expected_pair(i));
    }
    expect_absorb_idle(&tc);
    expect_write_addr(&tc, BCMA_COMMAND);
    expect_w(&tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);      /* 0xFFFF0000 */
    expect_w(&tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);  /* 0x31 */
    trace_expect_end(&tc);

    int fails = trace_done(&tc);

    /* Explicit exact-count check: writes to BCM_DATA are the 38400
     * pixel words plus the single BCMA_COMMAND word (bcm_write32). */
    size_t data_writes = mmio_mock_count(MMIO_OP_WRITE, BCM_DATA_ADDR);
    size_t pixel_writes = data_writes - 1u;   /* minus the command word */
    if (pixel_writes != FRAME_WORDS) {
        fprintf(stderr,
                "[lcd_present_first] FAIL: %zu pixel data writes, "
                "expected %u\n", pixel_writes, FRAME_WORDS);
        fails++;
    } else {
        printf("[lcd_present_first] pixel data writes = %zu (== %u)\n",
               pixel_writes, FRAME_WORDS);
    }

    /* Explicit packing check on the very first streamed word: it must
     * be (fb[1] << 16) | fb[0] = 0x00010000 for this pattern — proving
     * fb[0] (even) landed in the LOW half and fb[1] (odd) in the HIGH
     * half, matching how lcd_fill would place two identical pixels. */
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    uint32_t first_data = 0;
    int found = 0;
    for (size_t k = 0; k < len; k++) {
        if (log[k].op == MMIO_OP_WRITE && log[k].width == 32 &&
            log[k].addr == BCM_DATA_ADDR) {
            first_data = log[k].value;
            found = 1;
            break;
        }
    }
    uint32_t want = ((uint32_t)g_fb[1] << 16) | g_fb[0];   /* 0x00010000 */
    if (!found || first_data != want) {
        fprintf(stderr,
                "[lcd_present_first] FAIL: first data word = %08X, "
                "expected %08X (fb[1]<<16 | fb[0])\n",
                found ? first_data : 0u, want);
        fails++;
    } else {
        printf("[lcd_present_first] first word %08X = fb[1]<<16 | fb[0] "
               "(low=even, high=odd)\n", first_data);
    }

    return fails;
}

/* Non-first frame: the frame streams FIRST, then wait-for-idle polls
 * BCMA_COMMAND via bcm_read32 until it reads idle (programmed busy,
 * busy-remnant, then idle) — AFTER the pixel stream, BEFORE the command
 * + strobe. This ordering is the "second present stalls" fix: the wait
 * moved out of bcm_frame_begin and into bcm_frame_commit. */
static int test_lcd_present_subsequent(void)
{
    mmio_mock_reset();
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);
    const uint32_t idle_seq[] = { BCMCMD_LCD_UPDATE, 0xFFFF, 0x00000000 };
    mmio_mock_queue_read(BCM_DATA_ADDR, idle_seq, 3);

    fill_pattern();
    lcd_present_fb(g_fb);

    trace_cursor tc = trace_begin("lcd_present_subsequent");
    /* the stream comes first, identical to the first-frame path */
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (unsigned i = 0; i < FRAME_WORDS; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, expected_pair(i));
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

/* Program the mock so a non-first-frame commit's wait-for-idle poll
 * (bcm_read32(BCMA_COMMAND)) sees busy, busy-remnant, then idle. */
static void program_idle_seq(void)
{
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    mmio_mock_set_read(BCM_RD_ADDR_ADDR, BCM_RD_ADDR_READY);
    const uint32_t idle_seq[] = { BCMCMD_LCD_UPDATE, 0xFFFF, 0x00000000 };
    mmio_mock_queue_read(BCM_DATA_ADDR, idle_seq, 3);
}

/* Emit the three wait-for-idle bcm_read32(BCMA_COMMAND) iterations that
 * bcm_frame_commit runs (after the stream) on a non-first frame. */
static void expect_wait_for_idle(trace_cursor *tc)
{
    for (int it = 0; it < 3; it++) {
        expect_r(tc, 16, BCM_RD_ADDR_ADDR);            /* RD_ADDR_READY poll */
        expect_w(tc, 32, BCM_RD_ADDR_ADDR, BCMA_COMMAND);
        expect_r(tc, 16, BCM_CONTROL_ADDR);            /* RD_READY poll */
        expect_r(tc, 32, BCM_DATA_ADDR);               /* status word */
    }
}

/* Emit the shared LCD_UPDATE command + strobe that ends every present. */
static void expect_command_strobe(trace_cursor *tc)
{
    expect_write_addr(tc, BCMA_COMMAND);
    expect_w(tc, 32, BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);
    expect_w(tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
}

/*
 * Full-width, partial-height rect (x=0, y=8, w=320, h=16) — the row-band
 * case our UI hits most. Because it is full-width the rows are contiguous
 * in BCM memory, so the grammar is: ONE write-addr at BCMA_CMDPARAM +
 * y*stride, then exactly w*h/2 = 2560 packed words read at the RIGHT
 * stride (source row y+r), then the shared idle-wait + command + strobe.
 *
 * Mutation guard: the expected words are computed from fb[(y+r)*W + c],
 * so an implementation that ignored y (wrong stride / wrong source
 * offset) or streamed the wrong pixel count would diverge from this
 * golden trace and fail. An explicit first-word check pins the offset.
 */
static int test_lcd_present_rect_band(void)
{
    const int X = 0, Y = 8, W = 320, H = 16;
    const uint32_t stride_bytes = (uint32_t)LCD_WIDTH * 2u;
    const uint32_t rect_words = (uint32_t)(W * H) / 2u;   /* 2560 */

    mmio_mock_reset();
    program_idle_seq();
    fill_pattern();
    lcd_present_rect(g_fb, X, Y, W, H);

    trace_cursor tc = trace_begin("lcd_present_rect_band");
    /* single contiguous stream starting at row Y */
    expect_write_addr(&tc, BCMA_CMDPARAM + (uint32_t)Y * stride_bytes);
    for (uint32_t k = 0; k < rect_words; k++) {
        const uint16_t *src = &g_fb[(uint32_t)Y * LCD_WIDTH];
        uint32_t word = ((uint32_t)src[2u * k + 1u] << 16) | src[2u * k];
        expect_w(&tc, 32, BCM_DATA_ADDR, word);
    }
    expect_wait_for_idle(&tc);
    expect_command_strobe(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);

    /* Explicit stride pin on the first streamed word: must be
     * (fb[Y*W+1] << 16) | fb[Y*W], NOT (fb[1]<<16)|fb[0] — the latter is
     * what a stride-ignoring bug would emit. */
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    uint32_t first_data = 0;
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].width == 32 &&
            log[i].addr == BCM_DATA_ADDR) {
            first_data = log[i].value; found = 1; break;
        }
    }
    uint32_t want = ((uint32_t)g_fb[(uint32_t)Y * LCD_WIDTH + 1u] << 16) |
                    g_fb[(uint32_t)Y * LCD_WIDTH];
    if (!found || first_data != want) {
        fprintf(stderr,
                "[lcd_present_rect_band] FAIL: first word = %08X, "
                "expected %08X (fb[Y*W+1]<<16 | fb[Y*W])\n",
                found ? first_data : 0u, want);
        fails++;
    }

    /* Explicit pixel-count pin: data writes = rect words + 1 command. */
    size_t pixel_writes = mmio_mock_count(MMIO_OP_WRITE, BCM_DATA_ADDR) - 1u;
    if (pixel_writes != rect_words) {
        fprintf(stderr,
                "[lcd_present_rect_band] FAIL: %zu pixel writes, "
                "expected %u\n", pixel_writes, rect_words);
        fails++;
    } else {
        printf("[lcd_present_rect_band] %u pixel words at row-%d stride "
               "(w*h/2)\n", rect_words, Y);
    }
    return fails;
}

/*
 * Arbitrary (narrower-than-full) rect (x=4, y=8, w=8, h=4). Both x and w
 * are already even, so no rounding. Grammar: per row, a write-addr at
 * BCMA_CMDPARAM + (y+r)*stride + x*2, then w/2 = 4 words read at the
 * per-row source offset (y+r)*W + x — proving BOTH the destination
 * per-row re-address (the inter-row gap) AND the source stride.
 */
static int test_lcd_present_rect_arbitrary(void)
{
    const int X = 4, Y = 8, W = 8, H = 4;
    const uint32_t stride_bytes = (uint32_t)LCD_WIDTH * 2u;
    const uint32_t row_words = (uint32_t)W / 2u;          /* 4 */

    mmio_mock_reset();
    program_idle_seq();
    fill_pattern();
    lcd_present_rect(g_fb, X, Y, W, H);

    trace_cursor tc = trace_begin("lcd_present_rect_arbitrary");
    for (int r = 0; r < H; r++) {
        uint32_t row = (uint32_t)(Y + r);
        expect_write_addr(&tc,
            BCMA_CMDPARAM + row * stride_bytes + (uint32_t)X * 2u);
        const uint16_t *src = &g_fb[row * (uint32_t)LCD_WIDTH + (uint32_t)X];
        for (uint32_t k = 0; k < row_words; k++) {
            uint32_t word = ((uint32_t)src[2u * k + 1u] << 16) | src[2u * k];
            expect_w(&tc, 32, BCM_DATA_ADDR, word);
        }
    }
    expect_wait_for_idle(&tc);
    expect_command_strobe(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);

    /* Pixel-count pin: 4 rows * 4 words + 1 command word. */
    size_t pixel_writes = mmio_mock_count(MMIO_OP_WRITE, BCM_DATA_ADDR) - 1u;
    if (pixel_writes != row_words * (uint32_t)H) {
        fprintf(stderr,
                "[lcd_present_rect_arbitrary] FAIL: %zu pixel writes, "
                "expected %u\n", pixel_writes, row_words * (uint32_t)H);
        fails++;
    }
    return fails;
}

/*
 * A full-frame rect must reproduce the exact full-frame grammar — this is
 * the guarantee that lets lcd_present_fb delegate to lcd_present_rect.
 * Same golden trace as test_lcd_present_subsequent, but driven through
 * the rect entry point.
 */
static int test_lcd_present_rect_fullframe(void)
{
    mmio_mock_reset();
    program_idle_seq();
    fill_pattern();
    lcd_present_rect(g_fb, 0, 0, LCD_WIDTH, LCD_HEIGHT);

    trace_cursor tc = trace_begin("lcd_present_rect_fullframe");
    expect_write_addr(&tc, BCMA_CMDPARAM);
    for (unsigned i = 0; i < FRAME_WORDS; i++) {
        expect_w(&tc, 32, BCM_DATA_ADDR, expected_pair(i));
    }
    expect_wait_for_idle(&tc);
    expect_command_strobe(&tc);
    trace_expect_end(&tc);
    return trace_done(&tc);
}

/* A zero-area / fully out-of-bounds rect must be a pure no-op: no BCM
 * traffic at all (and it must NOT flip lcd_first_frame). */
static int test_lcd_present_rect_noop(void)
{
    int fails = 0;

    mmio_mock_reset();
    program_idle_seq();
    lcd_present_rect(g_fb, 10, 10, 0, 0);        /* zero area */
    lcd_present_rect(g_fb, 400, 10, 20, 20);     /* fully right of panel */
    lcd_present_rect(g_fb, -40, 10, 20, 20);     /* fully left of panel */
    lcd_present_rect(g_fb, 10, 300, 20, 20);     /* fully below panel */

    if (mmio_mock_log_len() != 0) {
        fprintf(stderr,
                "[lcd_present_rect_noop] FAIL: %zu bus events, expected 0\n",
                mmio_mock_log_len());
        fails++;
    } else {
        printf("[lcd_present_rect_noop] PASS (no BCM traffic)\n");
    }
    return fails;
}

int main(void)
{
    int fails = 0;
    /* order matters: the first present flips the file-static
     * lcd_first_frame flag, so the first-frame case must run first. All
     * cases after it are non-first-frame (idle-seq programmed). */
    fails += test_lcd_present_first_frame();
    fails += test_lcd_present_subsequent();
    fails += test_lcd_present_rect_band();
    fails += test_lcd_present_rect_arbitrary();
    fails += test_lcd_present_rect_fullframe();
    fails += test_lcd_present_rect_noop();
    return fails == 0 ? 0 : 1;
}
