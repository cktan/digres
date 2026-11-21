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
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "ty_decimal.h"
#include "ty_int.h"

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
  if (name == "UNDERFLOW") {
    return TY_ERR_UNDERFLOW;
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
ty_dec64_t d64(const vec_t &v, size_t i) {
  return {(int64_t)int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* A decimal128 from two tokens. */
ty_dec128_t d128(const vec_t &v, size_t i) {
  return {int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* Run one decimal64 vector; false if its op is not one of them. */
bool run64(const vec_t &v) {
  ty_dec64_t r = {0, 0};
  int rc;
  const std::string &op = v.op;

  if (op == "d64.add" || op == "d64.sub" || op == "d64.mul" ||
      op == "d64.mod" || op == "d64.gcd" || op == "d64.lcm") {
    ty_dec64_t a = d64(v, 0), b = d64(v, 2);
    rc = op == "d64.add"   ? ty_dec64_add(&a, &b, &r)
         : op == "d64.sub" ? ty_dec64_sub(&a, &b, &r)
         : op == "d64.mul" ? ty_dec64_mul(&a, &b, &r)
         : op == "d64.mod" ? ty_dec64_mod(&a, &b, &r)
         : op == "d64.gcd" ? ty_dec64_gcd(&a, &b, &r)
                           : ty_dec64_lcm(&a, &b, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.cmp") {
    check_int(v, 0, ty_dec64_cmp(d64(v, 0), d64(v, 2)));
  } else if (op == "d64.round" || op == "d64.trunc") {
    ty_dec64_t a = d64(v, 0);
    int n = std::stoi(v.args[2]);
    rc = op == "d64.round" ? ty_dec64_round(&a, n, &r)
                           : ty_dec64_trunc(&a, n, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.ceil" || op == "d64.floor") {
    r = op == "d64.ceil" ? ty_dec64_ceil(d64(v, 0)) : ty_dec64_floor(d64(v, 0));
    check_number(v, 0, r.value, r.scale);
  } else if (op == "d64.div") {
    ty_dec64_t a = d64(v, 0), b = d64(v, 2);
    rc = ty_dec64_div(&a, &b, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d64.fit") {
    ty_dec128_t a = d128(v, 0);
    rc = ty_dec64_from_dec128(&a, &r);
    check_number(v, rc, r.value, r.scale);
  } else {
    return false;
  }
  return true;
}

/* Run one decimal128 vector; false if its op is not one of them. */
bool run128(const vec_t &v) {
  ty_dec128_t r = {0, 0};
  int rc;
  const std::string &op = v.op;

  if (op == "d128.add" || op == "d128.sub" || op == "d128.mul" ||
      op == "d128.mod" || op == "d128.gcd" || op == "d128.lcm") {
    ty_dec128_t a = d128(v, 0), b = d128(v, 2);
    rc = op == "d128.add"   ? ty_dec128_add(&a, &b, &r)
         : op == "d128.sub" ? ty_dec128_sub(&a, &b, &r)
         : op == "d128.mul" ? ty_dec128_mul(&a, &b, &r)
         : op == "d128.mod" ? ty_dec128_mod(&a, &b, &r)
         : op == "d128.gcd" ? ty_dec128_gcd(&a, &b, &r)
                            : ty_dec128_lcm(&a, &b, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.cmp") {
    check_int(v, 0, ty_dec128_cmp(d128(v, 0), d128(v, 2)));
  } else if (op == "d128.round" || op == "d128.trunc") {
    ty_dec128_t a = d128(v, 0);
    int n = std::stoi(v.args[2]);
    rc = op == "d128.round" ? ty_dec128_round(&a, n, &r)
                            : ty_dec128_trunc(&a, n, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.ceil" || op == "d128.floor") {
    r = op == "d128.ceil" ? ty_dec128_ceil(d128(v, 0))
                          : ty_dec128_floor(d128(v, 0));
    check_number(v, 0, r.value, r.scale);
  } else if (op == "d128.div") {
    ty_dec128_t a = d128(v, 0), b = d128(v, 2);
    rc = ty_dec128_div(&a, &b, std::stoi(v.args[4]), v.args[5] == "1", &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.sqrt") {
    ty_dec128_t a = d128(v, 0);
    rc = ty_dec128_sqrt(&a, std::stoi(v.args[2]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.exp" || op == "d128.ln") {
    ty_dec128_t a = d128(v, 0);
    int rscale = std::stoi(v.args[2]);
    rc = op == "d128.exp" ? ty_dec128_exp(&a, rscale, &r)
                          : ty_dec128_ln(&a, rscale, &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.log") {
    ty_dec128_t base = d128(v, 0), a = d128(v, 2);
    rc = ty_dec128_log(&base, &a, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.powi") {
    ty_dec128_t base = d128(v, 0);
    rc = ty_dec128_pow_int(&base, std::stoll(v.args[2]), std::stoi(v.args[3]),
                           &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.powr") {
    ty_dec128_t base = d128(v, 0), e = d128(v, 2);
    rc = ty_dec128_pow_real(&base, &e, std::stoi(v.args[4]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.fact") {
    rc = ty_dec128_factorial(std::stoll(v.args[0]), &r);
    check_number(v, rc, r.value, r.scale);
  } else if (op == "d128.wb") {
    ty_dec128_t x = d128(v, 0), b1 = d128(v, 2), b2 = d128(v, 4);
    int64_t bucket = 0;
    rc = ty_dec128_width_bucket(&x, &b1, &b2, std::stoi(v.args[6]), &bucket);
    check_int(v, rc, bucket);
  } else {
    return false;
  }
  return true;
}

/*
 * Run one vector of the integer width N, of type T, named "iN.op"; false
 * if it is not of that width. A value is a long long in the vector, and
 * a result is checked as a long long.
 */
#define RUN_INT(N, T)                                                         \
  bool run_i##N(const vec_t &v) {                                             \
    const std::string p = "i" #N ".";                                         \
    if (v.op.compare(0, p.size(), p) != 0) {                                  \
      return false;                                                           \
    }                                                                         \
    const std::string name = v.op.substr(p.size());                           \
    T a = (T)std::stoll(v.args[0]);                                           \
    T b = v.args.size() > 1 ? (T)std::stoll(v.args[1]) : 0;                   \
    int n = v.args.size() > 1 ? (int)std::stoll(v.args[1]) : 0;               \
    T r = 0;                                                                  \
    if (name == "add") {                                                      \
      check_int(v, ty_int##N##_add(a, b, &r), r);                             \
    } else if (name == "sub") {                                               \
      check_int(v, ty_int##N##_sub(a, b, &r), r);                             \
    } else if (name == "mul") {                                               \
      check_int(v, ty_int##N##_mul(a, b, &r), r);                             \
    } else if (name == "div") {                                               \
      check_int(v, ty_int##N##_div(a, b, &r), r);                             \
    } else if (name == "mod") {                                               \
      check_int(v, ty_int##N##_mod(a, b, &r), r);                             \
    } else if (name == "gcd") {                                               \
      check_int(v, ty_int##N##_gcd(a, b, &r), r);                             \
    } else if (name == "lcm") {                                               \
      check_int(v, ty_int##N##_lcm(a, b, &r), r);                             \
    } else if (name == "neg") {                                               \
      check_int(v, ty_int##N##_neg(a, &r), r);                                \
    } else if (name == "abs") {                                               \
      check_int(v, ty_int##N##_abs(a, &r), r);                                \
    } else if (name == "inc") {                                               \
      check_int(v, ty_int##N##_inc(a, &r), r);                                \
    } else if (name == "dec") {                                               \
      check_int(v, ty_int##N##_dec(a, &r), r);                                \
    } else if (name == "shl") {                                               \
      check_int(v, ty_int##N##_shl(a, n, &r), r);                             \
    } else if (name == "shr") {                                               \
      check_int(v, ty_int##N##_shr(a, n, &r), r);                             \
    } else if (name == "larger") {                                            \
      check_int(v, 0, ty_int##N##_larger(a, b));                              \
    } else if (name == "smaller") {                                           \
      check_int(v, 0, ty_int##N##_smaller(a, b));                             \
    } else if (name == "and") {                                               \
      check_int(v, 0, ty_int##N##_and(a, b));                                 \
    } else if (name == "or") {                                                \
      check_int(v, 0, ty_int##N##_or(a, b));                                  \
    } else if (name == "xor") {                                               \
      check_int(v, 0, ty_int##N##_xor(a, b));                                 \
    } else if (name == "not") {                                               \
      check_int(v, 0, ty_int##N##_not(a));                                    \
    } else {                                                                  \
      return false;                                                           \
    }                                                                         \
    return true;                                                              \
  }

RUN_INT(16, int16_t)
RUN_INT(32, int32_t)
RUN_INT(64, int64_t)
#undef RUN_INT

/* Run a narrowing and a double vector of the integers; false if neither. */
bool run_int_convert(const vec_t &v) {
  const std::string &op = v.op;
  int rc;

  if (op == "i32.to16") {
    int16_t r = 0;
    rc = ty_int32_to_int16((int32_t)std::stoll(v.args[0]), &r);
    check_int(v, rc, r);
  } else if (op == "i64.to32") {
    int32_t r = 0;
    rc = ty_int64_to_int32(std::stoll(v.args[0]), &r);
    check_int(v, rc, r);
  } else if (op == "i64.to16") {
    int16_t r = 0;
    rc = ty_int64_to_int16(std::stoll(v.args[0]), &r);
    check_int(v, rc, r);
  } else if (op == "i64.todouble") {
    INFO(v.line);
    CHECK(std::strtod(v.res[0].c_str(), nullptr) ==
          ty_int64_to_double(std::stoll(v.args[0])));
  } else if (op == "i64.fromdouble") {
    int64_t r = 0;
    rc = ty_int64_from_double(std::strtod(v.args[0].c_str(), nullptr), &r);
    check_int(v, rc, r);
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
TEST_CASE(
    "ty's decimal and integer operations agree with the reference vectors") {
  const char *path = getenv("TY_VECTORS");
  std::vector<vec_t> vecs = read_vectors(path ? path : "test/vectors.txt");

  // 1. Read the vectors of test/vectors.txt: a line each, with its result.
  REQUIRE(vecs.size() > 1000);
  // 2. Run each through ty, by its operation.
  // 3. Check the number and its scale, or the integer, or the error kind.
  for (const vec_t &v : vecs) {
    bool known = run_int_convert(v) || run64(v) || run128(v) || run_i16(v) ||
                 run_i32(v) || run_i64(v);
    INFO(v.line);
    REQUIRE(known);
  }
}
