#!/usr/bin/env sh
# check_size.sh — firmware size / static-RAM budget gate.
#
# Runs arm-none-eabi-size on the linked ELF, prints the text/data/bss
# breakdown, and FAILS the build when a section is past its documented budget.
# Nothing else in the tree reports binary size, so a new multi-MB static buffer
# could (and did) grow .bss unnoticed until the device stopped booting.
#
# THE BUDGETS (and why they are these numbers):
#
#   boot/linker.ld declares  SDRAM (rwx) : ORIGIN = 0x0, LENGTH = 32M
#   — deliberately the low 32 MB so ONE image boots on both the 5G (32 MB) and
#   the 5.5G (64 MB). Everything below is carved out of that single 32 MB
#   window; the linker itself only fails once the total overflows, which is far
#   too late to be useful feedback.
#
#   TEXT_MAX  1 MB   — code+rodata. Currently ~226 KB. The headroom is for the
#                      atlas/rodata tables; a jump past 1 MB means something
#                      unintended (a vendored blob, debug tables) got linked in.
#   DATA_MAX  64 KB  — initialised .data is COPIED from the image at boot by
#                      crt0, so every byte costs both image size and RAM.
#                      Currently ~204 bytes; anything near 64 KB is a
#                      const-should-be-const bug.
#   BSS_MAX   14 MB  — static RAM. Currently ~10.1 MB, dominated by the
#                      player's 8 MB anti-skip disk buffer + the 1 MB PCM ring.
#                      14 MB leaves room for one more MB-scale buffer while
#                      keeping the whole image inside the low 32 MB window with
#                      margin for the stack and any future heap carve-out.
#   IMAGE_MAX 30 MB  — text+data+bss together, i.e. the highest address the
#                      image occupies. Hard-capped 2 MB below the 32 MB SDRAM
#                      window the linker declares.
#
# Usage: check_size.sh <core.elf> [size-tool] [stamp-file]
# Exits non-zero on any budget overrun. The report always goes to stdout (so
# `ninja` shows it); when a stamp path is given the same report is written there
# as well, which is what makes this a real build edge rather than a side effect.

set -eu

ELF="${1:?usage: check_size.sh <core.elf> [arm-none-eabi-size] [stamp]}"
SIZE="${2:-arm-none-eabi-size}"
STAMP="${3:-}"

TEXT_MAX=$((1024 * 1024))
DATA_MAX=$((64 * 1024))
BSS_MAX=$((14 * 1024 * 1024))
IMAGE_MAX=$((30 * 1024 * 1024))

# `size -A` is the Berkeley/sysv-agnostic form; the default (Berkeley) layout
# is "text data bss dec hex filename" on the second line.
line=$("$SIZE" "$ELF" | awk 'NR == 2 { print $1, $2, $3 }')
text=$(printf '%s' "$line" | awk '{ print $1 }')
data=$(printf '%s' "$line" | awk '{ print $2 }')
bss=$(printf  '%s' "$line" | awk '{ print $3 }')

if [ -z "$text" ] || [ -z "$data" ] || [ -z "$bss" ]; then
    printf 'check_size: could not parse `%s %s` output\n' "$SIZE" "$ELF" >&2
    exit 1
fi

total=$((text + data + bss))

pct() { printf '%d' $(( $1 * 100 / $2 )); }

report=$(
    printf 'firmware size (budget gate)\n'
    printf '  text  %10d B  / %10d B  (%s%%)\n' "$text"  "$TEXT_MAX"  "$(pct "$text" "$TEXT_MAX")"
    printf '  data  %10d B  / %10d B  (%s%%)\n' "$data"  "$DATA_MAX"  "$(pct "$data" "$DATA_MAX")"
    printf '  bss   %10d B  / %10d B  (%s%%)\n' "$bss"   "$BSS_MAX"   "$(pct "$bss" "$BSS_MAX")"
    printf '  image %10d B  / %10d B  (%s%%)  [32 MB SDRAM window]\n' \
           "$total" "$IMAGE_MAX" "$(pct "$total" "$IMAGE_MAX")"
)
printf '%s\n' "$report"
if [ -n "$STAMP" ]; then
    printf '%s\n' "$report" > "$STAMP"
fi

fails=0
over() {
    printf 'FAIL: %s is %d B, over the %d B budget (by %d B)\n' \
           "$1" "$2" "$3" "$(( $2 - $3 ))" >&2
    fails=$((fails + 1))
}

[ "$text"  -le "$TEXT_MAX"  ] || over ".text"          "$text"  "$TEXT_MAX"
[ "$data"  -le "$DATA_MAX"  ] || over ".data"          "$data"  "$DATA_MAX"
[ "$bss"   -le "$BSS_MAX"   ] || over ".bss"           "$bss"   "$BSS_MAX"
[ "$total" -le "$IMAGE_MAX" ] || over "text+data+bss"  "$total" "$IMAGE_MAX"

if [ "$fails" -ne 0 ]; then
    printf '%d size budget(s) exceeded — see tests/scripts/check_size.sh\n' \
           "$fails" >&2
    exit 1
fi
printf 'OK: within budget\n'
