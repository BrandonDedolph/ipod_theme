/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tests/kernel/name_hash_ref.h — test-only entry point into the copied
 * name_hash() from core/kernel/main.c. See name_hash_ref.c for why it is a
 * copy and what keeps the copy honest.
 */
#ifndef CORE_TESTS_KERNEL_NAME_HASH_REF_H
#define CORE_TESTS_KERNEL_NAME_HASH_REF_H

#include <stdint.h>

/* Case/quote-folded FNV-1a-32 over a UTF-8 name — main.c's name_hash(). */
uint32_t name_hash_ref(const char *s);

#endif /* CORE_TESTS_KERNEL_NAME_HASH_REF_H */
