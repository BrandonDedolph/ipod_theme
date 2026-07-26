/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/panic.h — fault handling: the last thing this firmware does.
 *
 * There is no serial cable on the target unit, no debugger and no watchdog,
 * so an unhandled exception used to be indistinguishable from any other
 * freeze: boot/crt0.S vectored undefined-instruction, SWI, prefetch abort,
 * data abort, reserved and FIQ all at one `b .`. The fault stubs in crt0.S
 * now capture the faulting context and call panic(), which dumps it over
 * the SER0 UART *and* paints it on the panel — the register dump survives
 * on screen with no host attached.
 *
 * Also home to the supervisor-stack guard: crt0.S paints the whole stack
 * region with STACK_PAINT_WORD at boot, and the helpers below turn that
 * paint into a high-water mark and a cheap breach test. No per-call-frame
 * checking — the cost is paid only where it is asked for (idle path, panic).
 */
#ifndef CORE_KERNEL_PANIC_H
#define CORE_KERNEL_PANIC_H

#include <stdint.h>

/* Fault causes. MUST match the PANIC_* .equ values in boot/crt0.S. */
#define PANIC_UNDEF   1u   /* undefined instruction (vector 0x04)          */
#define PANIC_SWI     2u   /* SWI — we issue none, so this is a wild jump  */
#define PANIC_PABT    3u   /* prefetch abort (vector 0x0C)                 */
#define PANIC_DABT    4u   /* data abort — the wild-pointer case (0x10)    */
#define PANIC_RESV    5u   /* reserved vector (0x14): never taken by HW    */
#define PANIC_FIQ     6u   /* FIQ — nothing routes one, so: spurious       */
#define PANIC_STACK   7u   /* software: supervisor stack guard breached    */
#define PANIC_SW      8u   /* software: an explicit panic() from C code    */

/*
 * Word pattern crt0.S paints [_stack_limit, _stack_top) with at boot.
 * Chosen to be a value no plausible pointer, length or RGB565 pair equals,
 * and different in every byte lane so a partial overwrite still shows.
 */
#define STACK_PAINT_WORD 0xA5A55A5Au

/*
 * The faulting context, built on the panic stack by crt0.S's fault_common.
 * Field order IS the in-memory push order — do not reorder without editing
 * fault_common (which also indexes `cause` as [17*4]).
 */
typedef struct {
    uint32_t r[13];   /* r0..r12 as of the fault (FIQ: r8-r12 are the
                       * interrupted context's, not the FIQ-banked ones) */
    uint32_t sp;      /* faulting mode's banked SP  (0 if unrecoverable) */
    uint32_t lr;      /* faulting mode's banked LR  (0 if unrecoverable) */
    uint32_t pc;      /* faulting instruction, already pipeline-corrected */
    uint32_t cpsr;    /* CPSR at the time of the fault (the exception SPSR) */
    uint32_t cause;   /* PANIC_* */
} panic_regs_t;

/*
 * Terminal fault report. Dumps `cause`, `regs` and the stack high-water mark
 * over UART and onto the LCD (console_* text + a present), then halts the
 * core with interrupts masked. `regs` may be NULL for a software panic that
 * has no captured frame (e.g. panic(PANIC_STACK, 0)). Never returns.
 */
_Noreturn void panic(uint32_t cause, const panic_regs_t *regs);

/* Supervisor-stack occupancy, measured from crt0.S's boot paint. */
typedef struct {
    uint32_t limit;      /* _stack_limit: lowest address the stack may reach */
    uint32_t top;        /* _stack_top: where SP starts (exclusive)          */
    uint32_t size;       /* top - limit, bytes                               */
    uint32_t used_peak;  /* high-water mark in bytes (top - lowest painted)  */
    uint32_t free_min;   /* size - used_peak: headroom that was never used   */
    uint32_t sp;         /* SP right now                                     */
    uint32_t breached;   /* 1 = the bottom guard band was written            */
} stack_stat_t;

/*
 * Walk the paint to find the high-water mark. Bounded by the stack size
 * (~22k word compares, worst case) — cheap enough for an idle-path call or
 * a diagnostics screen, too expensive for a hot loop.
 */
void stack_stat(stack_stat_t *st);

/*
 * O(1) breach test: returns 0 if the bottom guard band of the supervisor
 * stack is still painted (and SP is inside the region), nonzero if the stack
 * has run into its floor. Cheap enough to call from the idle path every
 * pass. A caller that wants the firmware to stop dead on a breach can do
 * `if (stack_guard_breached()) panic(PANIC_STACK, 0);`.
 */
int stack_guard_breached(void);

#endif /* CORE_KERNEL_PANIC_H */
