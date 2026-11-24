/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the abbreviated keys of a sort (ty_abbrev.h): the keys order as
 * their bytes do, and the rules decide as PostgreSQL's, with the numbers
 * PostgreSQL logged (trace_sort) for the same values.
 */
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "doctest.h"
#include "ty_abbrev.h"

namespace {

/* A number as PostgreSQL's log prints it: %f. */
std::string f6(double v) {
  char buf[64];

  snprintf(buf, sizeof buf, "%f", v);
  return buf;
}

/* -1, 0 or 1 as a compare of two keys. */
int cmp_keys(uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }

/* -1, 0 or 1 as memcmp of the first 8 bytes, 0 after a shorter value. */
int cmp_prefix(const std::string &a, const std::string &b) {
  unsigned char x[8] = {0}, y[8] = {0};

  memcpy(x, a.data(), a.size() < 8 ? a.size() : 8);
  memcpy(y, b.data(), b.size() < 8 ? b.size() : 8);
  int c = memcmp(x, y, 8);

  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

} // namespace

// Verify that a key compares as the first 8 bytes of its value do.
// 1. Keys of bytes, short, long, with bytes of 0 and of 0xff, pair by pair.
// 2. A uuid's key is its first 8 bytes, most significant first.
TEST_CASE("abbrev keys order as their bytes") {
  ty_abbrev_bytes_t st;

  REQUIRE(0 == ty_abbrev_bytes_init(&st));

  // 1. Keys of bytes, short, long, with bytes of 0 and of 0xff, pair by pair.
  const std::vector<std::string> vals = {
      "",          std::string("\0", 1),
      "a",         std::string("a\0", 2),
      "ab",        "abcdefgh",
      "abcdefgh~", "abcdefgi",
      "\xff",      "\xff\xff\xff\xff\xff\xff\xff\xff",
      "b",         "zzzzzzzzzzzz"};

  for (const std::string &a : vals) {
    for (const std::string &b : vals) {
      INFO(a << " | " << b);
      CHECK(cmp_prefix(a, b) ==
            cmp_keys(ty_abbrev_bytes(&st, a.data(), a.size()),
                     ty_abbrev_bytes(&st, b.data(), b.size())));
    }
  }
  CHECK(0 == ty_abbrev_bytes(&st, "", 0));
  ty_hll_free(&st.abbr);
  ty_hll_free(&st.full);

  // 2. A uuid's key is its first 8 bytes, most significant first.
  ty_abbrev_uuid_t us;
  const uint8_t u[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  REQUIRE(0 == ty_abbrev_uuid_init(&us));
  CHECK(0x0123456789abcdefULL == ty_abbrev_uuid(&us, u));
  CHECK(1 == us.input_count);
  ty_hll_free(&us.card);
}

// Verify the rule of the keys of bytes, with PostgreSQL's numbers.
// 1. Under 100 rows the rule waits.
// 2. 160 values with one first 8 bytes abort, with the estimates PostgreSQL
//    logged for them: 'same prefix 1' to 'same prefix 160'.
// 3. Distinct keys go on; past 10,000 rows the share falls to 0.65 of
//    itself, and the rule gives the share it judged by.
TEST_CASE("abbrev rule of bytes") {
  ty_abbrev_bytes_t st;
  double a, k, prop;

  REQUIRE(0 == ty_abbrev_bytes_init(&st));

  // 1. Under 100 rows the rule waits.
  CHECK(TY_ABBREV_WAIT == ty_abbrev_bytes_abort(&st, 99, &a, &k, &prop));

  // 2. 160 values with one first 8 bytes abort, with the estimates
  //    PostgreSQL logged for them: 'same prefix 1' to 'same prefix 160'.
  for (int i = 1; i <= 160; i++) {
    std::string v = "same prefix " + std::to_string(i);

    ty_abbrev_bytes(&st, v.data(), v.size());
  }
  CHECK(TY_ABBREV_ABORT == ty_abbrev_bytes_abort(&st, 160, &a, &k, &prop));
  CHECK(f6(a) == "1.000489");
  CHECK(f6(k) == "155.187096");
  CHECK(f6(prop) == "0.200000");
  ty_hll_free(&st.abbr);
  ty_hll_free(&st.full);

  // 3. Distinct keys go on; past 10,000 rows the share falls to 0.65 of
  //    itself, and the rule gives the share it judged by.
  REQUIRE(0 == ty_abbrev_bytes_init(&st));
  for (int i = 0; i < 20000; i++) {
    char v[8];
    uint32_t hi = (uint32_t)i * 2654435761u;

    // the halves differ: the hash of a key joins them by xor
    memcpy(v, &i, 4);
    memcpy(v + 4, &hi, 4);
    ty_abbrev_bytes(&st, v, 8);
  }
  CHECK(TY_ABBREV_GO_ON == ty_abbrev_bytes_abort(&st, 10000, &a, &k, &prop));
  CHECK(0.20 == st.prop_card);
  CHECK(TY_ABBREV_GO_ON == ty_abbrev_bytes_abort(&st, 20000, &a, &k, &prop));
  CHECK(0.20 == prop);
  CHECK(0.20 * 0.65 == st.prop_card);
  ty_hll_free(&st.abbr);
  ty_hll_free(&st.full);
}

// Verify the rule of the uuid keys, with PostgreSQL's numbers.
// 1. Under 10,000 rows or values the rule waits.
// 2. 10,240 uuids of one first 8 bytes abort, with the estimate and the
//    threshold PostgreSQL logged.
// 3. Past 100,000 distinct keys counting ends, and the rule waits after.
TEST_CASE("abbrev rule of uuid") {
  ty_abbrev_uuid_t st;
  uint8_t u[16] = {0};
  double card;

  REQUIRE(0 == ty_abbrev_uuid_init(&st));

  // 1. Under 10,000 rows or values the rule waits.
  CHECK(TY_ABBREV_WAIT == ty_abbrev_uuid_abort(&st, 10240, &card));

  // 2. 10,240 uuids of one first 8 bytes abort, with the estimate and the
  //    threshold PostgreSQL logged.
  for (int i = 0; i < 10240; i++) {
    u[15] = (uint8_t)(i % 9);
    ty_abbrev_uuid(&st, u);
  }
  CHECK(TY_ABBREV_WAIT == ty_abbrev_uuid_abort(&st, 9999, &card));
  CHECK(TY_ABBREV_ABORT == ty_abbrev_uuid_abort(&st, 10240, &card));
  CHECK(f6(card) == "1.000489");
  CHECK(f6(st.input_count / 2000.0 + 0.5) == "5.620000");
  ty_hll_free(&st.card);

  // 3. Past 100,000 distinct keys counting ends, and the rule waits after.
  REQUIRE(0 == ty_abbrev_uuid_init(&st));
  for (uint32_t i = 0; i < 200000; i++) {
    memcpy(u, &i, 4);
    ty_abbrev_uuid(&st, u);
  }
  CHECK(TY_ABBREV_ENDS == ty_abbrev_uuid_abort(&st, 200000, &card));
  CHECK(card > 100000.0);
  CHECK(0 == st.estimating);
  CHECK(TY_ABBREV_WAIT == ty_abbrev_uuid_abort(&st, 400000, &card));
  ty_hll_free(&st.card);
}
