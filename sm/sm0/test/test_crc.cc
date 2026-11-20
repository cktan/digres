/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_crc.cc -- CRC32C (include/sm0_crc.h).
 */

#include "doctest.h"

#include "sm0_crc.h"

#include <string>

// Verify that crc32c gives the published check values, and extends.
// 1. Check the empty string and "123456789", whose CRC32C is the
//    standard check value 0xE3069283.
// 2. Check 32 zero bytes against RFC 3720's value, 0x8A9136AA.
// 3. Check that a CRC taken in two parts equals one taken whole.
TEST_CASE("crc32c gives the check values and extends") {
  // 1. The empty string, and the standard check string.
  CHECK(sm0::crc32c("") == 0u);
  CHECK(sm0::crc32c("123456789") == 0xE3069283u);
  // 2. RFC 3720, B.4: 32 bytes of zero.
  std::string zeros(32, '\0');
  CHECK(sm0::crc32c(zeros) == 0x8A9136AAu);
  // 3. In two parts.
  std::string s = "the quick brown fox jumps over the lazy dog";
  uint32_t whole = sm0::crc32c(s);
  uint32_t part = sm0::crc32c(0, s.data(), 10);
  part = sm0::crc32c(part, s.data() + 10, s.size() - 10);
  CHECK(part == whole);
}
