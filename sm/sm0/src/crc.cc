/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * crc.cc -- CRC32C (include/sm0_crc.h), by a table of 256 entries,
 * reflected, as iSCSI and S3 define it.
 */

#include "sm0_crc.h"

#include <array>

namespace {

/* The reflected Castagnoli polynomial. */
constexpr uint32_t poly = 0x82F63B78u;

/* The CRC of each byte value, made at compile time. */
constexpr std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<uint32_t, 256> table = make_table();

} // namespace

/* Extend crc over n bytes at p; see sm0_crc.h. */
uint32_t sm0::crc32c(uint32_t crc, const void *p, size_t n) {
  const unsigned char *b = static_cast<const unsigned char *>(p);
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; i++) {
    c = table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  }
  return ~c;
}
