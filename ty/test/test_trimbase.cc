/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of to_bin, to_oct and to_hex (ty_uint64_to_base, ty_int.h) and of
 * bytea's btrim, ltrim and rtrim (ty_bytea_trim_range, ty_bytea.h), by hand and
 * by the vectors of test/gen_trimbase.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_bytea.h"
#include "ty_int.h"
#include "vl.h"

namespace {

/* v in base, as a string. */
std::string base(uint64_t v, int b) {
  char out[64];

  return std::string(out, ty_uint64_to_base(v, b, out));
}

/* The bytes that the hex digits h stand for; "-" is empty. */
std::string unhex(const std::string &h) {
  std::string s;

  if ("-" == h) {
    return s;
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    s += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  }
  return s;
}

/* The bytes s as hex digits; "-" if empty. */
std::string hex(const std::string &s) {
  static const char d[] = "0123456789abcdef";
  std::string h;

  for (unsigned char c : s) {
    h += d[c >> 4];
    h += d[c & 15];
  }
  return h.empty() ? "-" : h;
}

/* The trim of a by set, from the left, the right or both. */
std::string trim(const std::string &a, const std::string &set, int left,
                 int right) {
  vl_t x(a), y(set);
  size_t off, len;

  ty_bytea_trim_range(x, y, left, right, &off, &len);
  return a.substr(off, len);
}

} // namespace

// Verify to_bin, to_oct and to_hex, and bytea's trims, by hand.
// 1. Zero has one digit; the largest value has every digit; a negative
//    value is its unsigned bits, as the host passes it.
// 2. Each trim takes the bytes of set from its own ends only, and an empty
//    set or string trims nothing.
TEST_CASE("to_bin, to_oct, to_hex and bytea's trims") {
  // 1. Zero, the largest value, and a negative one's bits.
  CHECK(base(0, 2) == "0");
  CHECK(base(0, 16) == "0");
  CHECK(base(255, 2) == "11111111");
  CHECK(base(255, 8) == "377");
  CHECK(base(255, 16) == "ff");
  CHECK(base(UINT64_MAX, 2) == std::string(64, '1'));
  CHECK(base(UINT64_MAX, 8) == "1777777777777777777777");
  CHECK(base((uint32_t)-1, 16) == "ffffffff");

  // 2. Each trim takes the bytes of set from its own ends only.
  std::string a("\x00\x01\x02\x00", 4), z("\x00", 1);
  CHECK(trim(a, z, 1, 1) == std::string("\x01\x02", 2));
  CHECK(trim(a, z, 1, 0) == std::string("\x01\x02\x00", 3));
  CHECK(trim(a, z, 0, 1) == std::string("\x00\x01\x02", 3));
  CHECK(trim(a, "", 1, 1) == a);
  CHECK(trim("", z, 1, 1) == "");
  CHECK(trim(std::string(3, '\0'), z, 1, 1) == "");
}

// Verify to_bin, to_oct, to_hex and the trims are digres's, by what a
// digres built before they moved gave.
// 1. Read test/trimbase_known.txt.
// 2. base: an int4 is passed as its 32 unsigned bits, an int8 as its 64.
// 3. trim: btrim, ltrim and rtrim.
TEST_CASE("to_bin, to_oct, to_hex and the trims are digres's") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  FILE *fp = fopen(
      (dir.substr(0, dir.rfind('/') + 1) + "trimbase_known.txt").c_str(), "r");
  char line[4096];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/trimbase_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char a[1024], b[1024], c[1024], d[1024], e[1024];
    int w;
    std::string got, want;

    if (5 == sscanf(line, "base %d %1023s => %1023s %1023s %1023s", &w, a, b, c,
                    d)) {
      // 2. base: an int4's 32 unsigned bits, an int8's 64.
      long long v = strtoll(a, nullptr, 10);
      uint64_t u = 4 == w ? (uint64_t)(uint32_t)(int32_t)v : (uint64_t)v;

      want = std::string(b) + " " + c + " " + d;
      got = base(u, 2) + " " + base(u, 8) + " " + base(u, 16);
    } else if (5 == sscanf(line, "trim %1023s %1023s => %1023s %1023s %1023s",
                           a, b, c, d, e)) {
      // 3. trim: btrim, ltrim and rtrim.
      std::string x = unhex(a), s = unhex(b);

      want = std::string(c) + " " + d + " " + e;
      got = hex(trim(x, s, 1, 1)) + " " + hex(trim(x, s, 1, 0)) + " " +
            hex(trim(x, s, 0, 1));
    } else {
      continue;
    }
    n++;
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 250);
}
