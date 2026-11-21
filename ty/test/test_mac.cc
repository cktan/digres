/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the macaddr and macaddr8 operations; the parse of text is
 * in the vectors (known.txt).
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_hash.h"
#include "ty_mac.h"

namespace {

/* A macaddr from its text, which must be valid. */
ty_macaddr_t mac(const char *s) {
  ty_macaddr_t a = {};

  REQUIRE(0 == ty_macaddr_parse(s, &a));
  return a;
}

/* A macaddr8 from its text, which must be valid. */
ty_macaddr8_t mac8(const char *s) {
  ty_macaddr8_t a = {};

  REQUIRE(0 == ty_macaddr8_parse(s, &a));
  return a;
}

/* The text of a macaddr. */
std::string text(const ty_macaddr_t &a) {
  char out[TY_MACADDR_TEXT_SIZE];

  ty_macaddr_format(&a, out);
  return out;
}

/* The text of a macaddr8. */
std::string text8(const ty_macaddr8_t &a) {
  char out[TY_MACADDR8_TEXT_SIZE];

  ty_macaddr8_format(&a, out);
  return out;
}

} // namespace

// Verify the bit operations and the truncation of a macaddr.
// 1. NOT inverts every byte; AND and OR work byte by byte.
// 2. trunc keeps the vendor's first 3 bytes.
// 3. The order is that of the bytes: 08:00:2b < 08:00:2c.
TEST_CASE("macaddr bit operations, trunc and order") {
  ty_macaddr_t a = mac("08:00:2b:01:02:03"), m = mac("ff:ff:ff:00:00:00");

  // 1. NOT inverts every byte; AND and OR work byte by byte.
  CHECK(text(ty_macaddr_not(&a)) == "f7:ff:d4:fe:fd:fc");
  CHECK(text(ty_macaddr_and(&a, &m)) == "08:00:2b:00:00:00");
  CHECK(text(ty_macaddr_or(&a, &m)) == "ff:ff:ff:01:02:03");

  // 2. trunc keeps the vendor's first 3 bytes.
  CHECK(text(ty_macaddr_trunc(&a)) == "08:00:2b:00:00:00");

  // 3. The order is that of the bytes: 08:00:2b < 08:00:2c.
  ty_macaddr_t b = mac("08:00:2c:00:00:00");
  CHECK(ty_macaddr_cmp(&a, &b) == -1);
  CHECK(ty_macaddr_cmp(&b, &a) == 1);
  CHECK(ty_macaddr_cmp(&a, &a) == 0);
}

// Verify the conversions between the two types, and set7bit.
// 1. A macaddr becomes a macaddr8 with FF:FE in the middle.
// 2. A macaddr8 with FF:FE there becomes a macaddr again.
// 3. One without is TY_ERR_RANGE.
// 4. set7bit sets bit 0x02 of the first byte.
TEST_CASE("macaddr and macaddr8 convert by FF:FE in the middle") {
  ty_macaddr_t a = mac("08:00:2b:01:02:03"), back;

  // 1. A macaddr becomes a macaddr8 with FF:FE in the middle.
  ty_macaddr8_t wide = ty_macaddr_to_macaddr8(&a);
  CHECK(text8(wide) == "08:00:2b:ff:fe:01:02:03");

  // 2. A macaddr8 with FF:FE there becomes a macaddr again.
  CHECK(0 == ty_macaddr8_to_macaddr(&wide, &back));
  CHECK(text(back) == "08:00:2b:01:02:03");

  // 3. One without is TY_ERR_RANGE.
  ty_macaddr8_t odd = mac8("08:00:2b:01:02:03:04:05");
  CHECK(-1 == ty_macaddr8_to_macaddr(&odd, &back));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 4. set7bit sets bit 0x02 of the first byte.
  CHECK(text8(ty_macaddr8_set7bit(&odd)) == "0a:00:2b:01:02:03:04:05");
  ty_macaddr8_t m = mac8("ff:ff:ff:00:00:00:00:00");
  CHECK(text8(ty_macaddr8_trunc(&odd)) == "08:00:2b:00:00:00:00:00");
  CHECK(text8(ty_macaddr8_not(&odd)) == "f7:ff:d4:fe:fd:fc:fb:fa");
  CHECK(text8(ty_macaddr8_and(&odd, &m)) == "08:00:2b:00:00:00:00:00");
  CHECK(text8(ty_macaddr8_or(&odd, &m)) == "ff:ff:ff:01:02:03:04:05");
}

// Verify that the hash is PostgreSQL's hash of the bytes.
// 1. The hash of a macaddr is that of its 6 bytes, and of a macaddr8 its 8.
// 2. Equal addresses hash alike; another address, another hash.
TEST_CASE("the macaddr hash is the hash of the bytes") {
  ty_macaddr_t a = mac("08:00:2b:01:02:03"), b = mac("08:00:2b:01:02:04");

  // 1. The hash of a macaddr is that of its 6 bytes, and of a macaddr8 its 8.
  CHECK(ty_macaddr_hash(&a, 0) == ty_hash_bytes(a.b, 6, 0));
  ty_macaddr8_t w = ty_macaddr_to_macaddr8(&a);
  CHECK(ty_macaddr8_hash(&w, 5) == ty_hash_bytes(w.b, 8, 5));

  // 2. Equal addresses hash alike; another address, another hash.
  CHECK(ty_macaddr_hash(&a, 0) == ty_macaddr_hash(&a, 0));
  CHECK(ty_macaddr_hash(&a, 0) != ty_macaddr_hash(&b, 0));
}
