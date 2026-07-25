/*
 * core/hal/hw/uart.c — SER0 debug UART driver (PP5022, polled TX).
 *
 * First register-touching driver in the tree. Implements the init
 * sequence from core/docs/hw/08-boot-dock.md ("UART debug" -> "Init
 * sequence (default 115200 8-N-1)"), verified against Rockbox
 * uart-pp.c / pp5020.h (2026-06-10), minus the IRQ/FIFO setup a
 * polled transmitter doesn't need.
 *
 * Freestanding-clean: no libc, fixed-width types from <stdint.h> only.
 */

#include "pp5022.h"
#include "mmio.h"
#include "uart.h"

/*
 * Upper bound on the TX-ready poll so a dead UART can't hang the
 * kernel. One character at 115200 takes ~87 us = ~2100 CPU cycles at
 * the 24 MHz boot clock — a few hundred trips around this loop — so
 * 1<<16 iterations is orders of magnitude past "working hardware".
 * If we ever time out (e.g. SER0 unclocked or no cable attached),
 * the write is attempted anyway and the byte is silently lost; the
 * debug channel degrades, the firmware keeps running.
 */
#define UART_TX_SPIN_LIMIT  (1u << 16)

static void uart_tx_byte(uint8_t b)
{
    uint32_t spin = UART_TX_SPIN_LIMIT;
    while (!(mmio_read32(SER0_LSR_ADDR) & SER0_LSR_THRE) && --spin != 0) {
        /* poll */
    }
    mmio_write32(SER0_THR_ADDR, b);
}

void uart_init(void)
{
    /* Init order follows the iPod Video reference sequence (doc
     * corrected + verified against Rockbox uart-pp.c, 2026-06-10):
     * pin routing -> GPO32 release -> device enable -> reset pulse ->
     * line/divisor setup. */

    /* Route SER0 TX/RX to the dock-connector pins: clear bits 2-3 of
     * the routing register, then clear the same bits in GPO32_ENABLE
     * to release those pads from general-purpose-output mode so the
     * SER0 alternate function drives them (08-boot-dock.md, "GPIO
     * routing for SER0 on iPod Video"). */
    mmio_write32(SER0_GPIO_ROUTE_ADDR,
                 mmio_read32(SER0_GPIO_ROUTE_ADDR) & ~SER0_GPIO_ROUTE_MASK);
    mmio_write32(GPO32_ENABLE_ADDR,
                 mmio_read32(GPO32_ENABLE_ADDR) & ~SER0_GPIO_ROUTE_MASK);

    /* Power the UART block, then pulse its reset (08-boot-dock.md,
     * "Init sequence"; DEV_SER0 = bit 6). The reference holds reset
     * for one ~10 ms scheduler tick; with no timer driver yet we
     * busy-wait a conservatively long bounded loop instead (1M
     * iterations is multiple ms even at 80 MHz). */
    mmio_write32(DEV_EN_ADDR, mmio_read32(DEV_EN_ADDR) | DEV_SER0);
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) | DEV_SER0);
    for (volatile uint32_t i = 0; i < (1u << 20); i++) {
        /* hold reset */
    }
    mmio_write32(DEV_RS_ADDR, mmio_read32(DEV_RS_ADDR) & ~DEV_SER0);

    /* Program the divisor latch for 115200 on the 24 MHz reference:
     * divisor 13 = 0x0D (08-boot-dock.md, "Baud rate" table), then
     * drop DLAB and set 8N1 ("Init sequence"). The reference programs
     * DLL through a second DLAB window after LCR/IER/FCR; writing it
     * inside the first window is functionally equivalent. */
    mmio_write32(SER0_LCR_ADDR, SER0_LCR_DLAB);
    mmio_write32(SER0_DLM_ADDR, 0x00);
    mmio_write32(SER0_DLL_ADDR, SER0_DIV_115200);
    mmio_write32(SER0_LCR_ADDR, SER0_LCR_8N1);

    /*
     * Deliberate deviations from the reference init tail (the IER/FCR
     * stride question is resolved — IER/DLM share +0x04, FCR/IIR
     * +0x08, all word-wide — so these are now choices, not doc gaps):
     *   - SER0_IER = 0x01 (RX IRQ) is skipped: the reference enables
     *     it for accessory-protocol RX and then masks the SER0 IRQ at
     *     the controller anyway; we are polled-TX only with no vector
     *     table installed.
     *   - SER0_FCR = 0x07 (enable + reset FIFOs) is skipped: an 8250
     *     transmits fine in non-FIFO mode via THR/THRE, and the panic
     *     channel has no use for RX buffering yet.
     */
}

void uart_putc(char c)
{
    if (c == '\n') {
        uart_tx_byte('\r');
    }
    uart_tx_byte((uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

void uart_put_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";   /* 17 B incl NUL; idx 0..15 */

    for (int shift = 28; shift >= 0; shift -= 4) {
        uart_tx_byte((uint8_t)hex[(v >> shift) & 0xF]);
    }
}
