/*
 * core/boot/linker.ld — link-time memory layout for the iPod Video image.
 *
 * Memory map (PP5022, see core/docs/hw/01-soc-pp5022.md):
 *   SDRAM @ 0x00000000 — code, rodata, data, bss. These are POST-REMAP
 *          addresses: at bootloader handoff SDRAM is still at its native
 *          0x10000000 and our crt0.S performs the MMAP0 remap to
 *          0x00000000 itself before relying on any address linked here
 *          (core/docs/hw/08-boot-dock.md, "Boot ROM → our code:
 *          handoff state"). Everything in crt0.S up to that point is
 *          position-independent.
 *   IRAM  @ 0x40000000 — stack (top-down), crt0's remap stub, and
 *          future fast paths
 *
 * The image entry point is _start (boot/crt0.S), placed first in .text
 * via the .text.startup section so it sits at offset 0 of the loaded
 * image — which is also what the bootloader's entryOffset = 0 expects.
 *
 * Size choices:
 *   SDRAM 32M — the 5G has 32 MB; the 5.5G has 64 MB but our image lives
 *               in the low 32 to keep one image bootable on both. Heap /
 *               codec buffer carving for the upper region lands later.
 *   IRAM  96K — full region is 128 KB; the top 32 KB is reserved for SoC
 *               scratch / future use per the hw doc.
 */

OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
    SDRAM (rwx) : ORIGIN = 0x00000000, LENGTH = 32M
    IRAM  (rwx) : ORIGIN = 0x40000000, LENGTH = 96K
}

SECTIONS {
    .text : ALIGN(4) {
        KEEP(*(.text.startup))
        *(.text*)
        *(.rodata*)
        *(.glue_7) *(.glue_7t)
        . = ALIGN(4);
    } > SDRAM

    .data : ALIGN(4) {
        *(.data*)
        . = ALIGN(4);
    } > SDRAM

    .bss (NOLOAD) : ALIGN(4) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    /DISCARD/ : {
        *(.note*)
        *(.ARM.attributes)
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.comment)
        *(.eh_frame*)
    }
}

/*
 * Stacks carved from IRAM, all growing downward. Top of IRAM down:
 *
 *   - IRQ stack:        [0x40017C00, 0x40018000)  — 1 KB at the very top,
 *                       banked SP_irq, set up in crt0.S reset_handler.
 *   - Supervisor stack: [_stack_limit, 0x40017C00) — SP_svc, the main C
 *                       stack (~89 KB). It has a FLOOR now: crt0.S paints
 *                       the whole region with STACK_PAINT_WORD
 *                       (0xA5A55A5A, kernel/panic.h) at boot, so
 *                       stack_stat() can report the high-water mark and
 *                       stack_guard_ok() can spot a breach of the bottom
 *                       guard band. Deep UI call chains hold several
 *                       char[NAME_MAX+1] frames, and before this there was
 *                       nothing at all marking where the stack ends.
 *   - Guard band:       [0x40001000, 0x40002000) — 4 KB of DEAD space
 *                       belonging to no stack. A modest supervisor
 *                       overflow lands here and is detected by the paint
 *                       check without having trashed the fault stacks the
 *                       panic path is about to run on.
 *   - Panic stack:      [0x40000800, 0x40001000) — 2 KB. The dedicated
 *                       abort-mode stack the fault handlers switch to
 *                       before calling panic() (kernel/panic.c): the
 *                       faulting context's own stack may be exactly what
 *                       is broken, so the dump must not touch it.
 *   - Fault entry stubs: three small banked stacks used ONLY to stash
 *                       {r0, r1, lr, spsr, cause} on the way into
 *                       fault_common — abort (512 B), undefined (256 B)
 *                       and FIQ (256 B) modes each need their own SP
 *                       because those registers are banked per mode.
 *   - [0x40000000, 0x40000400): crt0.S copies its SDRAM remap stub to the
 *                       IRAM base and runs it there at boot; left free.
 *
 * check_hw_layout.sh pins _stack_top / _irq_stack_top; the rest are new.
 */
_irq_stack_top   = ORIGIN(IRAM) + LENGTH(IRAM);   /* 0x40018000 */
_stack_top       = _irq_stack_top - 0x400;        /* 1 KB IRQ stack; svc top = 0x40017C00 */
_stack_limit     = ORIGIN(IRAM) + 0x2000;         /* 0x40002000 — svc stack FLOOR   */
_panic_stack_top = ORIGIN(IRAM) + 0x1000;         /* 0x40001000 — 2 KB panic stack  */
_abt_stack_top   = ORIGIN(IRAM) + 0x0800;         /* 0x40000800 — 512 B abort stash */
_und_stack_top   = ORIGIN(IRAM) + 0x0600;         /* 0x40000600 — 256 B undef stash */
_fiq_stack_top   = ORIGIN(IRAM) + 0x0500;         /* 0x40000500 — 256 B FIQ   stash */

ASSERT(_stack_top > _stack_limit,
       "linker.ld: supervisor stack region is inverted/empty")
ASSERT(_panic_stack_top < _stack_limit,
       "linker.ld: panic stack overlaps the supervisor stack")
ASSERT(_abt_stack_top < _panic_stack_top,
       "linker.ld: abort stash stack overlaps the panic stack")
