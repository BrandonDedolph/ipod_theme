/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/hw_mmio/power_trace_test.c — golden-trace test for the PCF50605
 * deep-sleep write (hal/hw/power.c), host-side under -DMMIO_MOCK.
 *
 * WHY THIS ONE MATTERS MORE THAN THE OTHERS. power_standby() is the single
 * most dangerous write in the firmware: it latches the PMU's OOCC1 register
 * with GOSTDBY, and the driver's own comment says "never issue GOSTDBY without
 * a wake bit". Get that byte wrong — wake bits dropped, wrong register, wrong
 * device address, wrong byte order — and the PMU cuts power with no way to
 * turn it back on. Recovery is disassembling the iPod. battery.c and volume.c,
 * which can at worst misreport a percentage, both had full trace tests over
 * this same i2c_send path; the one write that can brick the device had none.
 *
 * The values are hand-derived from core/docs/hw/06-power.md and 09-i2c.md (via
 * power.c's own register defines), never from Rockbox source.
 *
 * ESCAPING THE POINT OF NO RETURN. power_standby() ends in a deliberate
 * `for (;;)` — on the device the PMU cuts power a few milliseconds later, so
 * returning would mean running on with a half-torn-down system. That makes it
 * uncallable from a plain host test. The mock bus's post-access hook (added
 * for exactly this) fires on each recorded access; once the transaction's
 * final strobe lands we longjmp back out, leaving the recorded trace intact.
 *
 * This asserts the TRANSACTION CONTENT, not how many times a retry loop runs:
 * a bounded retry is being added to this function, and the number of attempts
 * is a tuning decision. What must never change is the bytes.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>

#include "pp5022.h"
#include "i2c.h"
#include "power.h"
#include "mmio_mock.h"
#include "trace_expect.h"
#include "../xfail.h"

/* Mirrors of power.c's private defines — restated here rather than included so
 * that a silent edit to power.c shows up as a test failure, which is the whole
 * point of a golden trace. Source: core/docs/hw/06-power.md, "Standby/sleep".
 * PCF50605 control port is 7-bit device 0x08 (the same device the battery
 * gauge reads); OOCC1 is register 0x08. */
#define PMU_ADDR       0x08
#define PMU_OOCC1      0x08
#define OOCC1_GOSTDBY  0x01   /* latching transition to standby            */
#define OOCC1_CHGWAK   0x20   /* wake when a charger is inserted           */
#define OOCC1_EXTONWAK 0x40   /* wake on external / button / dock          */

/* The exact payload byte power_standby must write to OOCC1. */
#define OOCC1_EXPECTED ((uint8_t)(OOCC1_GOSTDBY | OOCC1_CHGWAK | OOCC1_EXTONWAK))

/* ---- escaping the deliberate spin ----------------------------------- */

static sigjmp_buf g_escape;
static int        g_escape_armed;
static volatile sig_atomic_t g_timed_out;

/*
 * Bail out of power_standby() once its transaction has been strobed. The
 * strobe is the last write of an i2c_send: a write to I2C_CTRL_ADDR with
 * I2C_SEND set. Everything the test asserts has been recorded by then, and
 * what follows in the driver is the intentional infinite loop.
 */
static void escape_on_strobe(const mmio_event *e)
{
    if (!g_escape_armed) {
        return;
    }
    if (e->op == MMIO_OP_WRITE && e->addr == I2C_CTRL_ADDR &&
        (e->value & I2C_SEND) != 0) {
        g_escape_armed = 0;
        siglongjmp(g_escape, 1);
    }
}

/*
 * Watchdog. power_standby() reaches its `for (;;)` on EVERY path, including
 * the one where the I2C transaction failed and no standby was ever latched.
 * That path performs no further MMIO, so the bus hook can't see it — a timer
 * is the only way out, and "did we need the timer?" is itself the finding.
 */
static void on_alarm(int sig)
{
    (void)sig;
    g_timed_out = 1;
    siglongjmp(g_escape, 2);
}

static void arm_watchdog(void)
{
    struct itimerval it;
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = 200000;      /* 200 ms is eons for a bounded spin */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 0;
    signal(SIGALRM, on_alarm);
    setitimer(ITIMER_REAL, &it, 0);
}

static void disarm_watchdog(void)
{
    struct itimerval it = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &it, 0);
    signal(SIGALRM, SIG_DFL);
}

typedef enum {
    STANDBY_STROBED,   /* the GOSTDBY transaction was strobed              */
    STANDBY_RETURNED,  /* the function returned without strobing           */
    STANDBY_SPUN,      /* it never returned and never strobed: a dead hang */
} standby_outcome;

static standby_outcome run_standby(void)
{
    g_timed_out    = 0;
    g_escape_armed = 1;
    mmio_mock_set_hook(escape_on_strobe);
    arm_watchdog();

    standby_outcome out;
    int jumped = sigsetjmp(g_escape, 1);
    if (jumped == 0) {
        power_standby();
        out = STANDBY_RETURNED;        /* returned on its own */
    } else if (jumped == 1) {
        out = STANDBY_STROBED;
    } else {
        out = STANDBY_SPUN;
    }

    disarm_watchdog();
    g_escape_armed = 0;
    mmio_mock_set_hook(0);
    return out;
}

/* ---- log helpers ----------------------------------------------------- */

static size_t count_writes(uint32_t addr)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len(), c = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            c++;
        }
    }
    return c;
}

static uint32_t last_write(uint32_t addr, int *found)
{
    const mmio_event *log = mmio_mock_log();
    size_t len = mmio_mock_log_len();
    uint32_t v = 0;
    *found = 0;
    for (size_t i = 0; i < len; i++) {
        if (log[i].op == MMIO_OP_WRITE && log[i].addr == addr) {
            v = log[i].value;
            *found = 1;
        }
    }
    return v;
}

int main(void)
{
    xfail_ctx c = { "power-standby", 0, 0, 0 };

    /* --- Case 1: the exact transaction, event by event ---------------- *
     * i2c_send's grammar (09-i2c.md): leading BUSY poll, device address with
     * the R/W bit CLEAR, select write mode, load DATA0..DATA(len-1), set the
     * byte count, strobe. Two payload bytes: the register pointer then the
     * value. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, 0x00);   /* controller idle          */
    mmio_mock_set_read(I2C_CTRL_ADDR,   0x00);   /* read-modify-write base   */
    standby_outcome out = run_standby();
    xpect(&c, "power_standby issues the standby transaction",
          out == STANDBY_STROBED);

    trace_cursor tc = trace_begin("power-standby-grammar");
    expect_r(&tc, 8, I2C_STATUS_ADDR);                       /* BUSY poll     */
    expect_w(&tc, 8, I2C_ADDR_ADDR, (PMU_ADDR & 0x7F) << 1); /* write to 0x08 */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, 0x00);                   /* write mode    */
    expect_w(&tc, 8, I2C_DATA_ADDR(0), PMU_OOCC1);           /* register ptr  */
    expect_w(&tc, 8, I2C_DATA_ADDR(1), OOCC1_EXPECTED);      /* the payload   */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    expect_w(&tc, 8, I2C_CTRL_ADDR, (2 - 1) << 1);           /* count = 2     */
    expect_r(&tc, 8, I2C_CTRL_ADDR);
    /* Each CTRL read-modify-write re-reads the register, and the mock bus
     * serves a constant 0 for CTRL (as the battery/volume trace tests do), so
     * the strobe carries only I2C_SEND. The count field itself is asserted on
     * the preceding write. */
    expect_w(&tc, 8, I2C_CTRL_ADDR, I2C_SEND);                   /* strobe    */
    trace_expect_end(&tc);
    if (trace_done(&tc) != 0) {
        c.fails++;
    }

    /* --- Case 2: the invariants, stated on their own ------------------ *
     * Restated separately from the trace so that if a bounded retry (or any
     * other reshaping) changes the event ORDER, these still hold — and so a
     * failure reads as "the wake bits went missing", not "event 6 moved". */
    {
        int found = 0;
        uint32_t payload = last_write(I2C_DATA_ADDR(1), &found);
        xpect(&c, "payload byte was written", found);
        xpect(&c, "GOSTDBY is set (the standby actually triggers)",
                       (payload & OOCC1_GOSTDBY) != 0);
        /* THE assertion. Without a wake bit the device never powers on again. */
        xpect(&c, "EXTONWAK set — button/dock can wake it",
                       (payload & OOCC1_EXTONWAK) != 0);
        xpect(&c, "CHGWAK set — charger insertion can wake it",
                       (payload & OOCC1_CHGWAK) != 0);
        xpect(&c, "no bits beyond GOSTDBY|CHGWAK|EXTONWAK are set",
                       (payload & (uint32_t)~OOCC1_EXPECTED) == 0);
        xpect(&c, "payload is exactly the documented byte",
                       payload == OOCC1_EXPECTED);

        uint32_t reg = last_write(I2C_DATA_ADDR(0), &found);
        xpect(&c, "register pointer byte was written", found);
        xpect(&c, "register pointer selects OOCC1", reg == PMU_OOCC1);

        uint32_t dev = last_write(I2C_ADDR_ADDR, &found);
        xpect(&c, "device address byte was written", found);
        xpect(&c, "addressed device is the PMU control port (0x08)",
                       (dev >> 1) == PMU_ADDR);
        xpect(&c, "R/W bit clear — this is a WRITE",
                       (dev & I2C_ADDR_RW) == 0);

        /* Order matters on a register-pointer device: pointer first, value
         * second. A swap writes GOSTDBY into whatever register 0x41 is. */
        const mmio_event *log = mmio_mock_log();
        size_t len = mmio_mock_log_len();
        size_t i_reg = len, i_val = len;
        for (size_t i = 0; i < len; i++) {
            if (log[i].op != MMIO_OP_WRITE) {
                continue;
            }
            if (log[i].addr == I2C_DATA_ADDR(0) && i_reg == len) {
                i_reg = i;
            }
            if (log[i].addr == I2C_DATA_ADDR(1) && i_val == len) {
                i_val = i;
            }
        }
        xpect(&c, "register pointer is loaded before the value",
                       i_reg < i_val && i_val < len);

        /* Exactly two payload bytes: a third would run OOCC1's auto-increment
         * into the next PMU register. */
        xpect(&c, "only DATA0 and DATA1 are loaded",
                       count_writes(I2C_DATA_ADDR(2)) == 0 &&
                       count_writes(I2C_DATA_ADDR(3)) == 0);
    }

    /* --- Case 3: a BUSY bus must not silently skip the standby -------- *
     * If the controller never goes idle, i2c_send times out and returns an
     * error. The driver may retry (a bounded retry is being added), but it
     * must never fall through and pretend it slept — and it must not hang:
     * the spin is bounded, so this call has to terminate. */
    mmio_mock_reset();
    mmio_mock_set_read(I2C_STATUS_ADDR, I2C_BUSY);  /* permanently busy */
    mmio_mock_set_read(I2C_CTRL_ADDR,   0x00);
    out = run_standby();
    xpect(&c, "a permanently-BUSY bus issues no GOSTDBY strobe",
          out != STANDBY_STROBED);
    xpect(&c, "a permanently-BUSY bus writes no payload",
          count_writes(I2C_DATA_ADDR(1)) == 0);
    xfail(&c, "a failed standby write does not wedge the core",
          out != STANDBY_SPUN,
          "hal/hw/power.c power_standby() ignores i2c_send()'s return value "
          "and enters its `for (;;)` unconditionally, so when the bus never "
          "goes idle the device neither sleeps nor comes back — it just stops "
          "responding until a hard reset. The spin is only correct AFTER the "
          "PMU has acknowledged the transition");

    return xfail_done(&c);
}

/*
 * power_standby() relights the backlight when it gives up on a wedged bus,
 * rather than leaving the user with a dark unresponsive device. No panel here.
 */
void backlight_set(int level);
void backlight_set(int level)
{
    (void)level;
}
