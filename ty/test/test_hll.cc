/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of HyperLogLog (ty_hll.h): its estimates against those PostgreSQL's
 * src/backend/lib/hyperloglog.c printed for the same hashes, bit for bit.
 */
#include <stdlib.h>

#include <string>

#include "doctest.h"
#include "ty_hll.h"

namespace {

/* murmur3's finalizer: the hash of i, as the reference hashed it. */
uint32_t mix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

/* An estimate PostgreSQL printed: of n hashes into 2^width registers. */
struct known_t {
  int width;
  long n;
  double estimate;
};

} // namespace

// Verify that every estimate is PostgreSQL's, bit for bit.
// 1. For each width and each number of values, add the hashes of 0 to n - 1.
// 2. Compare the estimate with PostgreSQL's, as a double, exactly.
TEST_CASE("hll estimates as PostgreSQL does") {
  static const known_t known[] = {
      {4, 0, 0x0p+0},
      {4, 1, 0x1.08598b59e3a06p+0},
      {4, 2, 0x1.1178e8227e47ap+1},
      {4, 10, 0x1.62e42fefa39efp+3},
      {4, 100, 0x1.cdd70fb1306efp+6},
      {4, 1000, 0x1.f533922b4bb57p+9},
      {4, 10000, 0x1.f8c957f583ee9p+13},
      {4, 100000, 0x1.bb45ea2bcc5cfp+16},
      {4, 1000000, 0x1.f5322604dc582p+19},
      {5, 0, 0x0p+0},
      {5, 1, 0x1.0415d89e7444p+0},
      {5, 2, 0x1.08598b59e3a06p+1},
      {5, 10, 0x1.7fafa3bd8151cp+3},
      {5, 100, 0x1.cc3cda12178f7p+6},
      {5, 1000, 0x1.2a8d8a011777dp+10},
      {5, 10000, 0x1.98127fcc07b66p+13},
      {5, 100000, 0x1.7f0c7dba7b3d5p+16},
      {5, 1000000, 0x1.e860148ab49f7p+19},
      {6, 0, 0x0p+0},
      {6, 1, 0x1.0205658935837p+0},
      {6, 2, 0x1.0415d89e7444p+1},
      {6, 10, 0x1.5bf406b543dbp+3},
      {6, 100, 0x1.85134cf3f367ep+6},
      {6, 1000, 0x1.ecabbfd7bc244p+9},
      {6, 10000, 0x1.79180117a04a7p+13},
      {6, 100000, 0x1.959629906af22p+16},
      {6, 1000000, 0x1.b4192e2b920aep+19},
      {10, 0, 0x0p+0},
      {10, 1, 0x1.0020055655885p+0},
      {10, 2, 0x1.0040155d5881ep+1},
      {10, 10, 0x1.41929f968330cp+3},
      {10, 100, 0x1.9335e5d594988p+6},
      {10, 1000, 0x1.fa3345d6acb32p+9},
      {10, 10000, 0x1.2cfe24290fde7p+13},
      {10, 100000, 0x1.8022b372a1e9cp+16},
      {10, 1000000, 0x1.efe4c4099c667p+19},
      {16, 0, 0x0p+0},
      {16, 1, 0x1.0000800055549p+0},
      {16, 2, 0x1.00010001554f5p+1},
      {16, 10, 0x1.40064029ab011p+3},
      {16, 100, 0x1.904e345e4cfcp+6},
      {16, 1000, 0x1.f550b84d4e7f8p+9},
      {16, 10000, 0x1.38695c866521ap+13},
      {16, 100000, 0x1.877ac0bc6035fp+16},
      {16, 1000000, 0x1.e9470b937cd01p+19},
  };

  for (const known_t &k : known) {
    INFO(k.width << " " << k.n);
    ty_hll_t h;

    // 1. For each width and each number of values, add the hashes of 0 to
    //    n - 1.
    REQUIRE(0 == ty_hll_init(&h, k.width));
    for (long i = 0; i < k.n; i++) {
      ty_hll_add(&h, mix((uint32_t)i));
    }

    // 2. Compare the estimate with PostgreSQL's, as a double, exactly.
    CHECK(k.estimate == ty_hll_estimate(&h));
    ty_hll_free(&h);
  }
}

// Verify the corrections at the ends, the widths by error, and the errors.
// 1. Registers of high rank take the correction for 32-bit collisions.
// 2. A width by error is the fewest registers below it, at most 2^16.
// 3. A width outside 4 to 16 is an error.
TEST_CASE("hll ends, widths and errors") {
  ty_hll_t h;

  // 1. Registers of high rank take the correction for 32-bit collisions.
  REQUIRE(0 == ty_hll_init(&h, 4));
  for (uint32_t i = 0; i < 16; i++) {
    ty_hll_add(&h, (i << 28) | (1u << (27 - 24 - i % 4)));
  }
  CHECK(0x1.95188e688c98ep+29 == ty_hll_estimate(&h));
  ty_hll_free(&h);

  // 2. A width by error is the fewest registers below it, at most 2^16.
  static const struct {
    double error;
    int width;
  } by_error[] = {
      {0.3, 4},   {0.26, 5},   {0.1, 7},    {0.05, 9},
      {0.01, 14}, {0.005, 16}, {0.001, 16},
  };

  for (const auto &e : by_error) {
    INFO(e.error);
    REQUIRE(0 == ty_hll_init_error(&h, e.error));
    CHECK(e.width == h.width);
    CHECK(((size_t)1 << e.width) == h.nreg);
    ty_hll_free(&h);
  }

  // 3. A width outside 4 to 16 is an error.
  CHECK(-1 == ty_hll_init(&h, 3));
  CHECK(TY_ERR_INVALID == ty_errnum());
  CHECK(std::string(ty_errmsg()) ==
        "bit width must be between 4 and 16 inclusive");
  CHECK(-1 == ty_hll_init(&h, 17));
}
