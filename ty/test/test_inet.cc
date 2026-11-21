/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the inet and cidr operations; the rest are in the vectors, and
 * the text in test/known.txt.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_inet.h"

namespace {

/* Parse a text as an inet; the value is checked by the caller. */
ty_inet_t inet(const char *s, int is_cidr = 0) {
  ty_inet_t a;

  REQUIRE(0 == ty_inet_parse(s, is_cidr, &a));
  return a;
}

/* The text of a value in a mode. */
std::string text(const ty_inet_t &a, int mode) {
  char buf[TY_INET_TEXT_MAX];

  REQUIRE(0 == ty_inet_format(&a, mode, buf));
  return buf;
}

} // namespace

// Verify the layout PostgreSQL stores: family 2 or 3, the mask, the bytes.
// 1. The struct is 18 bytes.
// 2. 1.2.3.4/24 is family 2, bits 24, bytes 1 2 3 4, then zeros.
// 3. ::1 is family 3, bits 128, and 15 zero bytes then 1.
TEST_CASE("an inet value has the stored layout") {
  // 1. The struct is 18 bytes.
  CHECK(18 == sizeof(ty_inet_t));

  // 2. 1.2.3.4/24 is family 2, bits 24, bytes 1 2 3 4, then zeros.
  ty_inet_t a = inet("1.2.3.4/24");
  CHECK(2 == a.family);
  CHECK(24 == a.bits);
  CHECK(a.addr[0] == 1);
  CHECK(a.addr[3] == 4);
  CHECK(a.addr[4] == 0);

  // 3. ::1 is family 3, bits 128, and 15 zero bytes then 1.
  ty_inet_t b = inet("::1");
  CHECK(3 == b.family);
  CHECK(128 == b.bits);
  CHECK(b.addr[14] == 0);
  CHECK(b.addr[15] == 1);
}

// Verify the text modes of one value.
// 1. 192.168.1.5/24 as inet: with /24; host: no mask; text: with /24.
// 2. 1.2.3.4 as inet has no /32; as text() it has /32.
// 3. A cidr always shows /n.
TEST_CASE("the text modes of inet and cidr") {
  // 1. 192.168.1.5/24 as inet: with /24; host: no mask; text: with /24.
  ty_inet_t a = inet("192.168.1.5/24");
  CHECK(text(a, TY_INET_OUT) == "192.168.1.5/24");
  CHECK(text(a, TY_INET_HOST) == "192.168.1.5");
  CHECK(text(a, TY_INET_SHOW) == "192.168.1.5/24");

  // 2. 1.2.3.4 as inet has no /32; as text() it has /32.
  ty_inet_t b = inet("1.2.3.4");
  CHECK(text(b, TY_INET_OUT) == "1.2.3.4");
  CHECK(text(b, TY_INET_SHOW) == "1.2.3.4/32");

  // 3. A cidr always shows /n.
  ty_inet_t c = inet("10.1.0.0/16", 1);
  CHECK(text(c, TY_INET_CIDR) == "10.1.0.0/16");
  CHECK(text(c, TY_INET_CIDR_ABBREV) == "10.1/16");
  CHECK(text(inet("::1", 1), TY_INET_CIDR) == "::1/128");
}

// Verify the two kinds of text error.
// 1. A bad address is TY_ERR_INVALID.
// 2. A mask past the family is TY_ERR_INVALID.
// 3. A cidr with a bit right of the mask is TY_ERR_RANGE.
TEST_CASE("inet text errors") {
  ty_inet_t a;

  // 1. A bad address is TY_ERR_INVALID.
  CHECK(-1 == ty_inet_parse("1.2.3", 0, &a));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. A mask past the family is TY_ERR_INVALID.
  CHECK(-1 == ty_inet_parse("1.2.3.4/33", 0, &a));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 3. A cidr with a bit right of the mask is TY_ERR_RANGE.
  CHECK(-1 == ty_inet_parse("1.2.3.4/24", 1, &a));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify the order and the containment of a few values.
// 1. 10.1.1.1/8 is below 10.1.1.1/16: the shorter mask first.
// 2. IPv4 is below IPv6.
// 3. 192.168.1.0/24 contains 192.168.1.5/32, and the reverse is not so.
TEST_CASE("inet order and containment") {
  ty_inet_t a = inet("10.1.1.1/8"), b = inet("10.1.1.1/16");

  // 1. 10.1.1.1/8 is below 10.1.1.1/16: the shorter mask first.
  CHECK(ty_inet_cmp(&a, &b) < 0);

  // 2. IPv4 is below IPv6.
  CHECK(ty_inet_cmp(&a, &b) < 0);
  ty_inet_t v6 = inet("::1");
  CHECK(ty_inet_cmp(&a, &v6) < 0);

  // 3. 192.168.1.0/24 contains 192.168.1.5/32, and the reverse is not so.
  ty_inet_t net = inet("192.168.1.0/24"), host = inet("192.168.1.5");
  CHECK(ty_inet_sup(&net, &host));
  CHECK(ty_inet_sub(&host, &net));
  CHECK(!ty_inet_sub(&net, &host));
  CHECK(!ty_inet_overlap(&net, &v6));
}

// Verify the address arithmetic at the ends.
// 1. 255.255.255.255 + 1 is out of range.
// 2. 0.0.0.0 - 1 is out of range.
// 3. 1.2.3.255 + 1 carries to 1.2.4.0.
// 4. The difference of two IPv6 values that does not fit int64 is out of range.
TEST_CASE("inet arithmetic stops at the ends") {
  ty_inet_t top = inet("255.255.255.255"), zero = inet("0.0.0.0");
  ty_inet_t mid = inet("1.2.3.255"), v6a = inet("::"), v6b = inet("8000::");
  ty_inet_t r;
  int64_t d = 0;

  // 1. 255.255.255.255 + 1 is out of range.
  CHECK(-1 == ty_inet_add(&top, 1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 2. 0.0.0.0 - 1 is out of range.
  CHECK(-1 == ty_inet_add(&zero, -1, &r));
  CHECK(TY_ERR_RANGE == ty_errnum());

  // 3. 1.2.3.255 + 1 carries to 1.2.4.0.
  CHECK(0 == ty_inet_add(&mid, 1, &r));
  CHECK(text(r, TY_INET_HOST) == "1.2.4.0");

  // 4. The difference of two IPv6 values that does not fit int64 is out of
  //    range.
  CHECK(-1 == ty_inet_sub_inet(&v6a, &v6b, &d));
  CHECK(TY_ERR_RANGE == ty_errnum());
}

// Verify that equal values hash alike and a different mask hashes apart.
// 1. 1.2.3.4 and 1.2.3.4/32 hash the same.
// 2. 1.2.3.4/24 hashes differently.
// 3. The extended hash with seed 0 has the standard hash as its low 32 bits.
TEST_CASE("the inet hash") {
  ty_inet_t a = inet("1.2.3.4"), b = inet("1.2.3.4/32"), c = inet("1.2.3.4/24");

  // 1. 1.2.3.4 and 1.2.3.4/32 hash the same.
  CHECK(ty_inet_hash(&a) == ty_inet_hash(&b));

  // 2. 1.2.3.4/24 hashes differently.
  CHECK(ty_inet_hash(&a) != ty_inet_hash(&c));

  // 3. The extended hash with seed 0 has the standard hash as its low 32 bits.
  CHECK((uint32_t)ty_inet_hash_extended(&a, 0) == ty_inet_hash(&a));
}
