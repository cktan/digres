/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests that ty's hashes are PostgreSQL's, bit for bit.
 */
#include <stdint.h>

#include <initializer_list>
#include <string.h>

#include "doctest.h"
#include "ty_bool.h"
#include "ty_bytea.h"
#include "ty_hash.h"
#include "ty_int.h"
#include "ty_uuid.h"

namespace {

/* The 32-bit standard hash as the signed int4 PostgreSQL shows it as. */
int32_t shown(uint64_t h) { return (int32_t)(uint32_t)h; }

} // namespace

// Verify the hashes against values that PostgreSQL printed for them, from
// hashint2, hashint4, hashint8, hashbool and uuid_hash and their extended
// forms, in a digres built before the hashes moved.
// 1. hashint4 of 0, 1, -1 and the edges.
// 2. hashint8 of values past 32 bits, and one that hashes as the int4 of
//    the same value does: 4294967296 as 1.
// 3. hashint2, and hashbool as the int4 0 or 1.
// 4. The extended hashes by a seed.
// 5. uuid_hash of two uuids, and the extended form.
TEST_CASE("the integer, bool and uuid hashes are PostgreSQL's") {
  // 1. hashint4 of 0, 1, -1 and the edges.
  CHECK(shown(ty_int32_hash(0, 0)) == -272711505);
  CHECK(shown(ty_int32_hash(1, 0)) == -1905060026);
  CHECK(shown(ty_int32_hash(-1, 0)) == 385747274);
  CHECK(shown(ty_int32_hash(INT32_MAX, 0)) == -96758253);
  CHECK(shown(ty_int32_hash(INT32_MIN, 0)) == -1524351049);

  // 2. hashint8 of values past 32 bits, and one that hashes as the int4 of
  //    the same value does: 4294967296 as 1.
  CHECK(shown(ty_int64_hash(0, 0)) == -272711505);
  CHECK(shown(ty_int64_hash(1, 0)) == -1905060026);
  CHECK(shown(ty_int64_hash(-1, 0)) == 385747274);
  CHECK(shown(ty_int64_hash(4294967296LL, 0)) == -1905060026);
  CHECK(shown(ty_int64_hash(-5000000000LL, 0)) == 954517114);
  CHECK(shown(ty_int64_hash(INT64_MAX, 0)) == -1524351049);
  CHECK(shown(ty_int64_hash(INT64_MIN, 0)) == -96758253);

  // 3. hashint2, and hashbool as the int4 0 or 1.
  CHECK(shown(ty_int16_hash(1, 0)) == -1905060026);
  CHECK(shown(ty_int16_hash(-1, 0)) == 385747274);
  CHECK(shown(ty_int16_hash(INT16_MAX, 0)) == 1289782602);
  CHECK(shown(ty_bool_hash(true, 0)) == -1905060026);
  CHECK(shown(ty_bool_hash(false, 0)) == -272711505);

  // 4. The extended hashes by a seed.
  CHECK((int64_t)ty_int32_hash(1, 0) == -3670598878359251130LL);
  CHECK((int64_t)ty_int32_hash(1, 5) == -475770148949854016LL);
  CHECK((int64_t)ty_int64_hash(-5000000000LL, 7) == -4714785727553898199LL);
  CHECK((int64_t)ty_int16_hash(-3, 1234567890123ULL) == 5697785217216294583LL);
  CHECK((int64_t)ty_bool_hash(true, 9) == 5702160639948485477LL);

  // 5. uuid_hash of two uuids, and the extended form.
  const char *a = "a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11";
  const char *z = "00000000-0000-0000-0000-000000000000";
  const char *f = "ffffffff-ffff-ffff-ffff-ffffffffffff";
  ty_uuid_t ua, uz, uf;
  REQUIRE(0 == ty_uuid_parse(a, strlen(a), &ua));
  REQUIRE(0 == ty_uuid_parse(z, strlen(z), &uz));
  REQUIRE(0 == ty_uuid_parse(f, strlen(f), &uf));
  CHECK(shown(ty_uuid_hash(&ua, 0)) == 1154666245);
  CHECK(shown(ty_uuid_hash(&uz, 0)) == 1353656403);
  CHECK((int64_t)ty_uuid_hash(&ua, 3) == -6826625711743895634LL);
  CHECK((int64_t)ty_uuid_hash(&uf, 123456789012345ULL) ==
        -3046247504661820713LL);
}

// Verify the relations that PostgreSQL's hashes keep.
// 1. The standard hash is the low 32 bits of the extended one with seed 0,
//    for a value and for bytes of every length 0 to 40, aligned or not.
// 2. The hash of a 32-bit value is the hash of its 4 bytes.
// 3. An int16, an int32 and an int64 of one value hash alike, so a hash
//    join across the widths finds equal values.
// 4. A bytea of the 16 bytes of a uuid hashes as the uuid does.
TEST_CASE("the hashes keep PostgreSQL's relations") {
  uint8_t buf[64];
  for (size_t i = 0; i < sizeof buf; i++) {
    buf[i] = (uint8_t)(i * 37 + 11);
  }

  // 1. The standard hash is the low 32 bits of the extended one with seed 0,
  //    for a value and for bytes of every length 0 to 40, aligned or not.
  for (size_t off = 0; off < 4; off++) {
    for (size_t n = 0; n <= 40; n++) {
      CHECK(ty_hash_bytes_std(buf + off, n) ==
            (uint32_t)ty_hash_bytes(buf + off, n, 0));
    }
  }
  for (uint32_t k : {0u, 1u, 0xdeadbeefu, 0xffffffffu}) {
    CHECK(ty_hash_u32_std(k) == (uint32_t)ty_hash_u32(k, 0));

    // 2. The hash of a 32-bit value is the hash of its 4 bytes.
    CHECK(ty_hash_u32_std(k) == ty_hash_bytes_std((const uint8_t *)&k, 4));
    CHECK(ty_hash_u32(k, 77) == ty_hash_bytes((const uint8_t *)&k, 4, 77));
  }

  // 3. An int16, an int32 and an int64 of one value hash alike, so a hash
  //    join across the widths finds equal values.
  const int16_t edges[] = {0, 1, -1, 12345, INT16_MIN, INT16_MAX};
  for (int16_t v : edges) {
    CHECK(ty_int16_hash(v, 5) == ty_int32_hash(v, 5));
    CHECK(ty_int32_hash(v, 5) == ty_int64_hash(v, 5));
  }

  // 4. A bytea of the 16 bytes of a uuid hashes as the uuid does.
  ty_uuid_t u;
  memcpy(u.data, buf, 16);
  CHECK(ty_uuid_hash(&u, 9) == ty_bytea_hash(buf, 16, 9));
}
