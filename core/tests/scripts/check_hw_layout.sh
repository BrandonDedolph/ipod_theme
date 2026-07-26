#!/usr/bin/env sh
# check_hw_layout.sh — structural asserts on the linked firmware ELF.
#
# Confirms crt0.S / linker.ld produced the memory layout the boot code
# assumes. These are the invariants the MMIO trace tests cannot reach
# (that harness stops at the C register grammar; this covers the
# assembly/linker side): the reset entry sits at the vector base, the
# temp stack lands at the top of usable IRAM, and .bss is a word-aligned
# range the crt0 zero loop can walk safely. A linker-script regression
# or a dropped digit in the stack/remap constants trips this.
#
# Usage: check_hw_layout.sh <core.elf> [nm-tool]
# Exits non-zero on any failed assertion.

set -eu

ELF="${1:?usage: check_hw_layout.sh <core.elf> [nm]}"
NM="${2:-arm-none-eabi-nm}"

# Expected constants (must track hal/hw/pp5022.h + boot/linker.ld):
#   IRAM_BASE 0x40000000 + IRAM_USABLE_SIZE 0x18000 = 0x40018000
EXP_START="0"
# PR #6 split the top of IRAM into a banked IRQ stack + the supervisor
# stack below it: _irq_stack_top = IRAM top (0x40018000), _stack_top =
# 1 KB below (0x40017C00).
EXP_IRQ_STACK_TOP="40018000"
EXP_STACK_TOP="40017c00"
# The linker script is the authority, not the largest device we might run on.
# boot/linker.ld declares SDRAM LENGTH = 32M *deliberately*: the 5G has 32 MB
# and the 5.5G has 64 MB, and keeping the image inside the low 32 MB is what
# makes ONE image bootable on both. This check previously allowed 64 MB, which
# both encoded the wrong device and was weaker than the linker it is supposed to
# corroborate — a .bss between 32 and 64 MB would have failed the link while
# passing here. Keep this in step with linker.ld's MEMORY block.
SDRAM_WINDOW=$((0x02000000))   # 32 MB — must match boot/linker.ld SDRAM LENGTH

fails=0
note() { printf 'FAIL: %s\n' "$1"; fails=$((fails + 1)); }

sym() {  # sym <name> -> hex address (lowercase, no 0x), empty if absent
    "$NM" "$ELF" | awk -v n="$1" '$3 == n { print $1; exit }'
}

START=$(sym _start)
STACK=$(sym _stack_top)
IRQ_STACK=$(sym _irq_stack_top)
BSS_S=$(sym __bss_start)
BSS_E=$(sym __bss_end)
KMAIN=$(sym kernel_main)

[ -n "$KMAIN" ] || note "kernel_main symbol missing"

# _start must be the ARM reset vector at 0x0 (post-remap image base).
if [ "$((0x${START:-1}))" -ne "$((0x$EXP_START))" ]; then
    note "_start = 0x$START, expected 0x$EXP_START (vector base)"
fi

# Supervisor stack just below the IRQ stack; IRQ stack at the IRAM top.
if [ "$(printf '%s' "$STACK" | tr 'A-F' 'a-f')" != "$EXP_STACK_TOP" ]; then
    note "_stack_top = 0x$STACK, expected 0x$EXP_STACK_TOP"
fi
if [ "$(printf '%s' "$IRQ_STACK" | tr 'A-F' 'a-f')" != "$EXP_IRQ_STACK_TOP" ]; then
    note "_irq_stack_top = 0x$IRQ_STACK, expected 0x$EXP_IRQ_STACK_TOP"
fi

# .bss must be a word-aligned, non-negative range within SDRAM so the
# crt0 word-strided zero loop neither misaligns nor overruns.
if [ -n "$BSS_S" ] && [ -n "$BSS_E" ]; then
    s=$((0x$BSS_S)); e=$((0x$BSS_E))
    [ $((s % 4)) -eq 0 ] || note "__bss_start 0x$BSS_S not word-aligned"
    [ $((e % 4)) -eq 0 ] || note "__bss_end 0x$BSS_E not word-aligned"
    [ "$e" -ge "$s" ]    || note "__bss_end < __bss_start (0x$BSS_E < 0x$BSS_S)"
    [ "$e" -le "$SDRAM_WINDOW" ] || note "__bss_end 0x$BSS_E past the 32 MB SDRAM window (linker.ld)"
else
    note "__bss_start/__bss_end symbols missing"
fi

if [ "$fails" -eq 0 ]; then
    printf 'OK: firmware layout sane — _start=0x%s _stack_top=0x%s bss=[0x%s,0x%s)\n' \
        "$START" "$STACK" "$BSS_S" "$BSS_E"
    exit 0
fi
printf '%d layout assertion(s) failed\n' "$fails"
exit 1
