/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_crc.cc -- CRC32C (include/sm0_crc.h).
 */

#include "doctest.h"

#include "crc.h"
#include "sm0_crc.h"

#include <random>
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

// Verify that the machine's instruction gives what the table gives.
// 1. Skip it where the machine has none: the table is then crc32c.
// 2. Each length from 0 to 64 at each alignment of 8, from 0 and from
//    another CRC.
// 3. A large buffer, whole, and extended at each of its first cuts.
TEST_CASE("crc32c by the machine is crc32c by the table") {
  // 1. The machine's.
  const sm0_internal::crc32c_fn by_cpu = sm0_internal::crc32c_by_cpu();
  if (nullptr == by_cpu) {
    MESSAGE("no CRC32C instruction: the table alone is tested");
    return;
  }
  std::mt19937 rng(3);
  std::string s;
  for (int i = 0; i < 100003; i++) {
    s.push_back(static_cast<char>(rng()));
  }
  // 2. Lengths and alignments.
  for (size_t at = 0; at < 8; at++) {
    for (size_t n = 0; n <= 64; n++) {
      for (uint32_t crc : {0u, 1u, 0xE3069283u, 0xFFFFFFFFu}) {
        CHECK(by_cpu(crc, s.data() + at, n) ==
              sm0_internal::crc32c_by_table(crc, s.data() + at, n));
      }
    }
  }
  // 3. A large buffer.
  const uint32_t whole = sm0_internal::crc32c_by_table(0, s.data(), s.size());
  CHECK(by_cpu(0, s.data(), s.size()) == whole);
  CHECK(sm0::crc32c(s) == whole);
  for (size_t cut = 0; cut < 40; cut++) {
    const uint32_t part = by_cpu(0, s.data(), cut);
    CHECK(by_cpu(part, s.data() + cut, s.size() - cut) == whole);
  }
}
