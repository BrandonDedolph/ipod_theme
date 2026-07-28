/*
 * core/hal/hw/pp5022.h — PortalPlayer PP5022 platform constants.
 *
 * Canonical home for the hardware constants used by the hw HAL backend
 * and (eventually) boot/crt0.S. Every value here is transcribed from
 * the Phase-0 hardware reference under core/docs/hw/ — that reference
 * is the ONLY permitted source for MMIO addresses and bit layouts.
 * Each block cites the doc section it came from. Where the docs are
 * silent or self-inconsistent, the gap is marked TODO(hw-doc) rather
 * than filled from memory.
 *
 * The header is dual-mode: bare addresses (*_ADDR / sizes / bit masks)
 * are visible to both C and assembly; the volatile lvalue accessors
 * (SER0_LCR = ...) are C-only behind the __ASSEMBLER__ guard. crt0.S
 * still carries its own inline constants for now — rewiring it to use
 * this header is deliberately out of scope for this PR.
 */

#ifndef CORE_HAL_HW_PP5022_H
#define CORE_HAL_HW_PP5022_H

/* ---------- MMIO accessors (C only) --------------------------------- */

#ifndef __ASSEMBLER__
#include <stdint.h>

#define PP_REG32(addr)  (*(volatile uint32_t *)(addr))
#define PP_REG16(addr)  (*(volatile uint16_t *)(addr))
#define PP_REG8(addr)   (*(volatile uint8_t  *)(addr))
#endif

/* ---------- Memory map ----------------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Memory map".
 *
 * SDRAM sits at its native 0x10000000 at handoff (the chainloading
 * bootloader restores the Apple-ROM MMAP state before jumping); OUR
 * crt0 remaps it to 0x00000000 so the ARM exception vectors land in
 * our image (see MMAP0 below and boot/crt0.S).
 */

#define SDRAM_BASE          0x00000000  /* post-MMAP0-remap logical base */
#define SDRAM_NATIVE_BASE   0x10000000  /* physical base, pre-remap      */
#define SDRAM_SIZE_5G       0x02000000  /* 32 MB (iPod Video 5G)         */
#define SDRAM_SIZE_5_5G     0x04000000  /* 64 MB (iPod Video 5.5G)       */

#define IRAM_BASE           0x40000000
#define IRAM_SIZE           0x00020000  /* 128 KB total                  */
#define IRAM_USABLE_SIZE    0x00018000  /* ~96 KB usable to user code    */

/* ---------- MMAP0: logical->physical remap ---------------------------
 * core/docs/hw/01-soc-pp5022.md, "Memory remap (MMAP0)", verified
 * against Rockbox crt0-pp.S + pp5020.h (2026-06-10): MMAP0_LOGICAL
 * (0xF000F000) takes the logical base OR'd with the window-size mask
 * (the mask lives HERE, not in the physical flags); MMAP0_PHYSICAL
 * (0xF000F004) takes the physical base OR'd with permission flags.
 * 0x0F00 = read|write|data|code; 0x84 is undocumented but present in
 * every known user. 0x0F84 is the PP502x flags value (0x3F84 is
 * PP5002's). Write order: logical first, then physical; no barrier.
 */

#define MMAP0_LOGICAL_ADDR    0xF000F000
#define MMAP0_PHYSICAL_ADDR   0xF000F004
#define MMAP0_REMAP_FLAGS     0x00000F84  /* read|write|data|code (+0x84) */
#define MMAP0_WINDOW_64M      0x00003C00  /* logical-register size mask   */
#define MMAP0_WINDOW_32M      0x00003E00  /*   (64M used for all Videos)  */

#ifndef __ASSEMBLER__
#define MMAP0_LOGICAL   PP_REG32(MMAP0_LOGICAL_ADDR)
#define MMAP0_PHYSICAL  PP_REG32(MMAP0_PHYSICAL_ADDR)
#endif

/* ---------- Processor ID ---------------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Dual core: CPU and COP". Reading
 * PROCESSOR_ID distinguishes the two ARM7TDMI cores.
 */

#define PROCESSOR_ID_ADDR   0x60000000
#define PROC_ID_CPU         0x55
#define PROC_ID_COP         0xAA

#ifndef __ASSEMBLER__
#define PROCESSOR_ID    PP_REG32(PROCESSOR_ID_ADDR)
#endif

/* ---------- Dual-core control: CPU_CTL / COP_CTL ---------------------
 * core/docs/hw/01-soc-pp5022.md, "Dual core: CPU and COP" -> "Sleep /
 * wake". Per-core power/flow control; PROC_CTL(core) selects between
 * them. CPU is 0x55 / COP is 0xAA in PROCESSOR_ID. A write must be
 * followed by 3 NOPs (the pipeline needs a beat to take effect).
 *
 *   PROC_SLEEP     sleep until an interrupt (hangs with no IRQ source)
 *   PROC_WAIT_CNT  sleep until a countdown expires (self-wakes)
 *   PROC_WAKE_INT  also raise an IRQ on wake
 *   PROC_CNT_*     countdown tick unit; low 8 bits are the count
 *
 * clicky models this (cpucon.rs: a core with any flow bit set is not
 * stepped), so a COP_CTL sleep halts the COP in the emulator exactly as
 * on hardware — verified 2026-07-17.
 */

#define CPU_CTL_ADDR        0x60007000
#define COP_CTL_ADDR        0x60007004

#define PROC_SLEEP          0x80000000
#define PROC_WAIT_CNT       0x40000000
#define PROC_WAKE_INT       0x20000000
#define PROC_CNT_USEC       0x02000000
#define PROC_CNT_MSEC       0x01000000
#define PROC_CNT_MASK       0x000000FF  /* low 8 bits: countdown value */

#ifndef __ASSEMBLER__
#define CPU_CTL         PP_REG32(CPU_CTL_ADDR)
#define COP_CTL         PP_REG32(COP_CTL_ADDR)
#endif

/* ---------- Unified cache (8 KB, 16-byte lines) ----------------------
 * core/docs/hw/01-soc-pp5022.md, "Cache". Write-back; init does NOT set
 * VECT_REMAP so the vectors stay at 0x0. A DMA agent reading CPU-written
 * memory must be preceded by a FLUSH (write dirty lines back to SDRAM).
 */
#define CACHE_CTL_ADDR       0x6000C000
#define CACHE_PRIORITY_ADDR  0x60006044
#define CACHE_MASK_ADDR      0xF000F040
#define CACHE_OP_ADDR        0xF000F044

#define CACHE_CTL_ENABLE     0x0001
#define CACHE_CTL_RUN        0x0002
#define CACHE_CTL_INIT       0x0004
#define CACHE_CTL_VECT_REMAP 0x0010
#define CACHE_CTL_READY      0x4000
#define CACHE_CTL_BUSY       0x8000

#define CACHE_OP_FLUSH       0x0002
#define CACHE_OP_INVALIDATE  0x0004

#define CACHE_PRIORITY_CPU   0x10   /* CACHE_PRIORITY bit for the CPU core */
#define CACHE_INIT_MASK      0x00001C00  /* CACHE_MASK value (cacheable region) */
#define CACHE_INIT_OP        0x00000FC0  /* CACHE_OPERATION value during init   */
#define CACHE_SIZE_BYTES     0x2000      /* 8 KB store                          */
#define CACHE_LINE_BYTES     16u
#define CACHE_PRIME_SRC     0x2000      /* readable cached SDRAM to prime lines */

#ifndef __ASSEMBLER__
#define CACHE_CTL       PP_REG32(CACHE_CTL_ADDR)
#define CACHE_PRIORITY  PP_REG32(CACHE_PRIORITY_ADDR)
#define CACHE_MASK_REG  PP_REG32(CACHE_MASK_ADDR)
#define CACHE_OP        PP_REG32(CACHE_OP_ADDR)
#endif

/* ---------- Interrupt controller -------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Interrupt controller". Low-priority
 * bank only (the timer sources we need live there). Sources #<32 route
 * through CPU_INT_EN/DIS at bit #N; write-1-to-enable / write-1-to-
 * disable. The CPSR I-bit gates IRQs at the core.
 */

#define CPU_INT_STAT_ADDR     0x60004000
#define CPU_INT_EN_ADDR       0x60004024  /* write 1<<N to enable IRQ N  */
#define CPU_INT_DIS_ADDR      0x60004028  /* write 1<<N to disable IRQ N */
#define CPU_INT_PRIORITY_ADDR 0x6000402C

/* High interrupt bank (sources 32..63), mirroring the low bank at +0x100.
 * The clickwheel/I2C IRQ (#40) lives here. We handle no high-bank source
 * yet, so irq_dispatch just masks any that assert — otherwise an un-acked
 * high-bank IRQ (e.g. an armed wheel) livelocks the core. */
#define CPU_HI_INT_STAT_ADDR  0x60004100
#define CPU_HI_INT_DIS_ADDR   0x60004128

/* The COP's own mask register. We park the COP at boot (crt0.S) and never
 * wake it, so nothing here needs it during normal running — it exists for
 * power_reboot(), which masks BOTH cores' interrupts before pulling the
 * system reset, so neither core can take an IRQ into a half-reset machine. */
#define COP_INT_DIS_ADDR      0x60004038

#define TIMER1_IRQ          0             /* low bank, 1<<0 (01-soc, IRQ #) */
#define TIMER2_IRQ          1             /* low bank, 1<<1 */

#define CPSR_I_BIT          0x00000080    /* CPSR IRQ-disable bit */

#ifndef __ASSEMBLER__
#define CPU_INT_STAT    PP_REG32(CPU_INT_STAT_ADDR)
#define CPU_INT_EN      PP_REG32(CPU_INT_EN_ADDR)
#define CPU_INT_DIS     PP_REG32(CPU_INT_DIS_ADDR)
#endif

/* ---------- Timers / system tick -------------------------------------
 * core/docs/hw/01-soc-pp5022.md, "Timers and the system tick". Two
 * programmable down-counters + a free-running microsecond counter, all
 * clocked at a fixed 1 MHz (TIMER_FREQ). TIMERx_CFG is armed in one
 * write: 0xC0000000 | (period_us - 1) — enable + IRQ/periodic-reload +
 * the reload period. Reading TIMERx_VAL acknowledges that timer's IRQ.
 */

#define TIMER1_CFG_ADDR     0x60005000
#define TIMER1_VAL_ADDR     0x60005004
#define TIMER2_CFG_ADDR     0x60005008  /* recorded; unused by the tick */
#define TIMER2_VAL_ADDR     0x6000500C
#define USEC_TIMER_ADDR     0x60005010  /* free-running 1 MHz counter   */

#define TIMER_FREQ          1000000     /* 1 MHz timer clock            */
#define TIMER_CFG_ENABLE    0x80000000  /* CFG bit 31: timer enable     */
#define TIMER_CFG_IRQEN     0x40000000  /* CFG bit 30: IRQ + periodic reload */

#ifndef __ASSEMBLER__
#define TIMER1_CFG      PP_REG32(TIMER1_CFG_ADDR)
#define TIMER1_VAL      PP_REG32(TIMER1_VAL_ADDR)
#define USEC_TIMER      PP_REG32(USEC_TIMER_ADDR)
#endif

/* ---------- Device enable / reset / clock control --------------------
 * core/docs/hw/01-soc-pp5022.md, "Power management" (DEV_*) and
 * "Clock tree" (PLL_*, CLOCK_SOURCE, DEV_TIMING1). These gate
 * per-peripheral clock and reset. DEV_SER0/SER1 bit values verified
 * against Rockbox pp5020.h (2026-06-10); used by the 08-boot-dock.md
 * "Init sequence" enable + reset pulse.
 */

#define DEV_RS_ADDR         0x60006004
#define DEV_RS2_ADDR        0x60006008
#define DEV_EN_ADDR         0x6000600C
#define DEV_EN2_ADDR        0x60006010

#define DEV_SER0            0x00000040  /* bit 6 in DEV_EN / DEV_RS */
#define DEV_SER1            0x00000080  /* bit 7 */

/* Whole-system reset. Setting this bit in DEV_RS reboots the SoC — the one
 * bit here that is NOT a per-peripheral gate. Used only by power_reboot()
 * (hal/hw/power.c), which is how we hand control back to the Apple boot ROM
 * (e.g. to enter ROM disk mode). Bit value verified against Rockbox
 * pp5020.h:154 DEV_SYSTEM and its PP502x system_reboot(), which for iPod
 * targets is exactly `DEV_RS |= DEV_SYSTEM` after masking both cores'
 * interrupts (2026-07-26). Our DEV_RS address already matches theirs. */
#define DEV_SYSTEM          0x00000004  /* bit 2: system reset */
#define DEV_INIT1_ADDR      0x70000010
#define DEV_INIT2_ADDR      0x70000020

#define CLOCK_SOURCE_ADDR   0x60006020
#define PLL_CONTROL_ADDR    0x60006034
#define PLL_STATUS_ADDR     0x6000603C
#define DEV_TIMING1_ADDR    0x70000034

#ifndef __ASSEMBLER__
#define DEV_RS          PP_REG32(DEV_RS_ADDR)
#define DEV_RS2         PP_REG32(DEV_RS2_ADDR)
#define DEV_EN          PP_REG32(DEV_EN_ADDR)
#define DEV_EN2         PP_REG32(DEV_EN2_ADDR)
#define DEV_INIT1       PP_REG32(DEV_INIT1_ADDR)
#define DEV_INIT2       PP_REG32(DEV_INIT2_ADDR)

#define CLOCK_SOURCE    PP_REG32(CLOCK_SOURCE_ADDR)
#define PLL_CONTROL     PP_REG32(PLL_CONTROL_ADDR)
#define PLL_STATUS      PP_REG32(PLL_STATUS_ADDR)
#define DEV_TIMING1     PP_REG32(DEV_TIMING1_ADDR)
#endif

/* ---------- Clock tree / PLL programming values ---------------------
 * core/docs/hw/01-soc-pp5022.md, "Clock tree" (the frequency-switch
 * sequence). Opaque multiplier/divider encodings transcribed from the
 * doc, not re-derived. The PLL runs off the 24 MHz crystal. The
 * PLL_STATUS lock poll MUST be a bounded spin: clicky (and a dead PLL)
 * never sets the lock bit, so an unbounded wait would hang.
 */
#define DEV_INIT2_PLL_POWER   0x40000000  /* DEV_INIT2 bit 30: PLL power */
#define PLL_CONTROL_ENABLE    0x88000000  /* PLL_CONTROL enable bits     */
#define PLL_STATUS_LOCK       0x80000000  /* PLL_STATUS bit 31: locked   */

/* PP5022 programs the PLL in a SINGLE write per target — no pre-stage.
 * (The 0x8A020A03 pre-stage in some references is the PP5020 sequence.) */
#define PLL_CONTROL_30MHZ     0x8A220501  /* 30 MHz: mult 5/1, /4         */
#define PLL_CONTROL_80MHZ     0x8A121403  /* 80 MHz: mult 20/3, /2        */

#define CLOCK_SOURCE_XTAL     0x20002222  /* all domains -> 24 MHz xtal   */
#define CLOCK_SOURCE_PLL      0x20007777  /* all domains -> PLL output    */

#define DEV_TIMING1_SLOW      0x00000303  /* relaxed timing (slow clock)  */
#define DEV_TIMING1_FAST      0x00000808  /* high-speed timing            */

/* ---------- SER0: dock-connector debug UART (8250-style) -------------
 * core/docs/hw/08-boot-dock.md, "UART debug" -> "SoC registers
 * (8250-style at SER0)", stride verified against Rockbox pp5020.h
 * (2026-06-10): the standard 8250 register file with each byte
 * register widened to a 32-bit word slot (uniform 4-byte stride, all
 * accesses word-wide). The classic 8250 sharing holds: RBR (read),
 * THR (write) and DLL (with LCR.DLAB) share +0x00; IER/DLM share
 * +0x04; IIR (read) / FCR (write) share +0x08. Reference clock is
 * 24 MHz; divisor = 24e6 / (16 * baud) (08-boot-dock.md, "Baud rate").
 * (An earlier doc revision byte-strided IER/FCR at 0x70006001/2 —
 * that was a transcription slip, now corrected in the doc.)
 */

#define SER0_BASE_ADDR      0x70006000

#define SER0_RBR_ADDR       0x70006000  /* RX buffer (read)              */
#define SER0_THR_ADDR       0x70006000  /* TX holding (write)            */
#define SER0_DLL_ADDR       0x70006000  /* divisor low  (when LCR.DLAB)  */
#define SER0_IER_ADDR       0x70006004  /* IRQ enable                    */
#define SER0_DLM_ADDR       0x70006004  /* divisor high (when LCR.DLAB)  */
#define SER0_IIR_ADDR       0x70006008  /* IRQ identification (read)     */
#define SER0_FCR_ADDR       0x70006008  /* FIFO control (write)          */
#define SER0_LCR_ADDR       0x7000600C  /* line control                  */
#define SER0_MCR_ADDR       0x70006010  /* modem control                 */
#define SER0_LSR_ADDR       0x70006014  /* line status                   */
#define SER0_MSR_ADDR       0x70006018  /* modem status                  */

/* SER0_LCR bits (08-boot-dock.md, "Init sequence"). */
#define SER0_LCR_DLAB       0x80        /* divisor latch access          */
#define SER0_LCR_8N1        0x03        /* 8 data, no parity, 1 stop     */

/* SER0_LSR bits (08-boot-dock.md, register table). */
#define SER0_LSR_RX_RDY     0x01        /* bit 0: RX data ready          */
#define SER0_LSR_THRE       0x20        /* bit 5: TX holding empty       */

/* Divisor latch values for the 24 MHz reference
 * (08-boot-dock.md, "Baud rate" table). DLM is 0 for all of these. */
#define SER0_DIV_9600       0x9C
#define SER0_DIV_19200      0x4E
#define SER0_DIV_38400      0x27
#define SER0_DIV_57600      0x1A
#define SER0_DIV_115200     0x0D

/* GPIO routing for SER0 on the iPod Video (08-boot-dock.md, "GPIO
 * routing for SER0 on iPod Video"): clear bits 2-3 of the routing
 * register at 0x7000008C (unnamed even in Rockbox, which uses the raw
 * literal), then clear the same bits in GPO32_ENABLE to release those
 * pads from general-purpose-output mode so the SER0 alternate function
 * drives them. GPO32 addresses verified against Rockbox pp5020.h
 * (2026-06-10). */
#define SER0_GPIO_ROUTE_ADDR  0x7000008C
#define SER0_GPIO_ROUTE_MASK  0x0000000C

#define GPO32_VAL_ADDR        0x70000080
#define GPO32_ENABLE_ADDR     0x70000084

/* SER0 interrupt: IRQ #36, i.e. high bank bit 4
 * (01-soc-pp5022.md, "IRQ numbers" table). Unused by the polled
 * driver; recorded for the future IRQ-driven RX path. */
#define SER0_IRQ            36

#ifndef __ASSEMBLER__
/* Word-wide accessors throughout: each 8250 byte register occupies a
 * 32-bit slot and Rockbox accesses them all as 32-bit words (verified
 * 2026-06-10 against pp5020.h / uart-pp.c) — we follow that precedent
 * rather than gamble on byte-lane behavior. */
#define SER0_RBR        PP_REG32(SER0_RBR_ADDR)
#define SER0_THR        PP_REG32(SER0_THR_ADDR)
#define SER0_DLL        PP_REG32(SER0_DLL_ADDR)
#define SER0_IER        PP_REG32(SER0_IER_ADDR)
#define SER0_DLM        PP_REG32(SER0_DLM_ADDR)
#define SER0_IIR        PP_REG32(SER0_IIR_ADDR)
#define SER0_FCR        PP_REG32(SER0_FCR_ADDR)
#define SER0_LCR        PP_REG32(SER0_LCR_ADDR)
#define SER0_MCR        PP_REG32(SER0_MCR_ADDR)
#define SER0_LSR        PP_REG32(SER0_LSR_ADDR)
#define SER0_MSR        PP_REG32(SER0_MSR_ADDR)

#define SER0_GPIO_ROUTE PP_REG32(SER0_GPIO_ROUTE_ADDR)
#define GPO32_VAL       PP_REG32(GPO32_VAL_ADDR)
#define GPO32_ENABLE    PP_REG32(GPO32_ENABLE_ADDR)
#endif

/* ---------- GPIO port C ----------------------------------------------
 * Used by the LCD host-side port init (02-lcd.md, "Host-side port
 * init"): GPIOC bit 6 is the BCM interrupt pin (configured as a GPIO
 * input), bit 7 is released to its alternate function. The doc's GPIO
 * sections carry no port-register addresses, so these come from
 * verified Rockbox pp5020.h facts (2026-06-11): ports A-D base
 * 0x6000D000, ENABLE at +0x00/04/08/0C per port A/B/C/D, OUTPUT_EN at
 * +0x10/14/18/1C. Only the two registers the LCD driver touches are
 * defined here.
 */

#define GPIOC_ENABLE_ADDR     0x6000D008
#define GPIOC_OUTPUT_EN_ADDR  0x6000D018

#ifndef __ASSEMBLER__
#define GPIOC_ENABLE     PP_REG32(GPIOC_ENABLE_ADDR)
#define GPIOC_OUTPUT_EN  PP_REG32(GPIOC_OUTPUT_EN_ADDR)
#endif

/* ---------- GPIO ports B / D / L (LCD backlight dimmer) --------------
 * core/docs/hw/02-lcd.md, "Backlight (GPIO charge-pump dimmer)".
 * The iPod Video 5G / 5.5G (and Nano 1G) backlight is NOT a hardware PWM
 * peripheral — it is three plain GPIO lines driving an external
 * multi-level LED-driver IC (register addresses + the pulse protocol
 * verified against Rockbox firmware/target/arm/ipod/backlight-nano_video.c
 * and firmware/export/pp5020.h, 2026-07-21):
 *   - GPIOB bit 3 (0x08) : backlight-circuit power enable
 *   - GPIOD bit 7 (0x80) : the driver IC's serial "clock" / step line
 *   - GPIOL bit 7 (0x80) : the LED-enable (fast on/off, ISR-safe)
 * Port register layout matches the A-D block above (ENABLE at +0x00
 * group, OUTPUT_EN at +0x10 group, OUTPUT_VAL at +0x20 group); the I-L
 * quad is based at 0x6000D100 (so GPIOL = base+0x0C in each group). Only
 * the three registers per port that the backlight touches are defined.
 */
#define GPIOB_ENABLE_ADDR     0x6000D004
#define GPIOB_OUTPUT_EN_ADDR  0x6000D014
#define GPIOB_OUTPUT_VAL_ADDR 0x6000D024
#define GPIOD_ENABLE_ADDR     0x6000D00C
#define GPIOD_OUTPUT_EN_ADDR  0x6000D01C
#define GPIOD_OUTPUT_VAL_ADDR 0x6000D02C
/* Port A OUTPUT_VAL, for the LTC4066 charge-current gate (HPWR, bit 0x04 —
 * 06-power.md "Charge current control"). Derived from the bank layout above
 * and anchored by GPIOA_INPUT_VAL @0x6000D030 further down: A-D base
 * 0x6000D000, port A at +0x00, OUTPUT_VAL is the +0x20 group. */
#define GPIOA_OUTPUT_VAL_ADDR 0x6000D020

#define GPIOL_ENABLE_ADDR     0x6000D10C
#define GPIOL_OUTPUT_EN_ADDR  0x6000D11C
#define GPIOL_OUTPUT_VAL_ADDR 0x6000D12C

/* GPIO port INPUT_VAL registers (read the live pin state). Same bank
 * layout as the ENABLE/OUTPUT_EN/OUTPUT_VAL groups above: INPUT_VAL sits
 * at the per-port +0x30 group (Rockbox pp5020.h GPIO_x_INPUT_VAL,
 * consistent with GPIOA_INPUT_VAL @0x6000D030 in the clickwheel block and
 * the GPIOC pair — verified 2026-07-21). GPIOB = A-D base 0x6000D000 +
 * 0x30 + port-B 0x04; GPIOL = I-L base 0x6000D100 + 0x30 + port-L 0x0C.
 * Used by battery.c for the charger/charging power-state bits:
 *   - GPIOL bit 3 (0x08): main charger present (active-LOW)
 *   - GPIOL bit 4 (0x10): USB charger present (active-HIGH)
 *   - GPIOB bit 0 (0x01): currently charging   (active-LOW)
 */
#define GPIOB_INPUT_VAL_ADDR  0x6000D034
#define GPIOL_INPUT_VAL_ADDR  0x6000D13C

#ifndef __ASSEMBLER__
#define GPIOB_INPUT_VAL   PP_REG32(GPIOB_INPUT_VAL_ADDR)
#define GPIOL_INPUT_VAL   PP_REG32(GPIOL_INPUT_VAL_ADDR)
#endif

/* Atomic bit set/clear alias. Each GPIO register has a shadow 0x800
 * bytes higher that performs a masked update in a SINGLE 32-bit write —
 * no read-modify-write, so it is safe against a concurrent ISR touching
 * a different bit of the same port. The written word packs a per-bit
 * WRITE-ENABLE mask in bits 15:8 and the target bit VALUES in bits 7:0:
 *   set   bits `m`:  (m << 8) | m
 *   clear bits `m`:  (m << 8)
 * (02-lcd.md, "Backlight"; Rockbox pp5020.h GPIO_SET/CLEAR_BITWISE.) */
#define GPIO_BITWISE_OFFSET   0x800

/* ---------- BCM video coprocessor (LCD) -------------------------------
 * core/docs/hw/02-lcd.md, "Memory-mapped BCM interface" + "Internal
 * BCM addresses" + "Commands", verified against Rockbox lcd-video.c /
 * ipodloader2 fb.c (2026-06-11).
 *
 * Each BCM "register" is a port: the BCM decodes only PP address bits
 * 16..18, low address bits are undecoded, so 16-bit and 32-bit
 * accesses to the same base address both work — a 32-bit store is
 * consumed as two consecutive 16-bit pushes (this is the pixel-stream
 * fast path). Address ports take 32-bit writes; status polls are
 * 16-bit reads.
 */

#define BCM_DATA_ADDR         0x30000000  /* data port (pixels/params)   */
#define BCM_WR_ADDR_ADDR      0x30010000  /* write-destination port      */
#define BCM_RD_ADDR_ADDR      0x30020000  /* read-source port            */
#define BCM_CONTROL_ADDR      0x30030000  /* status flags + cmd strobe   */
#define BCM_ALT_DATA_ADDR     0x30040000  /* alt channel (bootstrap only,*/
#define BCM_ALT_WR_ADDR_ADDR  0x30050000  /*   unused by this driver —   */
#define BCM_ALT_RD_ADDR_ADDR  0x30060000  /*   constants recorded for    */
#define BCM_ALT_CONTROL_ADDR  0x30070000  /*   the future bootstrap PR)  */

/* BCM_CONTROL bits / values (02-lcd.md, "BCM_CONTROL"). */
#define BCM_CONTROL_WR_READY  0x02        /* can accept write addr/data  */
#define BCM_CONTROL_RD_READY  0x10        /* data available on BCM_DATA32*/
#define BCM_CONTROL_STROBE    0x31        /* write: execute queued cmd   */

/* BCM_RD_ADDR (16-bit read) bit 0: read port ready to take an address
 * — poll this BEFORE writing the address (02-lcd.md, read handshake). */
#define BCM_RD_ADDR_READY     0x01

/* BCM-internal absolute addresses (02-lcd.md, "Internal BCM
 * addresses"). CMDPARAM doubles as the LCD_UPDATE framebuffer. */
#define BCMA_CMDPARAM         0xE0000
#define BCMA_COMMAND          0x1F8
#define BCMA_STATUS           0x1FC

/* GPO32 bit 14: BCM power rail. Nonzero in GPO32_VAL means the BCM is
 * powered (and, post-chainload, initialized) — the lcd_init probe
 * (02-lcd.md, "Host-side port init"). */
#define GPO32_BCM_POWER       0x4000

#ifndef __ASSEMBLER__
/* Command encoding: low half is the command, high half its bit
 * inverse, so the BCM's parser rejects corrupt writes (02-lcd.md,
 * "Commands"). BCM_CMD(0) = 0xFFFF0000 = LCD_UPDATE. */
#define BCM_CMD(x)            ((~(uint32_t)(x) << 16) | (uint32_t)(x))
#define BCMCMD_LCD_UPDATE     BCM_CMD(0)

/* 32-bit views: address ports + data fast path. */
#define BCM_DATA32      PP_REG32(BCM_DATA_ADDR)
#define BCM_WR_ADDR32   PP_REG32(BCM_WR_ADDR_ADDR)
#define BCM_RD_ADDR32   PP_REG32(BCM_RD_ADDR_ADDR)

/* 16-bit views: status polls + control strobe. */
#define BCM_RD_ADDR     PP_REG16(BCM_RD_ADDR_ADDR)
#define BCM_CONTROL     PP_REG16(BCM_CONTROL_ADDR)
#endif

/* ---------- BCM bootstrap / firmware upload ---------------------------
 * core/docs/hw/02-lcd.md, "Bootstrap and firmware upload" (lines 114-181)
 * plus "Sleep / wake" (bcm_powerdown's GPO32_VAL &= ~0x4000 power gate).
 *
 * NONE of this is on the normal boot path: the chainload handoff leaves
 * the BCM powered, bootstrapped and idle (see the BCM block above), so
 * these exist only for lcd_recover() — bringing a wedged BCM back from
 * scratch, which is otherwise an unrecoverable device (the framebuffer
 * console is the only debug channel on a unit with no serial cable).
 *
 * The three values 02-lcd.md explicitly files under "Magic constants we
 * don't fully understand" (BCM-INTERNAL 0x10000C00 / 0x10000400 /
 * 0x10001400) are deliberately NOT here: they address the BCM's own
 * memory, not a PP register, and they live next to their use in lcd.c
 * alongside the existing BCM_PANEL_CTL_ADDR.
 */

/* PP-side strap/boot-config registers poked before the BCM handshake
 * (02-lcd.md, "Bootstrap and firmware upload", Stage 1, lines 124-125).
 * The doc names STRAP_OPT_A and pins it to 0x70000008; the second
 * register appears only as the bare literal `outl(0x1313, 0x70000040)`
 * with the comment "strap pins for boot" and no symbol at all, so the
 * name below is OURS, not transcribed. */
#define STRAP_OPT_A_ADDR        0x70000008
#define STRAP_OPT_A_BCM_MASK    0x00000F00  /* cleared before BCM bringup  */
#define STRAP_BOOT_PINS_ADDR    0x70000040  /* unnamed in the doc          */
#define STRAP_BOOT_PINS_BCM     0x00001313  /* "strap pins for boot"       */

/* BCM_ALT_CONTROL handshake bits (02-lcd.md, lines 128-129 and 146-147:
 * before each bootstrap phase, wait for 0x80 to CLEAR then for 0x40 to
 * SET). The doc gives the masks but no names for them. */
#define BCM_ALT_CONTROL_BUSY    0x80
#define BCM_ALT_CONTROL_READY   0x40

/* BCM-internal SRAM base — the firmware blob's upload destination
 * (02-lcd.md line 149: bcm_write_addr(BCMA_SRAM_BASE), "0x0 in
 * BCM-internal mem"). Same family as BCMA_CMDPARAM/COMMAND/STATUS. */
#define BCMA_SRAM_BASE          0x0

/* ---------- Flash ROM window (holds the BCM `vmcs` firmware blob) -----
 * 01-soc-pp5022.md memory map ("ROM / flash 0x20000000-0x200FFFFF, 1 MB")
 * and 02-lcd.md lines 165-171: the blob is located via the flash
 * directory at 0x200FFE00 (ROM base 0x20000000 + 0xFFE00 — the doc
 * carries a correction note about exactly this address losing a zero).
 *
 * Deliberately NOT named *_ADDR. These are ROM *memory* addresses, not
 * peripheral registers, and tests/scripts/check_hw_consistency.py's
 * window check is a list of MMIO peripheral windows — ROM is not one of
 * them, and widening that list to admit memory would blunt the very
 * dropped-digit check this address is the poster child for.
 */
#define FLASH_ROM_BASE          0x20000000u
#define FLASH_ROM_SIZE          0x00100000u
#define FLASH_DIR_BASE          0x200FFE00u
#define FLASH_DIR_BYTES  (FLASH_ROM_BASE + FLASH_ROM_SIZE - FLASH_DIR_BASE)

#ifndef __ASSEMBLER__
/* Four-character ROM section tag, e.g. ROM_ID('v','m','c','s')
 * (02-lcd.md line 167: flash_get_section(ROM_ID('v','m','c','s'), ...)).
 * The doc does NOT state which byte order the directory stores the tag
 * in, so both packings are defined and lcd.c accepts either — see the
 * UNVERIFIED block over bcm_find_vmcs(). */
#define ROM_ID_BE(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                            ((uint32_t)(c) <<  8) |  (uint32_t)(d))
#define ROM_ID_LE(a,b,c,d) ROM_ID_BE(d,c,b,a)
#define FLASH_ID_VMCS_BE   ROM_ID_BE('v','m','c','s')
#define FLASH_ID_VMCS_LE   ROM_ID_LE('v','m','c','s')
#endif

/* ---------- Audio: device clock gating (DEV_EN / DEV_RS bits) --------
 * core/docs/hw/05-audio.md, "MCLK / clock-gating enable path", and
 * core/docs/hw/09-i2c.md, "Controller init". DEV_EN (0x6000600C) and
 * DEV_RS (0x60006004) are declared above; these are the peripheral bits
 * the audio path gates. The EXT-clock select clears bits 3:2 of
 * 0x70000018 to pick the 24 MHz external device clock (codec MCLK
 * reference). Undocumented encodings of that field are left untouched.
 */
#define DEV_EXTCLOCKS         0x00000002  /* DEV_EN bit 1: external device clocks */
#define DEV_I2S               0x00000800  /* DEV_EN/RS bit 11: I2S serializer clock */
#define DEV_I2C               0x00001000  /* DEV_EN/RS bit 12: I2C controller */
#define DEV_OPTO              0x00010000  /* DEV_EN/RS bit 16: clickwheel (03-clickwheel) */

#define DEV_EXTCLK_SEL_ADDR   0x70000018  /* clear the 24 MHz-select field to run EXT@24MHz */
#define DEV_EXTCLK_24MHZ_MASK 0x0000000C  /* bits 3:2 */

/* I2S/CDI pad-function select. Clearing these fields routes the pads to
 * their I2S alternate function (05-audio.md, "MCLK / clock-gating enable
 * path"). DEV_INIT1/2_ADDR are declared above (0x70000010/0x70000020). */
#define DEV_INIT2_I2S_PADS    0x00000300  /* DEV_INIT2 bits 9:8: CDI+I2S pad group */
#define DEV_INIT1_I2S_PADS    0x03000000  /* DEV_INIT1 bits 25:24: second pad group */

/* ---------- On-SoC I2C controller (0x7000C000) ----------------------
 * core/docs/hw/09-i2c.md. BYTE-WIDE registers (use mmio_*8). Distinct
 * from the clickwheel OPTO block at 0x7000C100 (03-clickwheel.md). Only
 * consumer in Phase 1 is the WM8758 codec control port; a transaction
 * carries at most 4 payload bytes (DATA0..DATA3, stride 4).
 */
#define I2C_CTRL_ADDR      0x7000C000  /* strobe/read-sel/count            */
#define I2C_ADDR_ADDR      0x7000C004  /* dev addr <<1 | R/W               */
#define I2C_DATA0_ADDR     0x7000C00C  /* payload byte 0 (stride 4)        */
#define I2C_DATA1_ADDR     0x7000C010  /* payload byte 1                   */
#define I2C_DATA2_ADDR     0x7000C014  /* payload byte 2                   */
#define I2C_DATA3_ADDR     0x7000C018  /* payload byte 3                   */
#define I2C_STATUS_ADDR    0x7000C01C  /* BUSY in bit 6                    */
#define I2C_CLKCFG_ADDR    0x600060A4  /* undocumented init clock poke     */

#define I2C_SEND           0x80        /* CTRL bit 7: begin transaction    */
#define I2C_READ           0x20        /* CTRL bit 5: 1=read, 0=write      */
#define I2C_COUNT_MASK     0x06        /* CTRL bits 2:1: (len-1)           */
#define I2C_ADDR_RW        0x01        /* ADDR bit 0: 1=read, 0=write      */
#define I2C_BUSY           0x40        /* STATUS bit 6: 1=busy             */

#ifndef __ASSEMBLER__
#define I2C_DATA_ADDR(n)   (I2C_DATA0_ADDR + 4u * (uintptr_t)(n))
#endif

/* ---------- Clickwheel / OPTO controller (0x7000C100) ----------------
 * core/docs/hw/03-clickwheel.md. The capacitive wheel AND all five face
 * buttons arrive as one 32-bit status word at CLICKWHEEL_DATA — there are
 * no dedicated GPIO lines for Menu/Play/Prev/Next/Select. The hold switch
 * IS a plain GPIO (GPIOA_INPUT_VAL bit 5, active-low). This block sits in
 * the same 0x7000Cxxx page as the I2C controller above but is distinct.
 * Power/reset via DEV_OPTO (0x00010000, defined in the audio block above)
 * in DEV_EN/DEV_RS; button-latch logic via INIT_BUTTONS in DEV_INIT1.
 * Shares I2C_IRQ (#40, high bank) — but the Phase 1 driver is POLLED, so
 * no IRQ wiring. 32-bit registers; use mmio_*32 / PP_REG32.
 */
#define CLICKWHEEL_CTRLA_ADDR   0x7000C100  /* config + IRQ enable/arm       */
#define CLICKWHEEL_CTRLB_ADDR   0x7000C104  /* IRQ pending / acknowledge     */
#define CLICKWHEEL_DATA_ADDR    0x7000C140  /* status word (bit 31 = valid)  */

/* CTRLA/CTRLB "magic" words. Rockbox writes them as raw literals with no
 * field decode, so we transcribe verbatim (03-clickwheel.md, "Init" + the
 * ISR ack/re-arm). The only structure we assert: the 0x0A1F00 config body
 * is shared between the init and re-arm CTRLA words (top nibble 0xC vs
 * 0x4). Used by the IRQ path; the polled driver writes CW_CTRLA_INIT +
 * CW_CTRLB_CLEAR at bring-up only. */
#define CW_CTRLA_INIT           0xC00A1F00  /* CTRLA: config + IRQ enable    */
#define CW_CTRLA_REARM          0x400A1F00  /* CTRLA: config + IRQ re-arm    */
#define CW_CTRLB_CLEAR          0x01000000  /* CTRLB: clear pending at init  */
#define CW_CTRLB_ACK            0x0C000000  /* CTRLB: OR to ack IRQ (bits 27:26) */

/* CLICKWHEEL_DATA status-word fields (03-clickwheel.md, "Packet format").
 * Valid gate is bit 31 set AND low byte == 0x1A: (w & MASK) == VALUE. */
#define CW_STAT_VALID           0x80000000  /* bit 31: status valid          */
#define CW_STAT_TOUCH           0x40000000  /* bit 30: finger on the wheel   */
#define CW_STAT_HEADER_MASK     0x800000FF  /* valid gate mask               */
#define CW_STAT_HEADER_VALUE    0x8000001A  /* valid gate value (header 0x1A)*/
#define CW_POS_SHIFT            16          /* position in bits 22:16        */
#define CW_POS_MASK             0x7F        /* (w >> 16) & 0x7F, 0..0x5F     */

/* Button bits inside the status word (03-clickwheel.md, "Button bits"). */
#define CW_BTN_SELECT           0x00000100  /* bit 8:  center / select       */
#define CW_BTN_RIGHT            0x00000200  /* bit 9:  next / ffwd           */
#define CW_BTN_LEFT             0x00000400  /* bit 10: prev / rew            */
#define CW_BTN_PLAY             0x00000800  /* bit 11: play / pause          */
#define CW_BTN_MENU             0x00001000  /* bit 12: menu                  */
#define CW_BTN_ALL              0x00001F00  /* all five button bits          */

/* Software quadrature (03-clickwheel.md, "Quadrature / wheel-delta"). The
 * wheel reports ABSOLUTE position; relative motion = differenced position
 * with wrap at half a rotation, gated at the sensitivity threshold. */
#define CW_CLICKS_PER_ROT       96          /* full circle, in steps         */
#define CW_WHEEL_SENSITIVITY    4           /* min |delta| to emit (Video/5G)*/

/* DEV_INIT1 (0x70000010) bit 18: enable the button-latch logic
 * (03-clickwheel.md, "Initialization"). DEV_OPTO lives in the audio
 * block above (0x00010000, DEV_EN/DEV_RS bit 16). */
#define INIT_BUTTONS            0x00040000

/* Hold switch: GPIOA_INPUT_VAL bit 5, active-low (03-clickwheel.md, "Hold
 * switch"). GPIO ports A-D base 0x6000D000; the read/input slot sits at
 * per-port +0x30 (Rockbox pp5020.h, consistent with the GPIOC pair above). */
#define GPIOA_INPUT_VAL_ADDR    0x6000D030
#define CW_HOLD_BIT             0x20        /* bit 5; clear = held (active-low) */

/* Wheel/OPTO shares I2C_IRQ (#40, high bank bit 8) — recorded for a
 * future IRQ path; the Phase 1 driver polls (01-soc-pp5022.md, IRQ tbl). */
#define CW_IRQ                  40

#ifndef __ASSEMBLER__
#define CLICKWHEEL_CTRLA  PP_REG32(CLICKWHEEL_CTRLA_ADDR)
#define CLICKWHEEL_CTRLB  PP_REG32(CLICKWHEEL_CTRLB_ADDR)
#define CLICKWHEEL_DATA   PP_REG32(CLICKWHEEL_DATA_ADDR)
#define GPIOA_INPUT_VAL   PP_REG32(GPIOA_INPUT_VAL_ADDR)
#endif

/* ---------- On-SoC I2S serializer / FIFO (0x70002800) ---------------
 * core/docs/hw/05-audio.md, "SoC I2S block" + "i2s_reset() config
 * sequence". 32-bit registers, absolute addresses (no base-relative
 * offsets in the hardware). On the iPod the WM8758 masters the bus
 * clocks, so IIS_MASTER stays clear and IISCLK is left alone.
 */
#define IISCONFIG_ADDR     0x70002800
#define IISCLK_ADDR        0x70002808
#define IISFIFO_CFG_ADDR   0x7000280C
#define IISFIFO_WR_ADDR    0x70002840  /* 32-bit packed [R<<16 | L] TX port */
#define IISFIFO_RD_ADDR    0x70002880

/* IISCONFIG bits */
#define IIS_RESET              0x80000000  /* bit 31: soft-reset pulse       */
#define IIS_TXFIFOEN           0x20000000  /* bit 29: enable TX / transmit   */
#define IIS_RXFIFOEN           0x10000000  /* bit 28: enable RX              */
#define IIS_MASTER             0x02000000  /* bit 25: SoC masters (NOT iPod) */
#define IIS_FORMAT_MASK        0x00000C00  /* bits 11:10                     */
#define IIS_FORMAT_IIS         0x00000000  /* standard I2S                   */
#define IIS_SIZE_MASK          0x00000300  /* bits 9:8                       */
#define IIS_SIZE_16BIT         0x00000000  /* 16-bit samples                 */
#define IIS_FIFO_FORMAT_MASK   0x00000070  /* bits 6:4                       */
#define IIS_FIFO_FORMAT_LE16_2 0x00000070  /* the PP502x-programmed value    */
#define IIS_IRQTX              0x00000002  /* bit 1                          */
#define IIS_IRQRX              0x00000001  /* bit 0                          */

/* IISFIFO_CFG bits */
#define IISFIFO_CFG_TXFREE_SHIFT 16         /* TX-free count in bits 21:16   */
#define IISFIFO_CFG_TXFREE_MASK  0x0000003F /* 6-bit count, after shift      */
#define IIS_RXCLR              0x00001000   /* bit 12: flush RX FIFO         */
#define IIS_TXCLR              0x00000100   /* bit 8:  flush TX FIFO         */
#define IIS_RX_FULL_LVL_12     0x00000030   /* RX attention at 12 slots      */
#define IIS_TX_EMPTY_LVL_4     0x00000001   /* TX request at 4 free slots    */
#define IIS_TX_FIFO_DEPTH      16           /* derived: TX_FREE tops at 16   */

/* ---------- On-SoC DMA engine (continuous playback — follow-up) ------
 * core/docs/hw/05-audio.md, "DMA engine". NOT used by the polled
 * first-sound path; recorded now so the continuous-playback driver has
 * a single source. Master control at 0x6000A000; per-channel file at
 * 0x6000B000 (stride 0x20, channel 0 = playback). The CMD size field is
 * written as (bytes - 4). Untested until the playback driver lands.
 */
#define DMA_MASTER_CONTROL_ADDR 0x6000A000
#define DMA_MASTER_CONTROL_EN   0x80000000  /* bit 31 */
#define DMA_MASTER_STATUS_ADDR  0x6000A004  /* per-channel done bits */
#define DMA_MASTER_STATUS_CH0   0x01000000  /* bit 24: channel-0 done */
#define DMA_REQ_STATUS_ADDR     0x6000A008
#define DMA0_CMD_ADDR           0x6000B000
#define DMA0_STATUS_ADDR        0x6000B004  /* read clears the channel IRQ */
#define DMA0_RAM_ADDR_ADDR      0x6000B010
#define DMA0_FLAGS_ADDR         0x6000B014
#define DMA0_PER_ADDR_ADDR      0x6000B018
#define DMA0_INCR_ADDR          0x6000B01C

#define DMA_REQ_IIS             2            /* IIS request id (REQ_ID field) */
#define DMA_CMD_START           0x80000000   /* bit 31 */
#define DMA_CMD_INTR            0x40000000   /* bit 30 */
#define DMA_CMD_RAM_TO_PER      0x08000000   /* bit 27 */
#define DMA_CMD_SINGLE          0x04000000   /* bit 26 */
#define DMA_CMD_WAIT_REQ        0x01000000   /* bit 24 */
#define DMA_CMD_REQ_ID_POS      16           /* REQ_ID in bits 19:16 */
#define DMA_CMD_SIZE_MASK       0x0000FFFF   /* byte-count field (bits 15:0) */
#define DMA_SIZE_BIAS           4            /* SIZE field = bytes - 4 */
#define DMA_PLAY_CONFIG         0x4D020000   /* INTR|RAM_TO_PER|SINGLE|WAIT_REQ|(IIS<<16) */
#define DMA_INCR_PLAY           0x20010000   /* RANGE_FIXED(1<<16) | WIDTH_32BIT(2<<28) */
#define DMA_FLAGS_PLAY          0x04000000   /* DMA0_FLAGS bit 26 (Rockbox sets unconditionally) */
#define DMA0_STATUS_INTR        0x40000000   /* bit 30: completion flag */
#define DMA0_STATUS_BUSY        0x80000000   /* bit 31: channel busy */
#define DMA_IRQ                 26           /* CPU int source; DMA_MASK = 1<<26 */
#define DMA_MASK                0x04000000   /* 1 << DMA_IRQ */

/* ---------- ATA / IDE storage controller (0xC3000000) ---------------
 * core/docs/hw/04-ata.md. Integrated PIO/UDMA IDE controller. The
 * chainloading bootloader already powered, spun, and PIO-timed the drive
 * (it read our image off the FAT32 partition), so our reader reuses that
 * config: no power/reset/IDENTIFY, and DO NOT rewrite IDE0_PRI_TIMING
 * (those PIO values are CPU-frequency-dependent). On PP502x the task
 * registers are plain accesses — the PP5002 IDE_CFG write-complete
 * handshake does NOT apply. Control registers are 8-bit; the data port is
 * 16-bit (256 halfwords per 512-byte sector, little-endian, no swap).
 */
#define IDE_BASE_ADDR         0xC3000000
#define ATA_DATA_ADDR         0xC30001E0  /* 16-bit data port              */
#define ATA_ERROR_ADDR        0xC30001E4  /* read: error (write: feature)  */
#define ATA_NSECTOR_ADDR      0xC30001E8  /* sector count (0 = 256)        */
#define ATA_SECTOR_ADDR       0xC30001EC  /* LBA[7:0]                      */
#define ATA_LCYL_ADDR         0xC30001F0  /* LBA[15:8]                     */
#define ATA_HCYL_ADDR         0xC30001F4  /* LBA[23:16]                    */
#define ATA_SELECT_ADDR       0xC30001F8  /* device/head + LBA[27:24]      */
#define ATA_COMMAND_ADDR      0xC30001FC  /* write: command; read: status  */
#define ATA_ALT_STATUS_ADDR   0xC30003F8  /* read: status (no side effect);
                                           * write: device control         */
#define ATA_CONTROL_ADDR      0xC30003F8

/* Status bits (ATA_COMMAND read / ATA_ALT_STATUS). */
#define ATA_STATUS_BSY        0x80
#define ATA_STATUS_RDY        0x40
#define ATA_STATUS_DF         0x20
#define ATA_STATUS_DRQ        0x08
#define ATA_STATUS_ERR        0x01
/* Error bits (ATA_ERROR read). */
#define ATA_ERROR_IDNF        0x10  /* ID not found / bad LBA */
/* Device control bits. */
#define ATA_CONTROL_NIEN      0x02  /* mask the ATA IRQ (pure polling) */
#define ATA_CONTROL_SRST      0x04  /* software reset */
/* ATA_SELECT (device/head) bits. Bits 7 and 5 are "obsolete" in the ATA
 * spec but historically must be 1 — a drive left in CHS mode reads LBA 0
 * as CHS sector 0 (invalid -> IDNF), so the LBA-mode device byte must be
 * ATA_SELECT_OBS | ATA_SELECT_LBA (= 0xE0) for the master, not 0x40. */
#define ATA_SELECT_OBS        0xA0  /* obsolete must-be-1 bits (7,5) + master */
#define ATA_SELECT_LBA        0x40  /* LBA-mode bit (6) */
/* Commands. */
#define ATA_CMD_READ_SECTORS  0x20  /* LBA28 PIO read (one DRQ per sector) */
#define ATA_CMD_IDENTIFY      0xEC  /* IDENTIFY DEVICE (256-word block, no LBA) */

#endif /* CORE_HAL_HW_PP5022_H */
