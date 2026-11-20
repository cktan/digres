/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * sm0_crc.h -- CRC32C, the Castagnoli polynomial: the checksum of
 * the Column Store's files (../../COLSTORE.md §5.3), the one S3
 * also computes (x-amz-checksum-crc32c).
 */
#ifndef SM0_CRC_H
#define SM0_CRC_H

#include <stddef.h>
#include <stdint.h>

#include <string_view>

namespace sm0 {

/*
 * Extend crc, the CRC32C of what came before, over n bytes at p. A
 * first call passes 0. crc32c(crc32c(0, a), b) is the CRC32C of a
 * then b.
 */
uint32_t crc32c(uint32_t crc, const void *p, size_t n);

/* The CRC32C of the bytes of s. */
inline uint32_t crc32c(std::string_view s) {
  return crc32c(0, s.data(), s.size());
}

} // namespace sm0

#endif /* SM0_CRC_H */
