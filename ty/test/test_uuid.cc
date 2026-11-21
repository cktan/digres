/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of the uuid operations.
 */
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_uuid.h"

// Verify that the text of a uuid is read in every form and written in one.
// 1. Parse the canonical form, with braces, with no hyphens, in capitals.
// 2. All four give the same bytes; format writes lowercase, hyphenated.
// 3. A short text, a long one and a stray brace fail with TY_ERR_INVALID.
TEST_CASE("a uuid parses in every form and formats in one") {
  const char *forms[] = {"a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11",
                         "{a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11}",
                         "a0eebc999c0b4ef8bb6d6bb9bd380a11",
                         "A0EEBC99-9C0B-4EF8-BB6D-6BB9BD380A11"};
  ty_uuid_t first, u;
  char out[TY_UUID_TEXT_SIZE];

  // 1. Parse the canonical form, with braces, with no hyphens, in capitals.
  CHECK(0 == ty_uuid_parse(forms[0], strlen(forms[0]), &first));
  for (const char *f : forms) {
    CHECK(0 == ty_uuid_parse(f, strlen(f), &u));

    // 2. All four give the same bytes; format writes lowercase, hyphenated.
    CHECK(0 == memcmp(first.data, u.data, 16));
  }
  ty_uuid_format(&first, out);
  CHECK(std::string(out) == forms[0]);

  // 3. A short text, a long one and a stray brace fail with TY_ERR_INVALID.
  for (const char *f : {"a0eebc99", "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a110",
                        "{a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11"}) {
    CHECK(-1 == ty_uuid_parse(f, strlen(f), &u));
    CHECK(TY_ERR_INVALID == ty_errnum());
  }
}

// Verify the hash: equal uuids hash alike, different ones do not, a seed
// changes it.
// 1. Hash two copies of a uuid: one hash.
// 2. Change one bit: another hash.
// 3. Use seed 0 and seed 1: two hashes.
TEST_CASE("the uuid hash depends on every byte and on the seed") {
  ty_uuid_t a, b;

  // 1. Hash two copies of a uuid: one hash.
  memset(a.data, 0x5a, 16);
  b = a;
  CHECK(ty_uuid_hash(&a, 0) == ty_uuid_hash(&b, 0));

  // 2. Change one bit: another hash.
  for (int i = 0; i < 16; i++) {
    b = a;
    b.data[i] ^= 1;
    CHECK(ty_uuid_hash(&a, 0) != ty_uuid_hash(&b, 0));
  }

  // 3. Use seed 0 and seed 1: two hashes.
  CHECK(ty_uuid_hash(&a, 0) != ty_uuid_hash(&a, 1));
}

// Verify that the uuids ty makes carry their version and variant, and that
// version 7 holds its time.
// 1. Make a version 4: its version is 4, and two of them differ.
// 2. Make a version 7 at 1,700,000,000,123 ms: its version is 7 and its
//    time is that, in microseconds.
// 3. Of one millisecond, a later fraction orders after an earlier one.
// 4. A time past 48 bits, and a fraction past a millisecond, are errors.
TEST_CASE("version 4 and 7 uuids are made with their bits and their time") {
  ty_uuid_t a, b;
  int64_t us = 0;

  // 1. Make a version 4: its version is 4, and two of them differ.
  CHECK(0 == ty_uuid_v4(&a));
  CHECK(0 == ty_uuid_v4(&b));
  CHECK(4 == ty_uuid_version(&a));
  CHECK(0 != memcmp(a.data, b.data, 16));
  CHECK(1 == ty_uuid_unix_us(&a, &us));

  // 2. Make a version 7 at 1,700,000,000,123 ms: its version is 7 and its
  //    time is that, in microseconds.
  CHECK(0 == ty_uuid_v7(1700000000123ULL, 0, &a));
  CHECK(7 == ty_uuid_version(&a));
  CHECK(0 == ty_uuid_unix_us(&a, &us));
  CHECK(1700000000123000LL == us);

  // 3. Of one millisecond, a later fraction orders after an earlier one.
  CHECK(0 == ty_uuid_v7(1700000000123ULL, 900000, &b));
  CHECK(ty_uuid_cmp(&a, &b) < 0);

  // 4. A time past 48 bits, and a fraction past a millisecond, are errors.
  CHECK(-1 == ty_uuid_v7(1ULL << 48, 0, &a));
  CHECK(TY_ERR_RANGE == ty_errnum());
  CHECK(-1 == ty_uuid_v7(0, 1000000, &a));
}
