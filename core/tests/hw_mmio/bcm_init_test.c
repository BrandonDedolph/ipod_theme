/*
 * tests/hw_mmio/bcm_init_test.c — golden-trace test for the BCM bootstrap
 * (bcm_init / lcd_recover in hal/hw/lcd.c), compiled host-side against the
 * recording mock bus (-DMMIO_MOCK) and with -DLCD_RECOVER_ON_WAKE=1 so the
 * post-wake wiring is exercised too (the shipping image defaults it to 0).
 *
 * WHAT THIS PROVES
 *   - the exact ordered register grammar of bcm_init(), step for step against
 *     core/docs/hw/02-lcd.md "Bootstrap and firmware upload" (lines 114-181):
 *     blob lookup, power cycle, strap writes, both alt-control busy/ready
 *     gates, the 8-byte handshake and its 3..7 replay on the alt port, the
 *     read-port sync + write-port dummy reads, the SRAM upload, and the
 *     SDRAM-map / firmware-start handshakes;
 *   - that every byte uploaded to the BCM came from the ROM address the flash
 *     directory named, in order (distinct per-word values make it observable);
 *   - that the upload length is the doc's ((len + 3) >> 1) & ~1 16-bit units,
 *     i.e. 16 32-bit stores for a 64-byte blob;
 *   - that a missing or implausible blob is REPORTED and NOTHING is touched:
 *     no BCM port, no GPO32, no strap register. Uploading garbage and then
 *     starting the BCM's processor on it is the one outcome worse than a
 *     wedged panel;
 *   - that lcd_recover() = port init + bcm_init() + one known full frame, and
 *     that it counts attempts via lcd_bcm_recoveries();
 *   - that the post-wake absorb path calls lcd_recover() when, and only when,
 *     its wall-clock budget is exhausted.
 *
 * WHAT THIS CANNOT PROVE
 *   Any of it against real silicon. No part of this sequence has ever run on
 *   a device — the chainloader has always bootstrapped the BCM for us — so
 *   this is a faithfulness check against the DOC, not against hardware. In
 *   particular the mock cannot tell us whether the flash directory entry
 *   layout assumed by bcm_find_vmcs (tag followed by {offset, length}) is the
 *   real one; the test programs that layout because the driver assumes it.
 *   See the UNVERIFIED block over bcm_find_vmcs in lcd.c.
 *
 * File-static state in lcd.c (lcd_first_frame, lcd_post_wake, lcd_slept) is
 * shared across cases, so main() runs them in a deliberate order.
 */

#include "pp5022.h"
#include "hal.h"
#include "lcd.h"
#include "mmio_mock.h"
#include "trace_expect.h"

#define FRAME_WORDS   ((LCD_WIDTH * LCD_HEIGHT) / 2u)   /* 38400 */

/* Where we plant the vmcs directory entry: third word of the directory, so
 * the scan has to walk past two non-matching words to find it. */
#define DIR_TAG_INDEX   2u
#define DIR_WORD(i)     (FLASH_DIR_BASE + 4u * (uint32_t)(i))

/*
 * The blob the directory points at: a ROM-relative offset (exercising
 * bcm_find_vmcs's relative-vs-absolute branch) and a length that is
 * DELIBERATELY NOT a multiple of 4, so the doc's round-up is observable.
 *
 * 66 bytes -> ((66 + 3) >> 1) & ~1 = 34 16-bit units -> 17 32-bit stores =
 * 68 bytes uploaded. A driver that dropped the "+ 3" would send 16 words and
 * upload a truncated blob; this pins that. (Note the "& ~1" itself cannot be
 * caught by any length: halving discards bit 0 anyway, so it is a no-op once
 * the unit count becomes a word count — it is kept only to mirror the doc.)
 */
#define BLOB_OFF        0x1000u
#define BLOB_ADDR       (FLASH_ROM_BASE + BLOB_OFF)
#define BLOB_LEN        66u
#define BLOB_WORDS      17u

/* Distinct per-word content so the ROM->BCM data path is observable: a word
 * streamed from the wrong address, or out of order, diverges here. */
static uint32_t blob_word(uint32_t k)
{
    return 0xB10B0000u + k;
}

/* lcd.c's BCM_WALL_GUARD_TRIPS under -DMMIO_MOCK. A bcm_wall_delay() whose
 * deadline never arrives (USEC_TIMER is left unprogrammed, so it reads 0
 * forever) therefore costs the baseline read plus this many polls. */
#define MOCK_WALL_TRIPS 4u

/* Program every status register the bootstrap polls so each poll is satisfied
 * on its first read — the healthy-hardware path. */
static void program_healthy_bcm(void)
{
    /* WR_READY for bcm_write_addr, RD_READY for bcm_read32. */
    mmio_mock_set_read(BCM_CONTROL_ADDR,
                       BCM_CONTROL_WR_READY | BCM_CONTROL_RD_READY);
    /* Alt control: busy (0x80) clear, ready (0x40) set. */
    mmio_mock_set_read(BCM_ALT_CONTROL_ADDR, BCM_ALT_CONTROL_READY);
    /* Both read ports ready for the post-handshake sync. */
    mmio_mock_set_read(BCM_RD_ADDR_ADDR,     BCM_RD_ADDR_READY);
    mmio_mock_set_read(BCM_ALT_RD_ADDR_ADDR, BCM_RD_ADDR_READY);
    /* BCM-internal reads: 1 satisfies both "SDRAM mapped" (bit 0) and
     * "firmware started" (nonzero), and reads as idle to a frame commit. */
    mmio_mock_set_read(BCM_DATA_ADDR, 1);
    /* The power rail reads asserted, so the cycle's clear/set are visible. */
    mmio_mock_set_read(GPO32_VAL_ADDR, GPO32_BCM_POWER);
    /* Strap word with bits inside and outside the 0xF00 mask, so the
     * read-modify-write is pinned to clearing exactly that mask. */
    mmio_mock_set_read(STRAP_OPT_A_ADDR, 0x00000F0Fu);
}

/* Plant a valid vmcs directory entry plus the blob it points at. */
static void program_vmcs_blob(uint32_t tag)
{
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX),      tag);
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX + 1u), BLOB_OFF);
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX + 2u), BLOB_LEN);
    for (uint32_t k = 0; k < BLOB_WORDS; k++) {
        mmio_mock_set_read(BLOB_ADDR + 4u * k, blob_word(k));
    }
}

/* ---- expectation helpers --------------------------------------------- */

/* bcm_write_addr: 32-bit address store then one WR_READY poll. */
static void expect_write_addr(trace_cursor *tc, uint32_t addr)
{
    expect_w(tc, 32, BCM_WR_ADDR_ADDR, addr);
    expect_r(tc, 16, BCM_CONTROL_ADDR);
}

/* bcm_write32: address, then the data word on the data port. */
static void expect_write32(trace_cursor *tc, uint32_t addr, uint32_t value)
{
    expect_write_addr(tc, addr);
    expect_w(tc, 32, BCM_DATA_ADDR, value);
}

/* bcm_read32: poll the read port ready, store the address, poll data-ready,
 * read the data port (02-lcd.md, read handshake). */
static void expect_read32(trace_cursor *tc, uint32_t addr)
{
    expect_r(tc, 16, BCM_RD_ADDR_ADDR);
    expect_w(tc, 32, BCM_RD_ADDR_ADDR, addr);
    expect_r(tc, 16, BCM_CONTROL_ADDR);
    expect_r(tc, 32, BCM_DATA_ADDR);
}

/* bcm_wall_delay with an unprogrammed (always-0) USEC_TIMER: the baseline
 * read plus MOCK_WALL_TRIPS polls before the trip guard ends it. */
static void expect_wall_delay(trace_cursor *tc)
{
    for (uint32_t i = 0; i < 1u + MOCK_WALL_TRIPS; i++) {
        expect_r(tc, 32, USEC_TIMER_ADDR);
    }
}

/* bcm_wait16 satisfied on its first read: wall-clock baseline + one poll. */
static void expect_wait16(trace_cursor *tc, uint32_t port)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 16, port);
}

/* bcm_wait_internal satisfied on its first read. */
static void expect_wait_internal(trace_cursor *tc, uint32_t addr)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_read32(tc, addr);
}

/* One wall-clock absorb that finds the BCM idle (bcm_wait_idle_wall). */
static void expect_absorb_idle(trace_cursor *tc)
{
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_read32(tc, BCMA_COMMAND);
}

/* The shared LCD_UPDATE command + 0x31 strobe that ends every present. */
static void expect_command_strobe(trace_cursor *tc)
{
    expect_write32(tc, BCMA_COMMAND, BCMCMD_LCD_UPDATE);
    expect_w(tc, 16, BCM_CONTROL_ADDR, BCM_CONTROL_STROBE);
}

/* The directory scan: DIR_TAG_INDEX misses, the hit, then offset + length. */
static void expect_blob_lookup(trace_cursor *tc)
{
    for (uint32_t i = 0; i <= DIR_TAG_INDEX; i++) {
        expect_r(tc, 32, DIR_WORD(i));
    }
    expect_r(tc, 32, DIR_WORD(DIR_TAG_INDEX + 1u));
    expect_r(tc, 32, DIR_WORD(DIR_TAG_INDEX + 2u));
}

/*
 * The full bcm_init() grammar. Each block cites the 02-lcd.md line range it
 * implements.
 */
static void expect_bcm_init(trace_cursor *tc)
{
    /* Blob located and validated before any hardware is touched (lines
     * 165-171). */
    expect_blob_lookup(tc);

    /* Stage 1 — power (lines 120-125). The rail is CYCLED, not merely
     * OR-ed: our lcd_sleep leaves the BCM powered, so a bare OR would be a
     * no-op against a wedged-but-powered BCM. */
    expect_r(tc, 32, GPO32_VAL_ADDR);
    expect_w(tc, 32, GPO32_VAL_ADDR, 0);         /* rail bit 0x4000 cleared */
    expect_wall_delay(tc);                       /* rail held low          */
    expect_r(tc, 32, GPO32_VAL_ADDR);
    expect_w(tc, 32, GPO32_VAL_ADDR, GPO32_BCM_POWER);
    expect_wall_delay(tc);                       /* the doc's 50 ms settle */

    /* Strap config: clear STRAP_OPT_A bits 0xF00 (0xF0F -> 0x00F, pinning
     * the mask), then drive the boot strap pins with 0x1313. */
    expect_r(tc, 32, STRAP_OPT_A_ADDR);
    expect_w(tc, 32, STRAP_OPT_A_ADDR, 0x0000000Fu);
    expect_w(tc, 32, STRAP_BOOT_PINS_ADDR, STRAP_BOOT_PINS_BCM);

    /* Stage 2 — wait for the BCM (lines 127-129): busy must clear, then
     * ready must set. */
    expect_wait16(tc, BCM_ALT_CONTROL_ADDR);
    expect_wait16(tc, BCM_ALT_CONTROL_ADDR);

    /* The 8-byte handshake (lines 131-136): all 8 to BCM_CONTROL, then
     * bytes 3..7 replayed to BCM_ALT_CONTROL. */
    static const uint8_t seq[8] = {
        0xA1, 0x81, 0x91, 0x02, 0x12, 0x22, 0x72, 0x62
    };
    for (uint32_t i = 0; i < 8u; i++) {
        expect_w(tc, 16, BCM_CONTROL_ADDR, seq[i]);
    }
    for (uint32_t i = 3u; i < 8u; i++) {
        expect_w(tc, 16, BCM_ALT_CONTROL_ADDR, seq[i]);
    }

    /* Post-handshake sync (lines 138-143): both read ports ready, then a
     * dummy read of each write-address port to flush them. */
    expect_r(tc, 32, USEC_TIMER_ADDR);
    expect_r(tc, 16, BCM_RD_ADDR_ADDR);
    expect_r(tc, 16, BCM_ALT_RD_ADDR_ADDR);
    expect_r(tc, 16, BCM_WR_ADDR_ADDR);
    expect_r(tc, 16, BCM_ALT_WR_ADDR_ADDR);

    /* Stage 3 — the same gate again before the upload (lines 146-147). */
    expect_wait16(tc, BCM_ALT_CONTROL_ADDR);
    expect_wait16(tc, BCM_ALT_CONTROL_ADDR);

    /* Upload the blob to BCM SRAM (lines 149-152): point the write port at
     * BCMA_SRAM_BASE, then stream ((len+3)>>1)&~1 16-bit units = 16 32-bit
     * stores, each read from the ROM address the directory named. */
    expect_write_addr(tc, BCMA_SRAM_BASE);
    for (uint32_t k = 0; k < BLOB_WORDS; k++) {
        expect_r(tc, 32, BLOB_ADDR + 4u * k);
        expect_w(tc, 32, BCM_DATA_ADDR, blob_word(k));
    }

    /* Initialize the BCM processor (lines 154-158). */
    expect_write32(tc, BCMA_COMMAND, 0);
    expect_write32(tc, 0x10000C00u, 0xC0000000u);       /* map BCM SDRAM   */
    expect_wait_internal(tc, 0x10000C00u);              /* wait for mapping*/
    expect_write32(tc, 0x10000C00u, 0);

    /* Start firmware execution (lines 160-162): the magic write, then wait
     * for BCMA_COMMAND to read nonzero. */
    expect_write32(tc, 0x10000400u, 0xA5A50002u);
    expect_wait_internal(tc, BCMA_COMMAND);
}

/* lcd_port_init() — the host-side setup lcd_init() also runs (02-lcd.md,
 * "Host-side port init"), with every RMW source reading 0. */
static void expect_port_init(trace_cursor *tc)
{
    expect_r(tc, 32, GPO32_ENABLE_ADDR);
    expect_w(tc, 32, GPO32_ENABLE_ADDR, 0xC000);
    expect_r(tc, 32, GPIOC_ENABLE_ADDR);
    expect_w(tc, 32, GPIOC_ENABLE_ADDR, 0x00);
    expect_r(tc, 32, GPIOC_ENABLE_ADDR);
    expect_w(tc, 32, GPIOC_ENABLE_ADDR, 0x40);
    expect_r(tc, 32, GPIOC_OUTPUT_EN_ADDR);
    expect_w(tc, 32, GPIOC_OUTPUT_EN_ADDR, 0x00);
    expect_r(tc, 32, GPO32_ENABLE_ADDR);
    expect_w(tc, 32, GPO32_ENABLE_ADDR, 0x00);
}

/* The known full frame lcd_recover() presents once the BCM is running: its
 * SDRAM framebuffer did not survive the power cycle, so black beats garbage. */
static void expect_black_frame(trace_cursor *tc)
{
    expect_write_addr(tc, BCMA_CMDPARAM);
    for (uint32_t i = 0; i < FRAME_WORDS; i++) {
        expect_w(tc, 32, BCM_DATA_ADDR, 0);
    }
    expect_absorb_idle(tc);
    expect_command_strobe(tc);
}

/* ---- cases ------------------------------------------------------------ */

/* The bootstrap grammar itself, tag stored big-endian. */
static int test_bcm_init_grammar(void)
{
    mmio_mock_reset();
    program_healthy_bcm();
    program_vmcs_blob(FLASH_ID_VMCS_BE);

    int rc = bcm_init();

    trace_cursor tc = trace_begin("bcm_init_grammar");
    expect_bcm_init(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (rc != LCD_BCM_OK) {
        fprintf(stderr, "[bcm_init_grammar] FAIL: rc = %d, expected %d\n",
                rc, LCD_BCM_OK);
        fails++;
    }
    return fails;
}

/* The doc does not pin the tag's byte order, so the locator accepts either.
 * Same blob, tag stored little-endian: identical grammar and result. */
static int test_bcm_init_tag_byte_order(void)
{
    mmio_mock_reset();
    program_healthy_bcm();
    program_vmcs_blob(FLASH_ID_VMCS_LE);

    int rc = bcm_init();

    trace_cursor tc = trace_begin("bcm_init_tag_le");
    expect_bcm_init(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (rc != LCD_BCM_OK) {
        fprintf(stderr, "[bcm_init_tag_le] FAIL: rc = %d\n", rc);
        fails++;
    }
    return fails;
}

/* Nothing outside the flash directory may be touched — no BCM port, no
 * GPO32, no strap register. Used by both blob-failure cases. */
static int only_read_the_directory(const char *label)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    const uint32_t dir_hi = FLASH_DIR_BASE + FLASH_DIR_BYTES;

    for (size_t i = 0; i < len; i++) {
        int in_dir = log[i].addr >= FLASH_DIR_BASE && log[i].addr < dir_hi;
        if (log[i].op != MMIO_OP_READ || !in_dir) {
            fprintf(stderr,
                    "[%s] FAIL: touched %s%d @%08X — a blob we cannot "
                    "validate must never reach the hardware\n",
                    label, log[i].op == MMIO_OP_READ ? "R" : "W",
                    log[i].width, log[i].addr);
            return 1;
        }
    }
    printf("[%s] PASS (%zu directory reads, no hardware touched)\n",
           label, len);
    return 0;
}

/* No vmcs tag anywhere in the directory: scan the whole region, report
 * LCD_BCM_ERR_NO_BLOB, touch nothing. */
static int test_bcm_init_no_blob(void)
{
    mmio_mock_reset();
    program_healthy_bcm();          /* hardware is fine; the blob is not */

    int rc = bcm_init();
    int fails = 0;

    if (rc != LCD_BCM_ERR_NO_BLOB) {
        fprintf(stderr, "[bcm_init_no_blob] FAIL: rc = %d, expected %d\n",
                rc, LCD_BCM_ERR_NO_BLOB);
        fails++;
    }
    fails += only_read_the_directory("bcm_init_no_blob");
    return fails;
}

/*
 * Tag present but the entry is not credible (zero length). The scan treats it
 * as a false-positive tag, keeps looking, finds nothing, and reports
 * LCD_BCM_ERR_BAD_BLOB — distinguishable from "no tag at all", which is what
 * a debug screen needs to tell "wrong directory layout" from "wrong ROM".
 */
static int test_bcm_init_bad_blob(void)
{
    mmio_mock_reset();
    program_healthy_bcm();
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX),      FLASH_ID_VMCS_BE);
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX + 1u), BLOB_OFF);
    mmio_mock_set_read(DIR_WORD(DIR_TAG_INDEX + 2u), 0);   /* zero length */

    int rc = bcm_init();
    int fails = 0;

    if (rc != LCD_BCM_ERR_BAD_BLOB) {
        fprintf(stderr, "[bcm_init_bad_blob] FAIL: rc = %d, expected %d\n",
                rc, LCD_BCM_ERR_BAD_BLOB);
        fails++;
    }
    fails += only_read_the_directory("bcm_init_bad_blob");
    return fails;
}

/*
 * THE OTHER HALF OF THE WIRING: a BOOT-TIME absorb that times out must NOT
 * re-bootstrap.
 *
 * The first frame now absorbs (we no longer assume a chainloader left the BCM
 * idle), so it can exhaust its budget just like the post-wake commit can. But
 * at frame one the panel has never been shown to work at all — a timeout there
 * far more likely means "this BCM was never alive" (emulator, dead unit) than
 * "a working BCM wedged", and power-cycling it would trade a diagnosable boot
 * for an unexplained dark screen. So the commit reports, proceeds with the
 * command + strobe as it always has, and leaves the recovery counter alone.
 *
 * Must run BEFORE any case that presents a frame — lcd_first_frame is a
 * file-static that the first present clears.
 */
static int test_boot_absorb_does_not_recover(void)
{
    uint32_t before = lcd_bcm_recoveries();
    static uint16_t fb[LCD_WIDTH * LCD_HEIGHT];

    mmio_mock_reset();
    program_healthy_bcm();
    program_vmcs_blob(FLASH_ID_VMCS_BE);         /* a blob IS available... */
    mmio_mock_set_read(BCM_DATA_ADDR, BCMCMD_LCD_UPDATE);   /* ...never idle */

    lcd_present_rect(fb, 0, 0, 2, 1);

    trace_cursor tc = trace_begin("boot_absorb_no_recover");
    expect_write_addr(&tc, BCMA_CMDPARAM);
    expect_w(&tc, 32, BCM_DATA_ADDR, 0);
    /* the absorb, run to its guard and never satisfied */
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    for (int trip = 0; trip < 4; trip++) {
        if (trip > 0) {
            expect_r(&tc, 32, USEC_TIMER_ADDR);
        }
        expect_read32(&tc, BCMA_COMMAND);
    }
    /* ...and then the ordinary command + strobe: no port init, no strap
     * writes, no power cycle anywhere in this trace. */
    expect_command_strobe(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (lcd_bcm_recoveries() != before) {
        fprintf(stderr, "[boot_absorb_no_recover] FAIL: recoveries %u -> %u, "
                        "expected no change — a boot-time timeout must not "
                        "power-cycle the BCM\n", before, lcd_bcm_recoveries());
        fails++;
    }
    return fails;
}

/* lcd_recover() = port init + bcm_init() + one known full frame, and it
 * counts the attempt. */
static int test_lcd_recover(void)
{
    uint32_t before = lcd_bcm_recoveries();

    mmio_mock_reset();
    program_healthy_bcm();
    program_vmcs_blob(FLASH_ID_VMCS_BE);

    int rc = lcd_recover();

    trace_cursor tc = trace_begin("lcd_recover");
    expect_port_init(&tc);
    expect_bcm_init(&tc);
    expect_black_frame(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (rc != LCD_BCM_OK) {
        fprintf(stderr, "[lcd_recover] FAIL: rc = %d\n", rc);
        fails++;
    }
    if (lcd_bcm_recoveries() != before + 1u) {
        fprintf(stderr, "[lcd_recover] FAIL: recoveries %u -> %u, "
                        "expected +1\n", before, lcd_bcm_recoveries());
        fails++;
    }
    return fails;
}

/*
 * THE WIRING. A post-wake commit whose wall-clock absorb never sees the BCM
 * go idle — exactly the state that latches the panel solid white until a
 * reboot — must fall through to lcd_recover(), and must NOT then issue its
 * own command + strobe on top of the frame lcd_recover already presented.
 *
 * Only reachable because this binary is built with -DLCD_RECOVER_ON_WAKE=1;
 * the shipping image compiles this branch out.
 */
static int test_recover_on_wake(void)
{
    uint32_t before = lcd_bcm_recoveries();
    static uint16_t fb[LCD_WIDTH * LCD_HEIGHT];

    /* Arm lcd_post_wake deterministically via the real sleep/wake pair, in a
     * scratch window we then discard — this case is about the commit, not
     * about sleep's grammar. */
    mmio_mock_reset();
    program_healthy_bcm();
    mmio_mock_set_read(BCM_DATA_ADDR, 0);        /* idle: sleep drains fast */
    lcd_sleep();
    lcd_wake();

    mmio_mock_reset();
    program_healthy_bcm();
    program_vmcs_blob(FLASH_ID_VMCS_BE);
    /*
     * BCMA_COMMAND reads busy for the four bcm_read32 trips the mock's wall
     * guard allows, so the absorb exhausts its budget; every read after that
     * returns 1, which satisfies the bootstrap's two internal waits and reads
     * idle to the frame lcd_recover presents.
     */
    const uint32_t stuck[] = { BCMCMD_LCD_UPDATE, BCMCMD_LCD_UPDATE,
                               BCMCMD_LCD_UPDATE, BCMCMD_LCD_UPDATE, 1 };
    mmio_mock_queue_read(BCM_DATA_ADDR, stuck, 5);

    /* A 2x1 rect keeps the triggering present's own pixel stream to a single
     * word, so the log has room for the whole recovery. */
    lcd_present_rect(fb, 0, 0, 2, 1);

    trace_cursor tc = trace_begin("recover_on_wake");
    /* the caller's stream */
    expect_write_addr(&tc, BCMA_CMDPARAM);
    expect_w(&tc, 32, BCM_DATA_ADDR, 0);
    /* the absorb, run to its guard: baseline read, then four read handshakes
     * with a deadline check between each */
    expect_r(&tc, 32, USEC_TIMER_ADDR);
    for (int trip = 0; trip < 4; trip++) {
        if (trip > 0) {
            expect_r(&tc, 32, USEC_TIMER_ADDR);   /* deadline check */
        }
        expect_read32(&tc, BCMA_COMMAND);
    }
    /* ... which never saw idle, so: re-bootstrap, and NO command + strobe
     * from this commit — lcd_recover's frame is the only one issued. */
    expect_port_init(&tc);
    expect_bcm_init(&tc);
    expect_black_frame(&tc);
    trace_expect_end(&tc);

    int fails = trace_done(&tc);
    if (lcd_bcm_recoveries() != before + 1u) {
        fprintf(stderr, "[recover_on_wake] FAIL: recoveries %u -> %u, "
                        "expected +1\n", before, lcd_bcm_recoveries());
        fails++;
    }
    if (mmio_mock_dropped() != 0) {
        fprintf(stderr, "[recover_on_wake] FAIL: %zu events dropped — the "
                        "asserted grammar is only a prefix\n",
                mmio_mock_dropped());
        fails++;
    }
    return fails;
}

int main(void)
{
    int fails = 0;
    /* Order matters: lcd_recover presents a frame, which flips lcd.c's
     * file-static lcd_first_frame, and it leaves lcd_post_wake armed. */
    fails += test_bcm_init_grammar();
    fails += test_bcm_init_tag_byte_order();
    fails += test_bcm_init_no_blob();
    fails += test_bcm_init_bad_blob();
    fails += test_boot_absorb_does_not_recover();   /* consumes lcd_first_frame */
    fails += test_lcd_recover();
    fails += test_recover_on_wake();
    return fails == 0 ? 0 : 1;
}
