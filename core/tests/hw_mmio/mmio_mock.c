/*
 * tests/hw_mmio/mmio_mock.c — implementation of the recording fake bus.
 *
 * Provides the six mmio_{read,write}{8,16,32} entry points that the
 * freestanding drivers call when compiled with -DMMIO_MOCK, plus the
 * mmio_mock_* control surface from mmio_mock.h.
 *
 * Deliberately simple and allocation-free: fixed-capacity arrays sized
 * well past any single driver call. Overrunning the read-map still aborts
 * loudly (it means a test is mis-programmed), but overrunning the EVENT log
 * saturates and counts the excess via mmio_mock_dropped() — a driver spinning
 * to its bounded limit on a deliberately wedged bus is legitimate behaviour,
 * and killing the binary there would destroy every result already produced.
 */

#include "mmio_mock.h"

#include <stdio.h>
#include <stdlib.h>

/* A full lcd_fill emits 320*240/2 = 38400 data writes plus a handful of
 * handshake accesses, so the log must comfortably exceed that. */
#define MMIO_LOG_CAP     65536
#define MMIO_READMAP_CAP 64
#define MMIO_SEQ_CAP     32

static mmio_event g_log[MMIO_LOG_CAP];
static size_t     g_log_len;
static size_t     g_log_dropped;

typedef struct {
    uint32_t addr;
    uint32_t seq[MMIO_SEQ_CAP];
    size_t   seq_len;    /* number of programmed values (>=1 if used) */
    size_t   consumed;   /* how many reads have been served so far    */
    int      in_use;
} read_entry;

static read_entry g_reads[MMIO_READMAP_CAP];

/* Post-access hook; see mmio_mock.h. NULL when unused, which is the norm. */
static mmio_mock_hook_fn g_hook;

void mmio_mock_set_hook(mmio_mock_hook_fn fn)
{
    g_hook = fn;
}

void mmio_mock_reset(void)
{
    g_log_len = 0;
    g_log_dropped = 0;
    g_hook    = NULL;
    for (size_t i = 0; i < MMIO_READMAP_CAP; i++) {
        g_reads[i].in_use = 0;
    }
}

static read_entry *find_read(uint32_t addr)
{
    for (size_t i = 0; i < MMIO_READMAP_CAP; i++) {
        if (g_reads[i].in_use && g_reads[i].addr == addr) {
            return &g_reads[i];
        }
    }
    return NULL;
}

static read_entry *alloc_read(uint32_t addr)
{
    read_entry *e = find_read(addr);
    if (e) {
        return e;
    }
    for (size_t i = 0; i < MMIO_READMAP_CAP; i++) {
        if (!g_reads[i].in_use) {
            g_reads[i].in_use   = 1;
            g_reads[i].addr     = addr;
            g_reads[i].seq_len  = 0;
            g_reads[i].consumed = 0;
            return &g_reads[i];
        }
    }
    fprintf(stderr, "mmio_mock: read-map capacity (%d) exhausted\n",
            MMIO_READMAP_CAP);
    abort();
}

void mmio_mock_set_read(uint32_t addr, uint32_t value)
{
    read_entry *e = alloc_read(addr);
    e->seq[0]   = value;
    e->seq_len  = 1;
    e->consumed = 0;
}

void mmio_mock_queue_read(uint32_t addr, const uint32_t *seq, size_t n)
{
    if (n == 0 || n > MMIO_SEQ_CAP) {
        fprintf(stderr, "mmio_mock: bad queue length %zu (cap %d)\n",
                n, MMIO_SEQ_CAP);
        abort();
    }
    read_entry *e = alloc_read(addr);
    for (size_t i = 0; i < n; i++) {
        e->seq[i] = seq[i];
    }
    e->seq_len  = n;
    e->consumed = 0;
}

/*
 * IRAM is memory, not a peripheral.
 *
 * This mock models the PERIPHERAL bus: the log is a register grammar, and every
 * trace test asserts an exact ordered sequence over it. power_standby() clears
 * ~112 KB of IRAM before sleeping (so Apple's OF doesn't take the boot-from-
 * sleep path on wake) and it does that through mmio_write32, because a raw
 * store to 0x4000C000 is a wild pointer on the host. Those 16k stores are not
 * register accesses and must not appear in a grammar — left in, they exhaust
 * the log and bury the two I2C writes the test exists to check.
 *
 * Stores here are accepted and discarded; reads fall through to the normal
 * programmed-read path, so nothing else changes.
 */
#define IRAM_LO 0x40000000u
#define IRAM_HI 0x40020000u

static int is_ram(uint32_t addr)
{
    return addr >= IRAM_LO && addr < IRAM_HI;
}

static void record(mmio_op op, int width, uint32_t addr, uint32_t value)
{
    if (op == MMIO_OP_WRITE && is_ram(addr)) {
        return;
    }
    /*
     * Saturate, don't abort.
     *
     * A driver that spins to its bounded limit on a deliberately wedged bus
     * emits far more polling reads than any grammar cares about — power_standby
     * against a permanently-BUSY controller does I2C_BUSY_SPIN_LIMIT (65536)
     * reads per attempt, which fills this log on its own. Aborting there kills
     * the whole test binary and destroys every result it had already produced,
     * turning "the driver spun a lot, exactly as designed" into a dead suite.
     * Record what fits, count the rest, and let trace_expect_end() decide
     * whether truncation actually invalidates what it was checking.
     */
    if (g_log_len >= MMIO_LOG_CAP) {
        g_log_dropped++;
        return;
    }
    g_log[g_log_len].op    = op;
    g_log[g_log_len].width = width;
    g_log[g_log_len].addr  = addr;
    g_log[g_log_len].value = value;
    g_log_len++;
    if (g_hook != NULL) {
        g_hook(&g_log[g_log_len - 1]);
    }
}

static uint32_t serve_read(uint32_t addr)
{
    read_entry *e = find_read(addr);
    if (!e || e->seq_len == 0) {
        return 0;   /* unprogrammed: default 0 */
    }
    size_t idx = e->consumed < e->seq_len ? e->consumed : e->seq_len - 1;
    e->consumed++;
    return e->seq[idx];
}

/* ---- the accessor entry points the drivers link against ---- */

uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v = serve_read((uint32_t)addr);
    record(MMIO_OP_READ, 32, (uint32_t)addr, v);
    return v;
}

uint16_t mmio_read16(uintptr_t addr)
{
    uint16_t v = (uint16_t)serve_read((uint32_t)addr);
    record(MMIO_OP_READ, 16, (uint32_t)addr, v);
    return v;
}

uint8_t mmio_read8(uintptr_t addr)
{
    uint8_t v = (uint8_t)serve_read((uint32_t)addr);
    record(MMIO_OP_READ, 8, (uint32_t)addr, v);
    return v;
}

void mmio_write32(uintptr_t addr, uint32_t value)
{
    record(MMIO_OP_WRITE, 32, (uint32_t)addr, value);
}

void mmio_write16(uintptr_t addr, uint16_t value)
{
    record(MMIO_OP_WRITE, 16, (uint32_t)addr, value);
}

void mmio_write8(uintptr_t addr, uint8_t value)
{
    record(MMIO_OP_WRITE, 8, (uint32_t)addr, value);
}

const mmio_event *mmio_mock_log(void)
{
    return g_log;
}

size_t mmio_mock_log_len(void)
{
    return g_log_len;
}

size_t mmio_mock_count(mmio_op op, uint32_t addr)
{
    size_t c = 0;
    for (size_t i = 0; i < g_log_len; i++) {
        if (g_log[i].op == op && g_log[i].addr == addr) {
            c++;
        }
    }
    return c;
}

/* Events discarded after the log filled (see record()). Nonzero means the
 * recorded grammar is a prefix of what actually happened. */
size_t mmio_mock_dropped(void)
{
    return g_log_dropped;
}
