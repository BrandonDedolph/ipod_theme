#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""check_resume_parity.py — keep the resume matcher's test copy honest.

resume_find_song() in core/kernel/main.c decides which library song the boot
path may re-open for a saved resume position. It is `static` inside a file that
also pulls in the UI, the FAT reader, the player and the HAL, so it cannot be
linked into a host test; core/tests/kernel/resume_test.c therefore holds a
VERBATIM COPY and tests that.

A copy that drifts is worse than no test — it passes while the shipped function
does something else. So this script asserts two things:

  A. the copy is byte-identical to main.c's;
  B. RESUME_DUR_SLOP, the one tunable the body reads, has the same value in
     both files (it is a #define, not part of the copied text, so a change in
     main.c alone would silently re-tune the shipped rule while the test kept
     asserting the old one).

Same pattern, and the same reasoning, as check_name_hash_parity.py.
"""

import difflib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
CORE = HERE.parent.parent.parent            # <repo>/core

MAIN_C = CORE / "kernel" / "main.c"
TEST_C = CORE / "tests" / "kernel" / "resume_test.c"

SIG = "static int resume_find_song("
SLOP_RE = re.compile(r"^#define\s+RESUME_DUR_SLOP\s+(\S+)", re.M)


def extract_func(path):
    """The source text of SIG's function: from its definition line to the
    first line that is exactly '}'. Holds for this codebase's style, and
    needing no C parser is the point."""
    lines = path.read_text(encoding="utf-8").split("\n")
    try:
        i = next(k for k, l in enumerate(lines) if l.startswith(SIG))
    except StopIteration:
        raise SystemExit(
            f"FAIL: {path}: no function starting with {SIG!r}.\n"
            "       It moved or was renamed — which is itself a parity break. "
            "Update SIG in tests/scripts/check_resume_parity.py and re-sync "
            "tests/kernel/resume_test.c."
        )
    j = i
    while lines[j] != "}":
        j += 1
    return "\n".join(lines[i:j + 1])


def extract_slop(path):
    m = SLOP_RE.search(path.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit(f"FAIL: {path}: no RESUME_DUR_SLOP definition.")
    return m.group(1)


def main():
    fails = 0

    orig, copy = extract_func(MAIN_C), extract_func(TEST_C)
    if orig != copy:
        fails += 1
        print(f"FAIL: resume_find_song() differs between\n"
              f"        {MAIN_C}\n      and the copy in\n"
              f"        {TEST_C}\n"
              "      The copy is what the host test compiles, so it must be "
              "re-pasted verbatim.", file=sys.stderr)
        for line in difflib.unified_diff(orig.split("\n"), copy.split("\n"),
                                         "main.c", "resume_test.c",
                                         lineterm=""):
            print("      " + line, file=sys.stderr)
    else:
        print(f"OK: resume_find_song() copy matches {MAIN_C.name}")

    a, b = extract_slop(MAIN_C), extract_slop(TEST_C)
    if a != b:
        fails += 1
        print(f"FAIL: RESUME_DUR_SLOP is {a} in {MAIN_C.name} but {b} in "
              f"{TEST_C.name} — the test would assert a rule the firmware no "
              f"longer applies.", file=sys.stderr)
    else:
        print(f"OK: RESUME_DUR_SLOP agrees ({a})")

    if fails:
        print(f"\n{fails} resume parity failure(s)", file=sys.stderr)
        return 1
    print("resume parity: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
