/* SPDX-License-Identifier: Apache-2.0 */
/*
 * core/kernel/panic.c — the fault report (see panic.h).
 *
 * Reached from the exception stubs in boot/crt0.S, which hand us a
 * panic_regs_t built on the dedicated abort-mode panic stack. Everything in
 * here has to work when the rest of the machine does not: no allocation, no
 * scheduler, no interrupts, no libc, no assumptions about which subsystem
 * was mid-transaction. Two output channels, both already proven at bring-up:
 *
 *   UART  — hw/uart.h's polled TX (bounded spin per character). Free when a
 *           cable IS attached, invisible when it is not.
 *   PANEL — console.c's 8x8 bitmap text into the RGB565 back buffer, then
 *           lcd_present_fb(). This is the channel that matters: the target
 *           unit has no serial cable, so the register dump has to be
 *           readable off the screen itself.
 *
 * The console font is uppercase-only (console.c g_font), so every string
 * here is uppercase by necessity, not style.
 */

#include "panic.h"
#include "console.h"

#include "hw/uart.h"
#include "hw/lcd.h"

/* Linker-provided supervisor stack bounds (boot/linker.ld). Declared as
 * arrays so the symbol's ADDRESS is the value we want. */
extern uint32_t _stack_limit[];
extern uint32_t _stack_top[];

/* Bottom-of-stack guard band: the last STACK_GUARD_WORDS words above the
 * floor. Cheap O(1) breach test — if anything has written here the stack has
 * effectively hit its floor, whether or not it went all the way over. */
#define STACK_GUARD_WORDS 8u

/* ---- stack occupancy ------------------------------------------------- */

void stack_stat(stack_stat_t *st)
{
    uint32_t *lo = _stack_limit;
    uint32_t *hi = _stack_top;
    uint32_t  sp;

    __asm__ volatile("mov %0, sp" : "=r"(sp));

    st->limit = (uint32_t)(uintptr_t)lo;
    st->top   = (uint32_t)(uintptr_t)hi;
    st->size  = st->top - st->limit;
    st->sp    = sp;

    /* Walk up from the floor through the boot paint; the first word that is
     * no longer STACK_PAINT_WORD is the deepest the stack has ever reached.
     * Bounded by the region (the loop cannot pass `hi`). */
    uint32_t *p = lo;
    while (p < hi && *p == STACK_PAINT_WORD) {
        p++;
    }
    st->used_peak = st->top - (uint32_t)(uintptr_t)p;
    st->free_min  = st->size - st->used_peak;
    st->breached  = (uint32_t)(stack_guard_breached() != 0);
}

int stack_guard_breached(void)
{
    const uint32_t *lo = _stack_limit;
    for (uint32_t i = 0; i < STACK_GUARD_WORDS; i++) {
        if (lo[i] != STACK_PAINT_WORD) {
            return 1;
        }
    }
    return 0;
}

/* ---- cause names ------------------------------------------------------ */

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
    case PANIC_UNDEF: return "UNDEFINED INSTRUCTION";
    case PANIC_SWI:   return "SWI";
    case PANIC_PABT:  return "PREFETCH ABORT";
    case PANIC_DABT:  return "DATA ABORT";
    case PANIC_RESV:  return "RESERVED VECTOR";
    case PANIC_FIQ:   return "SPURIOUS FIQ";
    case PANIC_STACK: return "STACK OVERFLOW";
    case PANIC_SW:    return "SOFTWARE PANIC";
    default:          return "UNKNOWN";
    }
}

/* Register labels in dump order: r0..r12, sp, lr, pc, psr. */
static const char *const reg_name[17] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "R9", "R10", "R11", "R12", "SP", "LR", "PC", "PSR"
};

/* ---- UART channel ----------------------------------------------------- */

static void uart_kv(const char *k, uint32_t v)
{
    uart_puts(k);
    uart_putc('=');
    uart_put_hex32(v);
    uart_putc(' ');
}

static void panic_uart(uint32_t cause, const panic_regs_t *regs,
                       const stack_stat_t *st)
{
    uart_puts("\n*** PANIC: ");
    uart_puts(cause_name(cause));
    uart_puts(" (");
    uart_put_hex32(cause);
    uart_puts(")\n");

    if (regs != 0) {
        for (int i = 0; i < 17; i++) {
            uint32_t v = (i < 13) ? regs->r[i]
                       : (i == 13) ? regs->sp
                       : (i == 14) ? regs->lr
                       : (i == 15) ? regs->pc
                                   : regs->cpsr;
            uart_kv(reg_name[i], v);
            if ((i % 4) == 3) {
                uart_putc('\n');
            }
        }
        uart_putc('\n');
    } else {
        uart_puts("(no register frame: software panic)\n");
    }

    uart_puts("STACK ");
    uart_kv("TOP", st->top);
    uart_kv("LIMIT", st->limit);
    uart_kv("PEAK", st->used_peak);
    uart_kv("FREE", st->free_min);
    uart_kv("SPNOW", st->sp);
    uart_kv("BREACH", st->breached);
    uart_puts("\nHALTED\n");
}

/* ---- panel channel ---------------------------------------------------- */

/* Character grid: 40x30 (console.c). Registers go three to a line in 13-cell
 * columns — a 4-cell label plus 8 hex digits fits with a space to spare. */
#define PAN_BG   CON_BLACK
#define PAN_FG   CON_WHITE
#define PAN_HDR  CON_RED
#define PAN_DIM  CON_YELLOW

static void panic_screen(uint32_t cause, const panic_regs_t *regs,
                         const stack_stat_t *st)
{
    console_clear(PAN_BG);
    console_str(0, 0, "PANIC", PAN_HDR, PAN_BG);
    console_str(6, 0, cause_name(cause), PAN_HDR, PAN_BG);
    console_str(0, 1, "----------------------------------------", PAN_DIM, PAN_BG);

    int row = 3;
    if (regs != 0) {
        for (int i = 0; i < 17; i++) {
            uint32_t v = (i < 13) ? regs->r[i]
                       : (i == 13) ? regs->sp
                       : (i == 14) ? regs->lr
                       : (i == 15) ? regs->pc
                                   : regs->cpsr;
            int col = (i % 3) * 13;
            console_str(col, row, reg_name[i], PAN_DIM, PAN_BG);
            console_hex32(col + 4, row, v, PAN_FG, PAN_BG);
            if ((i % 3) == 2) {
                row++;
            }
        }
        if ((17 % 3) != 0) {
            row++;
        }
    } else {
        console_str(0, row, "NO REGISTER FRAME", PAN_FG, PAN_BG);
        row += 2;
    }

    row++;
    console_str(0, row, "STACK TOP", PAN_DIM, PAN_BG);
    console_hex32(13, row, st->top, PAN_FG, PAN_BG);
    console_str(26, row, "LIMIT", PAN_DIM, PAN_BG);
    console_hex32(32, row, st->limit, PAN_FG, PAN_BG);
    row++;
    console_str(0, row, "USED PEAK", PAN_DIM, PAN_BG);
    console_hex32(13, row, st->used_peak, PAN_FG, PAN_BG);
    console_str(26, row, "FREE", PAN_DIM, PAN_BG);
    console_hex32(32, row, st->free_min, PAN_FG, PAN_BG);
    row++;
    console_str(0, row, "SP NOW", PAN_DIM, PAN_BG);
    console_hex32(13, row, st->sp, PAN_FG, PAN_BG);
    if (st->breached) {
        console_str(26, row, "OVERFLOW", PAN_HDR, PAN_BG);
    }

    /* No '+' in console.c's font — spell it out rather than draw a blank. */
    console_str(0, row + 2, "HALTED. HOLD MENU AND SELECT TO RESET",
                PAN_DIM, PAN_BG);

    /* Present blind: lcd_present_fb only ever polls the BCM with bounded
     * spins (hal/hw/lcd.c), so it degrades to wasted cycles — not a second
     * hang — if the panel was never brought up or the BCM is the thing that
     * wedged. There is no state left worth protecting at this point. */
    lcd_present_fb(console_framebuffer());
}

/* ---- entry point ------------------------------------------------------ */

_Noreturn void panic(uint32_t cause, const panic_regs_t *regs)
{
    /* Mask IRQ+FIQ. Already true when we arrive from a crt0.S fault stub;
     * not true for a software panic() from C. */
    uint32_t cpsr;
    __asm__ volatile("mrs %0, cpsr\n\t"
                     "orr %0, %0, #0xC0\n\t"
                     "msr cpsr_c, %0"
                     : "=r"(cpsr) : : "memory");

    /* Re-entry guard: if reporting the first fault faults again (a wedged
     * BCM, a bad framebuffer), do not loop through the report forever —
     * emit one marker and stop. */
    static int in_panic;
    if (in_panic) {
        uart_puts("\n*** DOUBLE FAULT\n");
        for (;;) {
            /* Terminal by design: the only exit is a battery pull / the
             * MENU+SELECT hardware reset. */
        }
    }
    in_panic = 1;

    stack_stat_t st;
    stack_stat(&st);

    panic_uart(cause, regs, &st);
    panic_screen(cause, regs, &st);

    for (;;) {
        /* Terminal by design (see above). */
    }
}
