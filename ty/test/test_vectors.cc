/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of ty's decimal operations against test/vectors.txt: vectors of
 * a reference in plain Python integers (gen_vectors.py), which follows the
 * rules of DESIGN.md and not ty's code. Each line is "OP ARGS => RESULT";
 * RESULT is "V S", a number, or "E NAME" for an error.
 */
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "ty_decimal.h"

namespace {

/* A vector: the operation, its arguments and its result, as tokens. */
struct vec_t {
  std::string op;
  std::vector<std::string> args;
  std::vector<std::string> res;
  std::string line;
};

/* A 128-bit integer from its decimal text. */
ty_int128_t int128_of(const std::string &s) {
  size_t i = 0;
  bool neg = false;
  ty_int128_t v = 0;

  if (s[i] == '-') {
    neg = true;
    i++;
  }
  for (; i < s.size(); i++) {
    v = v * 10 + (s[i] - '0');
  }
  return neg ? -v : v;
}

/* The vectors of the file, a line each. */
std::vector<vec_t> read_vectors(const char *path) {
  std::vector<vec_t> out;
  std::ifstream in(path);
  std::string line;

  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::string tok;
    vec_t v;
    bool res = false;

    v.line = line;
    ss >> v.op;
    while (ss >> tok) {
      if (tok == "=>") {
        res = true;
      } else {
        (res ? v.res : v.args).push_back(tok);
      }
    }
    out.push_back(v);
  }
  return out;
}

/* The ty_errnum_t a name stands for. */
ty_errnum_t errnum_of(const std::string &name) {
  if (name == "RANGE") {
    return TY_ERR_RANGE;
  }
  if (name == "DIVZERO") {
    return TY_ERR_DIVZERO;
  }
  return TY_ERR_INVALID;
}

/*
 * Check a call's result against the vector's: rc is its return, and v and s
 * the number it set.
 */
void check_number(const vec_t &v, int rc, ty_int128_t value, int scale) {
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(int128_of(v.res[0]) == value);
    CHECK(std::stoi(v.res[1]) == scale);
  }
}

/* Check a call that gives an integer: a compare, or a bucket. */
void check_int(const vec_t &v, int rc, long long got) {
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(std::stoll(v.res[0]) == got);
  }
}

/* A decimal64 from two tokens. */
ty_decimal64_t d64(const vec_t &v, size_t i) {
  return {(int64_t)int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* A decimal128 from two tokens. */
ty_decimal128_t d128(const vec_t &v, size_t i) {
  return {int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* Run one decimal64 vector; false if its op is not one of them. */
bool run64(const vec_t &v) {
  ty_decimal64_t r = {0, 0};
  int rc;
  const std::string &op = v.op;

  if (op == "d64.add" || op == "d64.sub" || op == "d64.mul" ||
      op == "d64.mod" || op == "d64.gcd" || op == "d64.lcm") {
    ty_decimal64_t a = d64(v, 0), b = d64(v, 2);
    rc = op == "d64.add"   ? ty_decimal64_add(&a, &b, &r)
         : op == "d64.sub" ? ty_decimal64_sub(&a, &b, &r)
         : op == "d64.mul" ? ty_decimal64_mul(&a, &b, &r)
         : op == "d64.mod" ? ty_decimal64_mod(&a, &b, &r)
         : op == "d64.gcd" ? ty_decimal64_gcd(&a, &b, &r)
                           : ty_decimal64_lcm(&a, &b, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.cmp") {
    check_int(v, 0, ty_decimal64_cmp(d64(v, 0), d64(v, 2)));
  } else if (op == "d64.round" || op == "d64.trunc") {
    ty_decimal64_t a = d64(v, 0);
    int n = std::stoi(v.args[2]);
    rc = op == "d64.round" ? ty_decimal64_round(&a, n, &r)
                           : ty_decimal64_trunc(&a, n, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.ceil" || op == "d64.floor") {
    r = op == "d64.ceil" ? ty_decimal64_ceil(d64(v, 0))
                         : ty_decimal64_floor(d64(v, 0));
    check_number(v, 0, r.value, r.scale);
  } else if (op == "d64.div") {
    ty_decimal64_t a = d64(v, 0), b = d64(v, 2);
    rc = ty_decimal64_div(&a, &b, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.fit") {
    ty_decimal128_t a = d128(v, 0);
    rc = ty_decimal64_from_decimal128(&a, &r);
    check_number(v, rc, r.value, r.scale);
  } else {
    return false;
  }
  return true;
}

/* Run one decimal128 vector; false if its op is not one of them. */
bool run128(const vec_t &v) {
  ty_decimal128_t r = {0, 0};
  int rc;
  const std::string &op = v.op;

  if (op == "d128.add" || op == "d128.sub" || op == "d128.mul" ||
      op == "d128.mod" || op == "d128.gcd" || op == "d128.lcm") {
    ty_decimal128_t a = d128(v, 0), b = d128(v, 2);
    rc = op == "d128.add"   ? ty_decimal128_add(&a, &b, &r)
         : op == "d128.sub" ? ty_decimal128_sub(&a, &b, &r)
         : op == "d128.mul" ? ty_decimal128_mul(&a, &b, &r)
         : op == "d128.mod" ? ty_decimal128_mod(&a, &b, &r)
         : op == "d128.gcd" ? ty_decimal128_gcd(&a, &b, &r)
                            : ty_decimal128_lcm(&a, &b, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.cmp") {
    check_int(v, 0, ty_decimal128_cmp(d128(v, 0), d128(v, 2)));
  } else if (op == "d128.round" || op == "d128.trunc") {
    ty_decimal128_t a = d128(v, 0);
    int n = std::stoi(v.args[2]);
    rc = op == "d128.round" ? ty_decimal128_round(&a, n, &r)
                            : ty_decimal128_trunc(&a, n, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.ceil" || op == "d128.floor") {
    r = op == "d128.ceil" ? ty_decimal128_ceil(d128(v, 0))
                          : ty_decimal128_floor(d128(v, 0));
    check_number(v, 0, r.value, r.scale);
  } else if (op == "d128.div") {
    ty_decimal128_t a = d128(v, 0), b = d128(v, 2);
    rc = ty_decimal128_div(&a, &b, std::stoi(v.args[4]), v.args[5] == "1", &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.sqrt") {
    ty_decimal128_t a = d128(v, 0);
    rc = ty_decimal128_sqrt(&a, std::stoi(v.args[2]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.exp" || op == "d128.ln") {
    ty_decimal128_t a = d128(v, 0);
    int rscale = std::stoi(v.args[2]);
    rc = op == "d128.exp" ? ty_decimal128_exp(&a, rscale, &r)
                          : ty_decimal128_ln(&a, rscale, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.log") {
    ty_decimal128_t base = d128(v, 0), a = d128(v, 2);
    rc = ty_decimal128_log(&base, &a, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.powi") {
    ty_decimal128_t base = d128(v, 0);
    rc = ty_decimal128_pow_int(&base, std::stoll(v.args[2]),
                               std::stoi(v.args[3]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.powr") {
    ty_decimal128_t base = d128(v, 0), e = d128(v, 2);
    rc = ty_decimal128_pow_real(&base, &e, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.fact") {
    rc = ty_decimal128_factorial(std::stoll(v.args[0]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.wb") {
    ty_decimal128_t x = d128(v, 0), b1 = d128(v, 2), b2 = d128(v, 4);
    int64_t bucket = 0;
    rc =
        ty_decimal128_width_bucket(&x, &b1, &b2, std::stoi(v.args[6]), &bucket);
    check_int(v, rc, bucket);
  } else {
    return false;
  }
  return true;
}

} // namespace

// Verify every decimal operation of ty against a reference of plain integers.
// 1. Read the vectors of test/vectors.txt: a line each, with its result.
// 2. Run each through ty, by its operation.
// 3. Check the number and its scale, or the integer, or the error kind.
TEST_CASE("ty's decimal operations agree with the reference vectors") {
  const char *path = getenv("TY_VECTORS");
  std::vector<vec_t> vecs = read_vectors(path ? path : "test/vectors.txt");

  // 1. Read the vectors of test/vectors.txt: a line each, with its result.
  REQUIRE(vecs.size() > 1000);
  // 2. Run each through ty, by its operation.
  // 3. Check the number and its scale, or the integer, or the error kind.
  for (const vec_t &v : vecs) {
    bool known = run64(v) || run128(v);
    INFO(v.line);
    REQUIRE(known);
  }
}
