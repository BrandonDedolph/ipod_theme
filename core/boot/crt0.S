/*
 * core/boot/crt0.S — ARM7TDMI startup for the iPod Video.
 *
 * The Apple boot ROM enters our OSOS image directly (we ARE the OSOS —
 * there is no chainloader any more; see core/docs/hw/08-boot-dock.md) with
 * (core/docs/hw/08-boot-dock.md, "Boot ROM → our code: handoff state"):
 *   - SDRAM at its NATIVE address (0x10000000) — NOT yet remapped to
 *     0x00000000. We are loaded into and entered in that native window,
 *     while linker.ld links every symbol at 0x0-based addresses, so
 *     performing the remap is OUR job and everything before the remap
 *     completes must be position-independent.
 *   - MMU off, cache off
 *   - IRQ + FIQ disabled, CPSR = 0xD3 (supervisor mode)
 *   - Stack pointer undefined
 *   - Vector table still pointing at boot ROM (we don't enable IRQs yet)
 *
 * Startup sequence (08-boot-dock.md, "Our crt0.S job", steps 1, 2, 4, 7):
 *   1. Temporary stack at the top of usable IRAM.
 *   2. Remap SDRAM to 0x00000000 via the MMAP0 registers, executing the
 *      remap from an IRAM-resident stub so the code doing the remap is
 *      not itself affected by it (08-boot-dock.md step 2;
 *      01-soc-pp5022.md, "Memory remap (MMAP0)").
 *   3. Zero .bss, set up the IRQ-mode stack, call kernel_main, hang.
 *
 * The 0x0-based exception vector table forms the first 32 bytes of the
 * image (it IS _start), and the banked IRQ/abort/undefined/FIQ stacks are
 * established in reset_handler; IRQs are left MASKED for the C side to
 * unmask after timer_init. The first-level IRQ entry stub
 * (irq_vector_entry) is a leaf ISR that calls the extern C dispatcher
 * irq_dispatch (kernel/irq.c). Every OTHER exception vectors to a fault
 * stub (see "FAULT VECTORS" near the end of this file) that captures the
 * faulting register state and calls panic() in kernel/panic.c, and
 * reset_handler paints the supervisor stack so panic can report its
 * high-water mark.
 *
 * No .data copy is needed (doc step 3 doesn't apply to us): the
 * bootloader loads the entire image into SDRAM and linker.ld links with
 * VMA == LMA, so .data is already in place at its final address. Cache
 * init and COP wake come in subsequent PRs as the kernel grows.
 */

    .section .text.startup, "ax"
    .arm
    .global _start

    /* ===================================================================
     * ARM7TDMI exception vector table — the first 32 bytes of the image.
     * _start IS the table, so _start stays at offset 0x0, which is what
     * both the layout check and the bootloader's entry-at-offset-0 rely on
     * (the table and the entry are the same bytes).
     *
     * Reset (0x00) is a PC-RELATIVE `b`: at bootloader entry SDRAM is still
     * at native 0x10000000 and the MMAP0 remap to 0x0 has NOT happened, so
     * an absolute `ldr pc,[pc,#..]` loading a 0x0-based address would jump
     * into unmapped space and brick. `b` is PC-relative and safe pre-remap.
     *
     * The other seven slots (0x04..0x1C) use `ldr pc, <literal>` loading
     * ABSOLUTE 0x0-based handler addresses from the explicit `.word` pool
     * placed immediately after the slots (NOT `ldr pc,=sym`, which would
     * let the literal pool drift out of the [pc,#imm] +/-4KB range and
     * reorder the table). These only ever fire AFTER remap + IRQ-unmask,
     * when 0x0-based addresses are live.
     * =================================================================== */
_start:
    b       reset_handler           @ 0x00 reset  (PC-relative, pre-remap safe)
    ldr     pc, _v_undef            @ 0x04 undefined instruction
    ldr     pc, _v_swi              @ 0x08 software interrupt (SWI)
    ldr     pc, _v_pabt             @ 0x0C prefetch abort
    ldr     pc, _v_dabt             @ 0x10 data abort
    ldr     pc, _v_resv             @ 0x14 reserved
    ldr     pc, _v_irq              @ 0x18 IRQ
    ldr     pc, _v_fiq              @ 0x1C FIQ

    /* Absolute (0x0-based) handler literals — kept adjacent to the slots
     * so every `ldr pc` above stays in [pc,#imm] range and the table does
     * not get reordered by an implicit literal pool. */
_v_undef:   .word undef_vector
_v_swi:     .word swi_vector
_v_pabt:    .word pabt_vector
_v_dabt:    .word dabt_vector
_v_resv:    .word resv_vector
_v_fiq:     .word fiq_vector
_v_irq:     .word irq_vector_entry

    /* Deterministic park. Only reached if panic() itself returns (it is
     * _Noreturn) — a tight self-branch so we stop here rather than running
     * off into garbage. */
trap_hang:
    b       trap_hang

    /* ===================================================================
     * RESET PATH. Reached via the reset slot's `b` above. This is the
     * entire original crt0 startup body, unchanged.
     * =================================================================== */
reset_handler:
    /* ===================================================================
     * PRE-REMAP. The PC is somewhere in SDRAM's native 0x10000000 window
     * but all symbols are linked at 0x0-based addresses. Until the MMAP0
     * remap below takes effect, nothing here may dereference or branch to
     * a link-time SDRAM address — PC-relative and fixed hardware
     * constants only.
     * =================================================================== */

    /* Re-assert supervisor mode + IRQ/FIQ masked. The bootloader already
     * leaves us this way; the explicit msr is defensive and ~free.
     * (Immediate-only, no memory access — position-independent.) */
    msr     cpsr_c, #0xD3

    /* Park the COP. The handoff contract says only the CPU runs, but
     * Rockbox parks the second core at entry defensively — and the
     * clicky emulator's HLE boot really does start both cores, where
     * an unparked COP races the CPU through BSS-zero and uart_init
     * (caught 2026-06-11; doubled UART output). PROCESSOR_ID low byte
     * is 0x55 on the CPU, 0xAA on the COP (01-soc-pp5022.md, "Dual
     * core: CPU and COP"). The COP is now put into COP_CTL sleep
     * (PROC_SLEEP = 0x80000000, sleep until interrupt) rather than
     * merely busy-spinning — clicky models this (a slept core is not
     * stepped), so the emulator's dual-core HLE boot no longer doubles
     * UART output (verified 2026-07-17). A busy-spin `cop_park` follows
     * as a backstop in case of a spurious wake before mailbox IPC
     * exists. (01-soc-pp5022.md, "Dual core: CPU and COP" -> "Sleep /
     * wake": CPU_CTL = 0x60007000, COP_CTL = 0x60007004; a write to a
     * PROC_CTL register must be followed by 3 NOPs for the pipeline.)
     * (Immediates + fixed MMIO only — position-independent.) */
    mov     r0, #0x60000000
    ldr     r0, [r0]
    and     r0, r0, #0xFF
    cmp     r0, #0x55
    beq     cpu_start

    /* COP: sleep via its control register until an interrupt. Build
     * COP_CTL (0x60007004) as a 0x60007000 base + [#4] offset, and
     * PROC_SLEEP (0x80000000) as a single rotated immediate. */
    mov     r0, #0x60000000
    orr     r0, r0, #0x00007000     /* r0 = 0x60007000 (PROC_CTL base) */
    mov     r1, #0x80000000
    str     r1, [r0, #4]            /* COP_CTL (base+4 = 0x60007004) = PROC_SLEEP */
    nop
    nop
    nop
cop_park:
    b       cop_park

cpu_start:
    /* Step 1 (08-boot-dock.md): temporary stack at the top of usable
     * IRAM, growing downward. IRAM is 0x40000000..0x4001FFFF (128 KB);
     * 96 KB usable to user code per core/docs/hw/01-soc-pp5022.md.
     * Safe pre-remap: the ldr is a PC-relative literal load, and the
     * VALUE fetched (_stack_top, from linker.ld) is an IRAM address —
     * IRAM's mapping is unaffected by the SDRAM remap. */
    ldr     sp, =_stack_top

    /* Step 2 (08-boot-dock.md): remap SDRAM to 0x00000000, "using
     * IRAM-resident remap code so we don't pull the rug out from under
     * ourselves" (08-boot-dock.md step 2, after Rockbox crt0-pp.S
     * 84-107). Copy the stub below to IRAM base and jump to it.
     * adr is PC-relative; 0x40000000 is a fixed hardware constant,
     * not a link-time address. */
    adr     r0, remap_stub
    adr     r1, remap_stub_end
    mov     r2, #0x40000000
1:
    cmp     r0, r1
    ldrlo   r3, [r0], #4
    strlo   r3, [r2], #4
    blo     1b
    mov     r2, #0x40000000
    mov     pc, r2

    /* SDRAM remap stub — copied to, and executed from, IRAM 0x40000000.
     * Self-contained and position-independent: constants are built with
     * mov/orr immediates, and its one literal (the post-remap target)
     * sits inside the copied region.
     *
     * Register programming per core/docs/hw/01-soc-pp5022.md, "Memory
     * remap (MMAP0)", verified against Rockbox crt0-pp.S + pp5020.h
     * (2026-06-10, master):
     *   MMAP0_LOGICAL (0xF000F000) = 0x00003C00 — logical base 0x0 OR
     *     the window mask. The size mask lives HERE, not in the physical
     *     flags: 0x3C00 = 64 MB window, 0x3E00 = 32 MB. Rockbox builds
     *     iPod Video with the 64 MB mask unconditionally (32 MB 5Gs
     *     included; actual RAM size is probed at runtime, not via MMAP),
     *     so we do the same. Writing 0 here (no mask bits) is a value no
     *     known firmware ever writes — undefined behavior.
     *   MMAP0_PHYSICAL (0xF000F004) = 0x10000F84 — physical base OR
     *     flags 0x0F84. 0x0F00 = read|write|data|code permission bits;
     *     0x84 is undocumented but present in every known user. 0x0F84
     *     is confirmed correct for PP502x (0x3F84 is the PP5002 value).
     * Logical is written first, physical second — the order Rockbox has
     * shipped on this silicon for two decades. Both writes execute from
     * IRAM, so the transient half-programmed mapping never bites. */
remap_stub:
    mov     r0, #0xF0000000
    orr     r0, r0, #0x0000F000     /* r0 = 0xF000F000 (MMAP0_LOGICAL) */
    mov     r2, #0x00003C00
    str     r2, [r0]                /* MMAP0_LOGICAL  = 0x3C00: base 0 | 64M mask */
    mov     r1, #0x10000000
    orr     r1, r1, #0x00000F00
    orr     r1, r1, #0x00000084     /* r1 = 0x10000F84: phys | flags 0x0F84 */
    str     r1, [r0, #4]            /* MMAP0_PHYSICAL = 0x10000F84 */
    /* Enter the remapped image: load the link-time (0x0-based) address
     * of post_remap into pc. The ldr itself is PC-relative into the
     * stub's own copied literal; only the loaded VALUE is absolute, and
     * by now SDRAM-at-0x0 is live. */
    ldr     pc, remap_stub_target
remap_stub_target:
    .word   post_remap
remap_stub_end:

    /* ===================================================================
     * POST-REMAP. Executing at link addresses; SDRAM is at 0x00000000.
     * Absolute (link-time) addresses are fine from here on.
     * =================================================================== */
post_remap:
    /* Zero .bss. Aligned 4-byte stores; the linker script asserts
     * (__bss_end - __bss_start) is word-multiple by aligning the section. */
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b

    /* Set up every banked stack pointer, then return to supervisor. We are
     * post-remap and running at link addresses, so `ldr sp,=` literals now
     * resolve to live IRAM addresses. SP is banked per mode, so each mode
     * has to be entered (I+F still masked) to load its own SP. IRQs stay
     * MASKED here — the C side unmasks after timer_init. (The earlier
     * `ldr sp,=_stack_top` at cpu_start ran on the temp/remap stack; this
     * re-load is harmless and just re-establishes SP_svc post-remap.)
     *
     * The abort / undefined / FIQ stacks are what make the fault handlers
     * below possible at all: a handler entered with an undefined SP cannot
     * even stash the faulting registers. Sizes + layout: boot/linker.ld. */
    msr     cpsr_c, #0xD7           @ abort mode     (0x17 | 0xC0)
    ldr     sp, =_abt_stack_top
    msr     cpsr_c, #0xDB           @ undefined mode (0x1B | 0xC0)
    ldr     sp, =_und_stack_top
    msr     cpsr_c, #0xD1           @ FIQ mode       (0x11 | 0xC0)
    ldr     sp, =_fiq_stack_top
    msr     cpsr_c, #0xD2           @ IRQ mode, I+F masked (0x12 | 0xC0)
    ldr     sp, =_irq_stack_top
    msr     cpsr_c, #0xD3           @ supervisor mode, I+F masked
    ldr     sp, =_stack_top

    /* Paint the supervisor stack with STACK_PAINT_WORD (kernel/panic.h) so
     * kernel/panic.c can measure the high-water mark and detect a breach of
     * the floor. The region is [_stack_limit, _stack_top) and SP is exactly
     * _stack_top with nothing live below it, so painting it here — after
     * .bss zero, before the first C call — clobbers nothing. ~89 KB of
     * word stores, a few ms at 30 MHz, paid once at boot. */
    ldr     r0, =_stack_limit
    ldr     r1, =_stack_top
    ldr     r2, =0xA5A55A5A
4:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     4b

    /* Hand off to C. */
    bl      kernel_main

    /* kernel_main is _Noreturn; if it ever does return, park. */
3:
    b       3b

    /* ===================================================================
     * First-level IRQ entry. Leaf ISR — no context switch (the scheduler
     * is cooperative). Vectored to from slot 0x18 via _v_irq. Fixes up the
     * IRQ return address, saves the caller-saved (AAPCS) registers plus the
     * return address, calls the C dispatcher, then restores and returns,
     * with `^` on the ldm restoring CPSR from SPSR_irq. `irq_dispatch` is
     * an extern C function (kernel/irq.c) supplied by another agent.
     * =================================================================== */
    .global irq_vector_entry
irq_vector_entry:
    sub     lr, lr, #4              @ IRQ return-address fixup
    @ Save an EVEN number of registers (6) so sp stays 8-byte aligned at
    @ the bl below, per AAPCS — do not add/remove one without padding.
    stmfd   sp!, {r0-r3, r12, lr}   @ save caller-saved + return
    bl      irq_dispatch            @ C dispatcher (extern, kernel/irq.c)
    ldmfd   sp!, {r0-r3, r12, pc}^  @ restore + return, ^ = CPSR <- SPSR

    /* ===================================================================
     * FAULT VECTORS. Every non-IRQ exception used to vector to `trap_hang`,
     * so a wild-pointer data abort, a cyclic FAT chain, a wedged BCM and a
     * stuck ATA spin were bit-identical on device: one frozen screen. There
     * is no serial cable on this unit, so telling them apart is the whole
     * difference between a 10-minute bug and an unfixable one.
     *
     * Each vector now stashes its (banked) LR and SPSR plus a cause code,
     * switches to abort mode and its dedicated panic stack (boot/linker.ld),
     * builds a panic_regs_t frame and calls the C panic() in kernel/panic.c,
     * which dumps it over UART *and* onto the panel.
     *
     * RETURN-ADDRESS CORRECTION (ARM7TDMI, ARM ARM "Exceptions"). The core
     * saves into LR_<mode> the address of the instruction AFTER the one that
     * was executing, plus the pipeline offset for that exception class, so
     * the address of the OFFENDING instruction is:
     *   - data abort:      LR - 8  (abort is taken one instruction late: the
     *                               load/store had already left the execute
     *                               stage when the memory system reported)
     *   - prefetch abort:  LR - 4  (LR points just past the instruction whose
     *                               FETCH aborted)
     *   - undefined instr: LR - 4  (LR points just past the undefined opcode)
     *   - SWI:             LR - 4  (LR points just past the SWI itself)
     *   - FIQ / reserved:  LR - 4  (asynchronous: LR-4 is the instruction that
     *                               would have run next, not a "faulting" one)
     * We apply exactly those and report the corrected value as `pc`, so the
     * dumped PC can be looked up straight in core.map / the disassembly.
     *
     * r0-r12 are NOT banked (except r8-r12 in FIQ mode), so after the mode
     * switch they still hold the faulting context's values and are dumped
     * as-is. In the FIQ case r8-r12 read as the INTERRUPTED context's — the
     * FIQ handler's own banked copies are not recoverable here, which is the
     * useful direction anyway.
     * =================================================================== */

    /* Cause codes — MUST match the PANIC_* defines in kernel/panic.h. */
    .equ    PANIC_UNDEF, 1
    .equ    PANIC_SWI,   2
    .equ    PANIC_PABT,  3
    .equ    PANIC_DABT,  4
    .equ    PANIC_RESV,  5
    .equ    PANIC_FIQ,   6

    /* One vector stub: correct LR, stash {spsr, cause, r0, r1, pc} on THIS
     * mode's own banked stack (5 words — that is all those 256/512-byte
     * stash stacks ever hold), hand fault_common a pointer to it. */
    .macro  FAULT_STUB name, cause, adj
\name:
    sub     lr, lr, #\adj           @ return-address correction (see above)
    stmfd   sp!, {r0, r1, lr}       @ [8]=r0 [12]=r1 [16]=faulting pc
    mrs     r0, spsr
    mov     r1, #\cause
    stmfd   sp!, {r0, r1}           @ [0]=spsr [4]=cause
    mov     r0, sp                  @ r0 -> stash (r0/r1 now free: both saved)
    b       fault_common
    .endm

    .global undef_vector
    .global swi_vector
    .global pabt_vector
    .global dabt_vector
    .global resv_vector
    .global fiq_vector
    FAULT_STUB undef_vector, PANIC_UNDEF, 4
    FAULT_STUB swi_vector,   PANIC_SWI,   4
    FAULT_STUB pabt_vector,  PANIC_PABT,  4
    FAULT_STUB dabt_vector,  PANIC_DABT,  8
    FAULT_STUB resv_vector,  PANIC_RESV,  4
    FAULT_STUB fiq_vector,   PANIC_FIQ,   4

    /* Common tail. On entry r0 -> the stub's 5-word stash and we are still in
     * the exception's own mode. Builds a panic_regs_t (kernel/panic.h) on the
     * dedicated panic stack — laid out ascending as r0..r12, sp, lr, pc, cpsr,
     * cause — and calls panic(cause, &regs). The stash lives in a DIFFERENT
     * IRAM region than the panic stack, so reloading SP_abt cannot eat it. */
fault_common:
    msr     cpsr_c, #0xD7           @ abort mode, I+F masked: the panic mode
    ldr     sp, =_panic_stack_top   @ dedicated stack (the faulting one may be
                                    @ exactly what is broken)
    ldr     r1, [r0, #4]            @ cause
    stmfd   sp!, {r1}
    ldr     r1, [r0, #0]            @ faulting CPSR (this exception's SPSR)
    stmfd   sp!, {r1}
    ldr     r1, [r0, #16]           @ corrected faulting PC
    stmfd   sp!, {r1}

    /* The faulting mode's own SP and LR are banked, so recover them by
     * briefly re-entering that mode (I+F kept masked). Two hops because only
     * r0 (the stash pointer) and r1 are free — every other register still
     * holds a faulting value we have not saved yet. Skipped when the SPSR
     * says user mode (entering it is a one-way trip: no SPSR to come back
     * through) or abort mode (a nested abort: SP_abt/LR_abt are now OURS,
     * not the faulting context's), reported as 0 in both cases. */
    ldr     r1, [r0, #0]
    and     r1, r1, #0x1F
    cmp     r1, #0x10               @ USR
    beq     5f
    cmp     r1, #0x17               @ ABT (nested)
    beq     5f
    orr     r1, r1, #0xC0
    msr     cpsr_c, r1
    mov     r1, lr                  @ faulting mode's LR
    msr     cpsr_c, #0xD7
    stmfd   sp!, {r1}
    ldr     r1, [r0, #0]
    and     r1, r1, #0x1F
    orr     r1, r1, #0xC0
    msr     cpsr_c, r1
    mov     r1, sp                  @ faulting mode's SP
    msr     cpsr_c, #0xD7
    stmfd   sp!, {r1}
    b       6f
5:
    mov     r1, #0
    stmfd   sp!, {r1}               @ lr unavailable
    stmfd   sp!, {r1}               @ sp unavailable
6:
    stmfd   sp!, {r2-r12}           @ frame[2..12]
    ldr     r1, [r0, #12]           @ faulting r1
    ldr     r0, [r0, #8]            @ faulting r0 (last use of the stash ptr)
    stmfd   sp!, {r0, r1}           @ frame[0..1]

    /* 18 words pushed from an 8-byte-aligned top, so SP is still 8-byte
     * aligned at the call, per AAPCS. Do not add or remove one word without
     * padding. */
    mov     r1, sp                  @ arg1 = &regs
    ldr     r0, [r1, #17*4]         @ arg0 = cause
    bl      panic                   @ kernel/panic.c, _Noreturn
    b       trap_hang               @ unreachable

    .size _start, . - _start
