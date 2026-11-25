/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * crc.h -- the ways crc.cc computes CRC32C, for it and its tests.
 */
#ifndef SM0_CRC_INTERNAL_H
#define SM0_CRC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

namespace sm0_internal {

/* Extend crc over n bytes at p, as sm0::crc32c does. */
using crc32c_fn = uint32_t (*)(uint32_t crc, const void *p, size_t n);

/* By a table of 256 entries, a byte at a time. */
uint32_t crc32c_by_table(uint32_t crc, const void *p, size_t n);

/*
 * By this machine's CRC32C instruction, 8 bytes at a time: nullptr if
 * it has none, or the compiler none for it.
 */
crc32c_fn crc32c_by_cpu();

} // namespace sm0_internal

#endif /* SM0_CRC_INTERNAL_H */
