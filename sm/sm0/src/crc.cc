/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * crc.cc -- CRC32C (include/sm0_crc.h), reflected, as iSCSI and S3
 * define it: by the machine's instruction where it has one, ARMv8's
 * or SSE4.2's, else by a table of 256 entries. Both give the same.
 */

#include "sm0_crc.h"

#include "crc.h"

#include <array>
#include <string.h>

#if defined(__aarch64__) && defined(__linux__) &&                              \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SM0_CRC_ARMV8
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__x86_64__)
#define SM0_CRC_SSE42
#include <nmmintrin.h>
#endif

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

#if defined(SM0_CRC_ARMV8)

/* crc32c_by_cpu's, by ARMv8's CRC32 extension. */
[[gnu::target("+crc")]] uint32_t crc32c_armv8(uint32_t crc, const void *p,
                                              size_t n) {
  const unsigned char *b = static_cast<const unsigned char *>(p);
  uint32_t c = ~crc;
  for (; n >= 8; b += 8, n -= 8) {
    uint64_t x;
    memcpy(&x, b, 8);
    c = __crc32cd(c, x);
  }
  for (; n > 0; b++, n--) {
    c = __crc32cb(c, *b);
  }
  return ~c;
}

#elif defined(SM0_CRC_SSE42)

/* crc32c_by_cpu's, by SSE4.2. */
[[gnu::target("sse4.2")]] uint32_t crc32c_sse42(uint32_t crc, const void *p,
                                                size_t n) {
  const unsigned char *b = static_cast<const unsigned char *>(p);
  uint64_t c = static_cast<uint32_t>(~crc);
  for (; n >= 8; b += 8, n -= 8) {
    uint64_t x;
    memcpy(&x, b, 8);
    c = _mm_crc32_u64(c, x);
  }
  uint32_t r = static_cast<uint32_t>(c);
  for (; n > 0; b++, n--) {
    r = _mm_crc32_u8(r, *b);
  }
  return ~r;
}

#endif

} // namespace

/* By the table; see crc.h. */
uint32_t sm0_internal::crc32c_by_table(uint32_t crc, const void *p, size_t n) {
  const unsigned char *b = static_cast<const unsigned char *>(p);
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; i++) {
    c = table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  }
  return ~c;
}

/* By the machine's instruction, or nullptr; see crc.h. */
sm0_internal::crc32c_fn sm0_internal::crc32c_by_cpu() {
#if defined(SM0_CRC_ARMV8)
  return getauxval(AT_HWCAP) & HWCAP_CRC32 ? crc32c_armv8 : nullptr;
#elif defined(SM0_CRC_SSE42)
  return __builtin_cpu_supports("sse4.2") ? crc32c_sse42 : nullptr;
#else
  return nullptr;
#endif
}

/* Extend crc over n bytes at p; see sm0_crc.h. */
uint32_t sm0::crc32c(uint32_t crc, const void *p, size_t n) {
  // The machine is asked once a process.
  static const sm0_internal::crc32c_fn by_cpu = sm0_internal::crc32c_by_cpu();
  return by_cpu ? by_cpu(crc, p, n) : sm0_internal::crc32c_by_table(crc, p, n);
}
