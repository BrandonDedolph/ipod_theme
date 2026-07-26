/*
 * tests/hw_mmio/mmio_mock.h — recording fake MMIO bus for host tests.
 *
 * Backs the -DMMIO_MOCK accessors declared in hal/hw/mmio.h. It does
 * two things:
 *
 *   1. Records every access as an ordered {op, width, addr, value}
 *      event, so a test can assert the exact register grammar a driver
 *      emits — addresses, values, widths, and their order.
 *
 *   2. Answers reads from a per-address script, so a test can drive the
 *      poll loops: e.g. "SER0_LSR reads not-ready N times, then ready"
 *      proves the driver actually spins on THRE, not just that it
 *      eventually wrote THR.
 *
 * Values are hand-derived from core/docs/hw/ (via pp5022.h), never
 * extracted from Rockbox source — the cleanroom boundary applies to
 * test vectors too.
 */

#ifndef CORE_TESTS_HW_MMIO_MOCK_H
#define CORE_TESTS_HW_MMIO_MOCK_H

#include <stddef.h>
#include <stdint.h>

typedef enum { MMIO_OP_READ, MMIO_OP_WRITE } mmio_op;

typedef struct {
    mmio_op  op;
    int      width;   /* 8, 16, or 32 */
    uint32_t addr;
    uint32_t value;   /* value written, or value returned to a read */
} mmio_event;

/* Clear the event log and all programmed reads. Call at the top of
 * every test case. */
void mmio_mock_reset(void);

/* Program the value a read of `addr` returns. Without a program, reads
 * return 0 (which, for a status register whose ready bit the driver
 * polls, means "never ready" — the driver spins to its bounded limit).
 * A constant program makes the register read `value` on every access. */
void mmio_mock_set_read(uint32_t addr, uint32_t value);

/* Program a finite read sequence for `addr`: the first `n` reads return
 * seq[0..n-1] in order, and every read after that repeats seq[n-1].
 * Used to model "busy K times, then ready". `n` must be >= 1. */
void mmio_mock_queue_read(uint32_t addr, const uint32_t *seq, size_t n);

/* The recorded event log, in access order. */
const mmio_event *mmio_mock_log(void);
size_t            mmio_mock_log_len(void);

/* Events dropped after the log filled. Nonzero means the recorded
 * grammar is only a PREFIX of what the driver actually did — expected
 * when a test deliberately wedges a bus and the driver spins to its
 * bounded limit. */
size_t            mmio_mock_dropped(void);

/* Convenience: count recorded events of a given op+addr (any width). */
size_t mmio_mock_count(mmio_op op, uint32_t addr);

/*
 * Post-access hook, invoked with the event that was just recorded.
 *
 * Exists for drivers that DELIBERATELY never return: hal/hw/power.c's
 * power_standby() ends in an intentional `for (;;)` because the PMU is about
 * to cut power, so a host test that simply called it would hang. The hook lets
 * the test observe the transaction and longjmp out at the exact access that
 * completes it, which is the only way to assert power_standby's grammar
 * without editing the driver.
 *
 * Cleared by mmio_mock_reset(); pass NULL to clear it explicitly. The hook
 * must not call back into the mock bus.
 */
typedef void (*mmio_mock_hook_fn)(const mmio_event *e);
void mmio_mock_set_hook(mmio_mock_hook_fn fn);

#endif /* CORE_TESTS_HW_MMIO_MOCK_H */
