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
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "ty_bits.h"
#include "ty_bool.h"
#include "ty_bytea.h"
#include "ty_decimal.h"
#include "ty_float.h"
#include "ty_hash.h"
#include "ty_datetime.h"
#include "ty_geo.h"
#include "ty_inet.h"
#include "ty_interval.h"
#include "ty_money.h"
#include "ty_small.h"
#include "ty_text.h"
#include "ty_int.h"
#include "ty_mac.h"
#include "ty_uuid.h"

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

/* A uuid from its 32 hexadecimal digits. */
ty_uuid_t uuid_of(const std::string &hex) {
  ty_uuid_t u;

  for (int i = 0; i < 16; i++) {
    u.data[i] = (uint8_t)std::stoi(hex.substr(2 * i, 2), nullptr, 16);
  }
  return u;
}

/* The 32 hexadecimal digits of a uuid. */
std::string hex_of(const ty_uuid_t &u) {
  static const char digits[] = "0123456789abcdef";
  std::string s;

  for (int i = 0; i < 16; i++) {
    s += digits[u.data[i] >> 4];
    s += digits[u.data[i] & 15];
  }
  return s;
}

/* A decimal64 from two tokens. */
ty_dec64_t d64(const vec_t &v, size_t i) {
  return {(int64_t)int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* A decimal128 from two tokens. */
ty_dec128_t d128(const vec_t &v, size_t i) {
  return {int128_of(v.args[i]), std::stoi(v.args[i + 1])};
}

/* The bytes of a token: its hexadecimal digits, or "-" for none. */
std::vector<uint8_t> bytes_of(const std::string &tok) {
  std::vector<uint8_t> b;

  if (tok != "-") {
    for (size_t i = 0; i + 1 < tok.size(); i += 2) {
      b.push_back((uint8_t)std::stoi(tok.substr(i, 2), nullptr, 16));
    }
  }
  return b;
}

/* The token of bytes: their hexadecimal digits, or "-" for none. */
std::string token_of(const uint8_t *p, size_t n) {
  static const char digits[] = "0123456789abcdef";
  std::string s;

  for (size_t i = 0; i < n; i++) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s.empty() ? "-" : s;
}

/* The token of a vector of bytes. */
std::string token_of(const std::vector<uint8_t> &b) {
  return token_of(b.data(), b.size());
}

/* A text token: "" stands for the empty text. */
std::string text_of(const std::string &tok) { return tok == "\"\"" ? "" : tok; }

/* Check a call that gives bytes against the vector: an error or a token. */
void check_bytes(const vec_t &v, int rc, const std::vector<uint8_t> &got) {
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(v.res[0] == token_of(got));
  }
}

/*
 * Run a hash vector: the op names the type, the last argument is the seed,
 * and the result is the 64-bit hash; with seed 0 its low 32 bits are the
 * standard hash too. False if the op is not a hash.
 */
bool run_hash(const vec_t &v) {
  const std::string &op = v.op;
  static const char *const hashes[] = {"hash.bytes", "bytea.hash", "hash.u32",
                                       "i16.hash",   "i32.hash",   "i64.hash",
                                       "bool.hash",  "uuid.hash",  "d128.hash"};
  bool is_hash = false;
  for (const char *h : hashes) {
    is_hash = is_hash || op == h;
  }
  if (!is_hash) {
    return false;
  }
  uint64_t seed = std::stoull(v.args.back());
  uint64_t got;

  if (op == "hash.bytes" || op == "bytea.hash") {
    std::vector<uint8_t> k = bytes_of(v.args[0]);
    got = op == "hash.bytes" ? ty_hash_bytes(k.data(), k.size(), seed)
                             : ty_bytea_hash(k.data(), k.size(), seed);
    if (seed == 0) {
      INFO(v.line);
      CHECK((uint32_t)got == ty_hash_bytes_std(k.data(), k.size()));
    }
  } else if (op == "hash.u32") {
    got = ty_hash_u32((uint32_t)std::stoull(v.args[0]), seed);
    if (seed == 0) {
      INFO(v.line);
      CHECK((uint32_t)got == ty_hash_u32_std((uint32_t)std::stoull(v.args[0])));
    }
  } else if (op == "i16.hash") {
    got = ty_int16_hash((int16_t)std::stoll(v.args[0]), seed);
  } else if (op == "i32.hash") {
    got = ty_int32_hash((int32_t)std::stoll(v.args[0]), seed);
  } else if (op == "i64.hash") {
    got = ty_int64_hash(std::stoll(v.args[0]), seed);
  } else if (op == "bool.hash") {
    got = ty_bool_hash(v.args[0] == "1", seed);
  } else if (op == "uuid.hash") {
    ty_uuid_t u = uuid_of(v.args[0]);
    got = ty_uuid_hash(&u, seed);
  } else if (op == "d128.hash") {
    ty_dec128_t d = d128(v, 0);
    got = ty_dec128_hash(&d, seed);
    ty_dec64_t d64v = {(int64_t)(d.value % 1000), d.scale > 17 ? 17 : d.scale};
    ty_dec128_t wide = {d64v.value, d64v.scale};
    INFO(v.line);
    CHECK(ty_dec64_hash(&d64v, seed) == ty_dec128_hash(&wide, seed));
  } else {
    return false;
  }
  INFO(v.line);
  CHECK(std::stoull(v.res[0]) == got);
  return true;
}

/* A float from a token: a number, inf, -inf or nan. */
double flt(const std::string &tok) { return std::strtod(tok.c_str(), nullptr); }

/* True if two floats are the same: the same bits, or both NaN. */
bool same_float(double want, double got) {
  if (want != want) {
    return got != got;
  }
  return memcmp(&want, &got, sizeof want) == 0;
}

/* Check a call that gives a float against the vector: a value or an error. */
void check_float(const vec_t &v, int rc, double got) {
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(same_float(flt(v.res[0]), got));
  }
}

/* The unary float64 functions of the trig family, by name. */
struct unary_t {
  const char *name;
  int (*fn)(double, double *);
};

const unary_t unary_fns[] = {
    {"acos", ty_float64_acos},   {"asin", ty_float64_asin},
    {"atan", ty_float64_atan},   {"cos", ty_float64_cos},
    {"sin", ty_float64_sin},     {"tan", ty_float64_tan},
    {"cot", ty_float64_cot},     {"acosd", ty_float64_acosd},
    {"asind", ty_float64_asind}, {"atand", ty_float64_atand},
    {"cosd", ty_float64_cosd},   {"sind", ty_float64_sind},
    {"tand", ty_float64_tand},   {"cotd", ty_float64_cotd},
    {"sinh", ty_float64_sinh},   {"cosh", ty_float64_cosh},
    {"tanh", ty_float64_tanh},   {"asinh", ty_float64_asinh},
    {"acosh", ty_float64_acosh}, {"atanh", ty_float64_atanh},
    {"erf", ty_float64_erf},     {"erfc", ty_float64_erfc},
    {"gamma", ty_float64_gamma}, {"lgamma", ty_float64_lgamma}};

/* True if name is one of them. */
bool is_unary(const std::string &name) {
  for (const unary_t &u : unary_fns) {
    if (name == u.name) {
      return true;
    }
  }
  return false;
}

/* Call the one that name says. */
int call_unary(const std::string &name, double a, double *r) {
  for (const unary_t &u : unary_fns) {
    if (name == u.name) {
      return u.fn(a, r);
    }
  }
  return -1;
}

/* Run one float vector; false if its op is not one of them. */
bool run_float(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 4, "f32.") != 0 && op.compare(0, 4, "f64.") != 0) {
    return false;
  }
  const bool is32 = op[1] == '3';
  const std::string name = op.substr(4);
  double r = 0;
  int rc = 0;

  /* the arguments of a float32 are exact floats: read them as floats */
  auto a32 = [&](size_t i) { return (float)flt(v.args[i]); };
  auto a64 = [&](size_t i) { return flt(v.args[i]); };

#define BIN(NAME, OP32, OP64)                                  \
  if (name == NAME) {                                          \
    if (is32) {                                                \
      float x = 0;                                             \
      rc = OP32(a32(0), a32(1), &x);                           \
      r = x;                                                   \
    } else {                                                   \
      rc = OP64(a64(0), a64(1), &r);                           \
    }                                                          \
    check_float(v, rc, r);                                     \
    return true;                                               \
  }
  BIN("add", ty_float32_add, ty_float64_add)
  BIN("sub", ty_float32_sub, ty_float64_sub)
  BIN("mul", ty_float32_mul, ty_float64_mul)
  BIN("div", ty_float32_div, ty_float64_div)
#undef BIN

#define PRED(NAME)                                                         \
  if (name == #NAME) {                                                     \
    int got = is32 ? ty_float32_##NAME(a32(0), a32(1))                     \
                   : ty_float64_##NAME(a64(0), a64(1));                    \
    INFO(v.line);                                                          \
    CHECK(std::stoi(v.res[0]) == got);                                     \
    return true;                                                           \
  }
  PRED(cmp)
  PRED(eq)
  PRED(ne)
  PRED(lt)
  PRED(le)
  PRED(gt)
  PRED(ge)
#undef PRED

  if (name == "larger" || name == "smaller") {
    r = is32 ? (name == "larger" ? ty_float32_larger(a32(0), a32(1))
                                 : ty_float32_smaller(a32(0), a32(1)))
             : (name == "larger" ? ty_float64_larger(a64(0), a64(1))
                                 : ty_float64_smaller(a64(0), a64(1)));
    check_float(v, 0, r);
  } else if (name == "neg") {
    check_float(v, 0, is32 ? ty_float32_neg(a32(0)) : ty_float64_neg(a64(0)));
  } else if (name == "abs") {
    check_float(v, 0, is32 ? ty_float32_abs(a32(0)) : ty_float64_abs(a64(0)));
  } else if (name == "hash") {
    uint64_t seed = std::stoull(v.args[1]);
    uint64_t got =
        is32 ? ty_float32_hash(a32(0), seed) : ty_float64_hash(a64(0), seed);
    INFO(v.line);
    CHECK(std::stoull(v.res[0]) == got);
  } else if (name == "toint16" || name == "toint32" || name == "toint64") {
    int16_t r16 = 0;
    int32_t r32 = 0;
    int64_t r64 = 0;
    rc = name == "toint16"   ? (is32 ? ty_float32_to_int16(a32(0), &r16)
                                     : ty_float64_to_int16(a64(0), &r16))
         : name == "toint32" ? (is32 ? ty_float32_to_int32(a32(0), &r32)
                                     : ty_float64_to_int32(a64(0), &r32))
                             : (is32 ? ty_float32_to_int64(a32(0), &r64)
                                     : ty_float64_to_int64(a64(0), &r64));
    check_int(v, rc, name == "toint16" ? r16 : name == "toint32" ? r32 : r64);
  } else if (name == "to32") {
    float x = 0;
    rc = ty_float64_to_float32(a64(0), &x);
    check_float(v, rc, x);
  } else if (name == "round") {
    check_float(v, 0, ty_float64_round(a64(0)));
  } else if (name == "ceil") {
    check_float(v, 0, ty_float64_ceil(a64(0)));
  } else if (name == "floor") {
    check_float(v, 0, ty_float64_floor(a64(0)));
  } else if (name == "trunc") {
    check_float(v, 0, ty_float64_trunc(a64(0)));
  } else if (name == "sign") {
    check_float(v, 0, ty_float64_sign(a64(0)));
  } else if (name == "sqrt" || name == "ln" || name == "log10" ||
             name == "exp") {
    rc = name == "sqrt"    ? ty_float64_sqrt(a64(0), &r)
         : name == "ln"    ? ty_float64_ln(a64(0), &r)
         : name == "log10" ? ty_float64_log10(a64(0), &r)
                           : ty_float64_exp(a64(0), &r);
    check_float(v, rc, r);
  } else if (name == "atan2" || name == "atan2d") {
    rc = name == "atan2" ? ty_float64_atan2(a64(0), a64(1), &r)
                         : ty_float64_atan2d(a64(0), a64(1), &r);
    check_float(v, rc, r);
  } else if (name == "degrees" || name == "radians") {
    rc = name == "degrees" ? ty_float64_degrees(a64(0), &r)
                           : ty_float64_radians(a64(0), &r);
    check_float(v, rc, r);
  } else if (is_unary(name)) {
    rc = call_unary(name, a64(0), &r);
    check_float(v, rc, r);
  } else if (name == "vaccum") {
    ty_float_var_t st = {0, 0, 0};
    rc = 0;
    if (v.args[0] != "-") {
      for (const std::string &x : v.args) {
        rc = ty_float_var_accum(&st, flt(x));
        if (rc != 0) {
          break;
        }
      }
    }
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(TY_ERR_RANGE == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(same_float(flt(v.res[0]), st.n));
      CHECK(same_float(flt(v.res[1]), st.sx));
      CHECK(same_float(flt(v.res[2]), st.sxx));
    }
  } else if (name == "vcombine") {
    ty_float_var_t a = {flt(v.args[0]), flt(v.args[1]), flt(v.args[2])};
    ty_float_var_t b = {flt(v.args[3]), flt(v.args[4]), flt(v.args[5])};
    rc = ty_float_var_combine(&a, &b);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(same_float(flt(v.res[0]), a.n));
      CHECK(same_float(flt(v.res[1]), a.sx));
      CHECK(same_float(flt(v.res[2]), a.sxx));
    }
  } else if (name == "vfinal") {
    ty_float_var_t st = {flt(v.args[1]), flt(v.args[2]), flt(v.args[3])};
    const std::string &f = v.args[0];
    rc = f == "avg"         ? ty_float_var_avg(&st, &r)
         : f == "pop"       ? ty_float_var_pop(&st, &r)
         : f == "samp"      ? ty_float_var_samp(&st, &r)
         : f == "stddevpop" ? ty_float_stddev_pop(&st, &r)
                            : ty_float_stddev_samp(&st, &r);
    INFO(v.line);
    if (v.res[0] == "NULL") {
      CHECK(1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(same_float(flt(v.res[0]), r));
    }
  } else if (name == "raccum") {
    ty_float_regr_t st = {0, 0, 0, 0, 0, 0};
    rc = 0;
    if (v.args[0] != "-") {
      for (size_t i = 0; i + 1 < v.args.size() && rc == 0; i += 2) {
        rc = ty_float_regr_accum(&st, flt(v.args[i]), flt(v.args[i + 1]));
      }
    }
    INFO(v.line);
    REQUIRE(0 == rc);
    CHECK(same_float(flt(v.res[0]), st.n));
    CHECK(same_float(flt(v.res[1]), st.sx));
    CHECK(same_float(flt(v.res[2]), st.sxx));
    CHECK(same_float(flt(v.res[3]), st.sy));
    CHECK(same_float(flt(v.res[4]), st.syy));
    CHECK(same_float(flt(v.res[5]), st.sxy));
  } else if (name == "rfinal") {
    ty_float_regr_t st = {flt(v.args[1]), flt(v.args[2]), flt(v.args[3]),
                          flt(v.args[4]), flt(v.args[5]), flt(v.args[6])};
    const std::string &f = v.args[0];
    rc = f == "sxx"         ? ty_float_regr_sxx(&st, &r)
         : f == "syy"       ? ty_float_regr_syy(&st, &r)
         : f == "sxy"       ? ty_float_regr_sxy(&st, &r)
         : f == "avgx"      ? ty_float_regr_avgx(&st, &r)
         : f == "avgy"      ? ty_float_regr_avgy(&st, &r)
         : f == "covarpop"  ? ty_float_covar_pop(&st, &r)
         : f == "covarsamp" ? ty_float_covar_samp(&st, &r)
         : f == "corr"      ? ty_float_corr(&st, &r)
         : f == "r2"        ? ty_float_regr_r2(&st, &r)
         : f == "slope"     ? ty_float_regr_slope(&st, &r)
                            : ty_float_regr_intercept(&st, &r);
    INFO(v.line);
    if (v.res[0] == "NULL") {
      CHECK(1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(same_float(flt(v.res[0]), r));
    }
  } else if (name == "pow") {
    rc = ty_float64_pow(a64(0), a64(1), &r);
    check_float(v, rc, r);
  } else if (name == "wb") {
    int32_t bucket = 0;
    rc = ty_float64_width_bucket(a64(0), a64(1), a64(2),
                                 (int32_t)std::stoll(v.args[3]), &bucket);
    check_int(v, rc, bucket);
  } else {
    return false;
  }
  return true;
}

/* The bytes of a bit string token (its 0s and 1s, or "-"), and its length. */
std::vector<uint8_t> bits_of(const std::string &tok, int32_t *nbits) {
  std::string t = tok == "-" ? "" : tok;
  std::vector<uint8_t> b(ty_bits_bytes((int32_t)t.size()) + 1, 0);

  for (size_t i = 0; i < t.size(); i++) {
    if (t[i] == '1') {
      b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
  }
  *nbits = (int32_t)t.size();
  return b;
}

/* The token of a bit string. */
std::string bits_token(const uint8_t *b, int32_t nbits) {
  std::string s(nbits + 1, '0');

  ty_bits_format(b, nbits, &s[0]);
  s.resize(nbits);
  return s.empty() ? "-" : s;
}

/* Run one bit string vector; false if its op is not one. */
bool run_bits(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 5, "bits.") != 0) {
    return false;
  }
  const std::string name = op.substr(5);
  int32_t na = 0, nb = 0, nout = 0;
  std::vector<uint8_t> a, b, out(4096, 0);
  int rc = 0;
  auto num = [&](size_t i) { return std::stoll(v.args[i]); };
  /* check a result that is a bit string, or an error */
  auto check = [&](int rcode) {
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rcode);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rcode);
      CHECK(v.res[0] == bits_token(out.data(), nout));
    }
  };

  if (name == "parse") {
    std::vector<uint8_t> t = bytes_of(v.args[0]);
    out.assign(ty_bits_parse_room(t.size()) + 8, 0);
    rc = ty_bits_parse(reinterpret_cast<const char *>(t.data()), t.size(),
                       out.data(), &nout);
    check(rc);
    return true;
  }
  if (name == "fromint32" || name == "fromint64") {
    if (name == "fromint32") {
      ty_bits_from_int32((int32_t)std::stoll(v.args[0]), (int32_t)num(1),
                         out.data(), &nout);
    } else {
      ty_bits_from_int64(std::stoll(v.args[0]), (int32_t)num(1), out.data(),
                         &nout);
    }
    check(0);
    return true;
  }
  a = bits_of(v.args[0], &na);
  if (name == "cmp" || name == "eq" || name == "cat" || name == "position" ||
      name == "and" || name == "or" || name == "xor" || name == "overlay") {
    b = bits_of(v.args[1], &nb);
  }
  if (name == "cmp") {
    int c = ty_bits_cmp(a.data(), na, b.data(), nb);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
  } else if (name == "eq") {
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == ty_bits_eq(a.data(), na, b.data(), nb));
  } else if (name == "cat") {
    rc = ty_bits_cat(a.data(), na, b.data(), nb, out.data(), &nout);
    check(rc);
  } else if (name == "position") {
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == ty_bits_position(a.data(), na, b.data(), nb));
  } else if (name == "and" || name == "or" || name == "xor") {
    rc = name == "and"  ? ty_bits_and(a.data(), na, b.data(), nb, out.data())
         : name == "or" ? ty_bits_or(a.data(), na, b.data(), nb, out.data())
                        : ty_bits_xor(a.data(), na, b.data(), nb, out.data());
    nout = na;
    check(rc);
  } else if (name == "overlay") {
    rc = ty_bits_overlay(a.data(), na, b.data(), nb, (int32_t)num(2),
                         (int32_t)num(3), out.data(), &nout);
    check(rc);
  } else if (name == "count") {
    INFO(v.line);
    CHECK(std::stoll(v.res[0]) == ty_bits_count(a.data(), na));
  } else if (name == "not") {
    ty_bits_not(a.data(), na, out.data());
    nout = na;
    check(0);
  } else if (name == "shl" || name == "shr") {
    if (name == "shl") {
      ty_bits_shl(a.data(), na, (int32_t)num(1), out.data());
    } else {
      ty_bits_shr(a.data(), na, (int32_t)num(1), out.data());
    }
    nout = na;
    check(0);
  } else if (name == "substr") {
    rc = ty_bits_substr(a.data(), na, (int32_t)num(1), (int32_t)num(2),
                        (int)num(3), out.data(), &nout);
    check(rc);
  } else if (name == "get") {
    int32_t r = 0;
    rc = ty_bits_get(a.data(), na, (int32_t)num(1), &r);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(std::stoi(v.res[0]) == r);
    }
  } else if (name == "set") {
    rc =
        ty_bits_set(a.data(), na, (int32_t)num(1), (int32_t)num(2), out.data());
    nout = na;
    check(rc);
  } else if (name == "resize") {
    ty_bits_resize(a.data(), na, (int32_t)num(1), out.data());
    nout = (int32_t)num(1);
    check(0);
  } else if (name == "toint32" || name == "toint64") {
    int32_t r32 = 0;
    int64_t r64 = 0;
    rc = name == "toint32" ? ty_bits_to_int32(a.data(), na, &r32)
                           : ty_bits_to_int64(a.data(), na, &r64);
    check_int(v, rc, name == "toint32" ? r32 : r64);
  } else {
    return false;
  }
  return true;
}

/*
 * An inet or cidr text (known.txt): the forms PostgreSQL printed, joined by
 * commas: for an inet, the output, host(), text() and abbrev(); for a cidr,
 * the output, text() and abbrev().
 */
bool run_inet_text(const vec_t &v) {
  const bool cidr = v.op == "cidr.parse";
  std::vector<uint8_t> t = bytes_of(v.args[0]);
  std::string z(reinterpret_cast<const char *>(t.data()), t.size());
  ty_inet_t a;
  char buf[TY_INET_TEXT_MAX];
  int rc = ty_inet_parse(z.c_str(), cidr, &a);
  std::string got;

  if (rc == 0) {
    int modes_inet[] = {TY_INET_OUT, TY_INET_HOST, TY_INET_SHOW,
                        TY_INET_ABBREV};
    int modes_cidr[] = {TY_INET_CIDR, TY_INET_SHOW, TY_INET_CIDR_ABBREV};

    for (int i = 0; i < (cidr ? 3 : 4); i++) {
      REQUIRE(0 ==
              ty_inet_format(&a, cidr ? modes_cidr[i] : modes_inet[i], buf));
      got += (i ? "," : "") + std::string(buf);
    }
  }
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(v.res[0] == got);
  }
  return true;
}

/*
 * A tid, pg_lsn or "char" text (known.txt): the canonical text PostgreSQL
 * printed; the text of "char" may be empty, which the file writes as -.
 */
bool run_small_text(const vec_t &v) {
  std::vector<uint8_t> t = bytes_of(v.args[0]);
  std::string z(reinterpret_cast<const char *>(t.data()), t.size());
  char buf[32];
  int rc = 0;
  std::string got;

  if (v.op == "tid.parse") {
    ty_tid_t a;

    rc = ty_tid_parse(z.c_str(), &a);
    if (rc == 0) {
      ty_tid_format(&a, buf);
      got = buf;
    }
  } else if (v.op == "lsn.parse") {
    uint64_t a;

    rc = ty_lsn_parse(z.c_str(), &a);
    if (rc == 0) {
      ty_lsn_format(a, buf);
      got = buf;
    }
  } else {
    /* "char" never fails: its text is cut to a byte */
    ty_char_format(ty_char_parse(z.data(), z.size()), buf);
    got = buf[0] ? buf : "-";
  }
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(v.res[0] == got);
  }
  return true;
}

/* Run one character(n) or name vector. */
bool run_bpchar(const vec_t &v) {
  const std::string &op = v.op;

  if (op != "bpchar.truelen" && op != "bpchar.cmp" && op != "bpchar.fit" &&
      op != "name.parse" && op != "text.cliplen" && op != "text.clipbytes") {
    return false;
  }
  std::vector<uint8_t> ab = bytes_of(v.args[0]);
  std::string a(reinterpret_cast<const char *>(ab.data()), ab.size());
  INFO(v.line);
  if (op == "bpchar.truelen") {
    CHECK(std::stoll(v.res[0]) ==
          (int64_t)ty_bpchar_truelen(a.data(), a.size()));
  } else if (op == "bpchar.cmp") {
    std::vector<uint8_t> bb = bytes_of(v.args[1]);
    std::string b(reinterpret_cast<const char *>(bb.data()), bb.size());
    int c = ty_bpchar_cmp(a.data(), a.size(), b.data(), b.size());

    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
  } else if (op == "text.cliplen") {
    CHECK(std::stoll(v.res[0]) ==
          (int64_t)ty_text_cliplen(a.data(), a.size(), std::stoull(v.args[1])));
  } else if (op == "text.clipbytes") {
    CHECK(
        std::stoll(v.res[0]) ==
        (int64_t)ty_text_clipbytes(a.data(), a.size(), std::stoull(v.args[1])));
  } else if (op == "bpchar.fit") {
    size_t keep = 0, total = 0;
    int rc = ty_bpchar_fit(a.data(), a.size(), std::stoull(v.args[1]),
                           std::stoi(v.args[2]), std::stoi(v.args[3]), &keep,
                           &total);

    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(std::stoll(v.res[0]) == (int64_t)keep);
      CHECK(std::stoll(v.res[1]) == (int64_t)total);
    }
  } else {
    char out[TY_NAMEDATALEN];

    ty_name_parse(a.data(), a.size(), out);
    CHECK(v.res[0] ==
          token_of(reinterpret_cast<const uint8_t *>(out), strlen(out)));
  }
  return true;
}

/* The made-up locales of the money vectors (see gen_vectors.py). */
ty_money_fmt_t money_fmt(int i) {
  static const ty_money_fmt_t fmts[] = {
      {2, ".", ",", "$", "", "-", 3, 1, 1, 0, 0, 1, 1},
      {0, "", ".", "€", "+", "-", 3, 0, 0, 1, 1, 2, 2},
      {3, ",", " ", "kr", "", "~", 2, 0, 1, 2, 2, 3, 4},
      {2, ".", ",", "USD", "", "", 3, 1, 1, 1, 2, 0, 0},
      {127, "", "", "", "", "", 0, 127, 127, 127, 127, 127, 127},
      {4, "..", "'", "CHF", "", "-", 4, 1, 0, 1, 0, 4, 3},
  };

  return fmts[i];
}

/* Run one money vector. */
bool run_money(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 6, "money.") != 0) {
    return false;
  }
  const std::string name = op.substr(6);
  int rc = 0;
  int64_t r = 0;
  auto i64 = [&](size_t i) { return (int64_t)std::stoll(v.args[i]); };

  INFO(v.line);
  if (name == "words") {
    char out[256];
    size_t n = ty_money_words(i64(0), out);

    CHECK(v.res[0] == token_of(reinterpret_cast<const uint8_t *>(out), n));
    return true;
  }
  if (name == "format") {
    ty_money_fmt_t f = money_fmt(std::stoi(v.args[0]));
    char out[512];
    size_t n = ty_money_format(i64(1), &f, out, sizeof out);

    CHECK(v.res[0] == token_of(reinterpret_cast<const uint8_t *>(out), n));
    return true;
  }
  if (name == "add") {
    rc = ty_money_add(i64(0), i64(1), &r);
  } else if (name == "sub") {
    rc = ty_money_sub(i64(0), i64(1), &r);
  } else if (name == "mul_i64") {
    rc = ty_money_mul_int64(i64(0), i64(1), &r);
  } else if (name == "div_i64") {
    rc = ty_money_div_int64(i64(0), i64(1), &r);
  } else if (name == "mul_f64") {
    rc = ty_money_mul_float64(i64(0), std::stod(v.args[1]), &r);
  } else if (name == "div_f64") {
    rc = ty_money_div_float64(i64(0), std::stod(v.args[1]), &r);
  } else if (name == "div_money") {
    double d;

    rc = ty_money_div_money(i64(0), i64(1), &d);
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
    return true;
  } else {
    return false;
  }
  check_int(v, rc, r);
  return true;
}

/* An interval token "months:days:microseconds". */
ty_interval_t iv_of(const std::string &tok) {
  ty_interval_t a;
  long long m, d, t;

  REQUIRE(3 == sscanf(tok.c_str(), "%lld:%lld:%lld", &m, &d, &t));
  a.month = (int32_t)m;
  a.day = (int32_t)d;
  a.time = t;
  return a;
}

/* The token of an interval. */
std::string iv_token(const ty_interval_t &a) {
  char buf[96];

  snprintf(buf, sizeof buf, "%d:%d:%lld", a.month, a.day, (long long)a.time);
  return buf;
}

/* Run one interval vector. */
bool run_interval(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 3, "iv.") != 0) {
    return false;
  }
  const std::string name = op.substr(3);

  /* the text of an interval is in known.txt, run by run_known */
  if (name == "out" || name == "parse" || name == "trunc") {
    return false;
  }
  if (name == "make") {
    int y, mo, w, d, h, mi;
    char sec[64];
    ty_interval_t r2;

    REQUIRE(7 == sscanf(v.args[0].c_str(), "%d:%d:%d:%d:%d:%d:%63s", &y, &mo,
                        &w, &d, &h, &mi, sec));
    int rc2 = ty_interval_make(y, mo, w, d, h, mi, strtod(sec, nullptr), &r2);

    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc2);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc2);
      CHECK(v.res[0] == iv_token(r2));
    }
    return true;
  }
  ty_interval_t a = iv_of(v.args[0]), b, r;
  int rc = 0;

  memset(&r, 0, sizeof r);
  INFO(v.line);
  if (name == "sign") {
    CHECK(std::stoi(v.res[0]) == ty_interval_sign(&a));
    return true;
  }
  if (name == "cmp") {
    b = iv_of(v.args[1]);
    int c = ty_interval_cmp(&a, &b);
    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
    return true;
  }
  if (name == "add" || name == "sub") {
    b = iv_of(v.args[1]);
    rc = name == "add" ? ty_interval_add(&a, &b, &r)
                       : ty_interval_sub(&a, &b, &r);
  } else if (name == "mul" || name == "div") {
    double f = std::stod(v.args[1]);

    rc =
        name == "mul" ? ty_interval_mul(&a, f, &r) : ty_interval_div(&a, f, &r);
  } else if (name == "typmod") {
    r = a;
    rc = ty_interval_adjust_typmod(&r, (int32_t)std::stoll(v.args[1]));
  } else if (name == "neg") {
    rc = ty_interval_neg(&a, &r);
  } else if (name == "justify_hours") {
    rc = ty_interval_justify_hours(&a, &r);
  } else if (name == "justify_days") {
    rc = ty_interval_justify_days(&a, &r);
  } else if (name == "justify") {
    rc = ty_interval_justify(&a, &r);
  } else {
    return false;
  }
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(v.res[0] == iv_token(r));
  }
  return true;
}

/* Run one calendar, date, time or timestamp vector. */
/* A timetz token "time:zone". */
ty_timetz_t timetz_of(const std::string &t) {
  long long tm;
  int zn;

  REQUIRE(2 == sscanf(t.c_str(), "%lld:%d", &tm, &zn));
  return {tm, zn};
}

/* The order, the hash and the RANGE offsets of timetz, and of time. */
bool run_timetz(const vec_t &v) {
  const std::string &op = v.op;
  int r = 0;
  int rc = 0;

  if (op == "timetz.cmp") {
    ty_timetz_t a = timetz_of(v.args[0]), b = timetz_of(v.args[1]);

    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == ty_timetz_cmp(&a, &b));
    return true;
  }
  if (op == "timetz.hashx") {
    ty_timetz_t a = timetz_of(v.args[0]);
    uint64_t seed = std::stoull(v.args[1]);
    uint64_t got = ty_timetz_hash(&a, seed);

    INFO(v.line);
    CHECK(std::stoull(v.res[0]) == got);
    if (seed == 0) {
      CHECK((uint32_t)got == (ty_hash_u32_std((uint32_t)a.zone) ^
                              (uint32_t)ty_int64_hash(a.time, 0)));
    }
    return true;
  }
  if (op == "date.totimestamp" || op == "ts.todate" || op == "ts.totime" ||
      op == "dt.totimestamp" || op == "iv.totime") {
    int64_t out64 = 0;
    int32_t out32 = 0;
    int ovf = 0;

    if (op == "date.totimestamp") {
      rc = ty_date_to_timestamp((int32_t)std::stoll(v.args[0]), &out64,
                                v.args[1] == "1" ? &ovf : nullptr);
    } else if (op == "ts.todate") {
      rc = ty_timestamp_to_date(std::stoll(v.args[0]), &out32);
      out64 = out32;
    } else if (op == "ts.totime") {
      rc = ty_timestamp_to_time(std::stoll(v.args[0]), &out64);
    } else if (op == "dt.totimestamp") {
      rc = ty_datetime_to_timestamp((int32_t)std::stoll(v.args[0]),
                                    std::stoll(v.args[1]), &out64);
    } else {
      long long m, d, t;

      REQUIRE(3 == sscanf(v.args[0].c_str(), "%lld:%lld:%lld", &m, &d, &t));
      ty_interval_t in = {t, (int32_t)d, (int32_t)m};

      rc = ty_interval_to_time(&in, &out64);
    }
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      CHECK(0 == rc);
      CHECK(std::stoll(v.res[0]) == out64);
    }
    return true;
  }
  if (op == "overlaps.i64" || op == "overlaps.timetz") {
    int64_t iv[4];
    ty_timetz_t tz[4];
    const int64_t *ip[4];
    const ty_timetz_t *tp[4];

    for (int i = 0; i < 4; i++) {
      bool null = v.args[i] == "N";

      if (op == "overlaps.i64") {
        iv[i] = null ? 0 : std::stoll(v.args[i]);
        ip[i] = null ? nullptr : &iv[i];
      } else {
        tz[i] = null ? ty_timetz_t{0, 0} : timetz_of(v.args[i]);
        tp[i] = null ? nullptr : &tz[i];
      }
    }
    r = op == "overlaps.i64" ? ty_overlaps_i64(ip[0], ip[1], ip[2], ip[3])
                             : ty_overlaps_timetz(tp[0], tp[1], tp[2], tp[3]);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == r);
    return true;
  }
  if (op != "timetz.inrange" && op != "time.inrange") {
    return false;
  }
  long long m, d, t;

  REQUIRE(3 == sscanf(v.args[2].c_str(), "%lld:%lld:%lld", &m, &d, &t));
  ty_interval_t off = {t, (int32_t)d, (int32_t)m};
  int sub = std::stoi(v.args[3]), less = std::stoi(v.args[4]);

  if (op == "timetz.inrange") {
    ty_timetz_t a = timetz_of(v.args[0]), b = timetz_of(v.args[1]);

    rc = ty_timetz_in_range(&a, &b, &off, sub, less, &r);
  } else {
    rc = ty_time_in_range(std::stoll(v.args[0]), std::stoll(v.args[1]), &off,
                          sub, less, &r);
  }
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    CHECK(0 == rc);
    CHECK(std::stoi(v.res[0]) == r);
  }
  return true;
}

bool run_datetime(const vec_t &v) {
  const std::string &op = v.op;
  const bool cal = op.compare(0, 4, "cal.") == 0,
             date = op.compare(0, 5, "date.") == 0,
             tim = op.compare(0, 5, "time.") == 0,
             ts = op.compare(0, 3, "ts.") == 0;

  if (!cal && !date && !tim && !ts) {
    return false;
  }
  const std::string name = op.substr(op.find('.') + 1);

  /* the text of these types is in known.txt, run by run_known */
  if (name == "out" || name == "trunc" || name == "age" || name == "bin") {
    return false;
  }
  int rc = 0;
  auto i64 = [&](size_t i) { return (int64_t)std::stoll(v.args[i]); };
  /* a result that is a number, or an error */
  auto check_num = [&](int rcode, long long got) {
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rcode);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rcode);
      CHECK(std::stoll(v.res[0]) == got);
    }
  };
  auto tm_token = [](const ty_tm_t &t, int32_t f) {
    char buf[96];

    snprintf(buf, sizeof buf, "%d:%d:%d:%d:%d:%d:%d", t.year, t.mon, t.mday,
             t.hour, t.min, t.sec, f);
    return std::string(buf);
  };

  INFO(v.line);
  if (cal) {
    if (name == "date2j") {
      CHECK(std::stoll(v.res[0]) ==
            ty_date2j((int)i64(0), (int)i64(1), (int)i64(2)));
    } else if (name == "j2date") {
      int y, m, d;

      ty_j2date((int)i64(0), &y, &m, &d);
      char buf[64];

      snprintf(buf, sizeof buf, "%d:%d:%d", y, m, d);
      CHECK(v.res[0] == buf);
    } else {
      CHECK(std::stoll(v.res[0]) == ty_j2day((int)i64(0)));
    }
    return true;
  }
  if (date) {
    int32_t r = 0;

    if (name == "make") {
      rc = ty_date_make((int32_t)i64(0), (int32_t)i64(1), (int32_t)i64(2), &r);
    } else if (name == "add_days") {
      rc = ty_date_add_days((int32_t)i64(0), (int32_t)i64(1), &r);
    } else if (name == "sub_days") {
      rc = ty_date_sub_days((int32_t)i64(0), (int32_t)i64(1), &r);
    } else {
      rc = ty_date_diff((int32_t)i64(0), (int32_t)i64(1), &r);
    }
    check_num(rc, r);
    return true;
  }
  if (tim) {
    int64_t r = 0;
    ty_interval_t iv;

    if (name == "typmod") {
      r = i64(0);
      ty_time_adjust_typmod(&r, (int32_t)i64(1));
      CHECK(std::stoll(v.res[0]) == r);
      return true;
    }
    if (name == "make") {
      rc = ty_time_make((int32_t)i64(0), (int32_t)i64(1), std::stod(v.args[2]),
                        &r);
    } else if (name == "diff") {
      ty_time_diff(i64(0), i64(1), &iv);
      CHECK(v.res[0] == iv_token(iv));
      return true;
    } else {
      iv = iv_of(v.args[1]);
      rc = name == "add_interval" ? ty_time_add_interval(i64(0), &iv, &r)
                                  : ty_time_sub_interval(i64(0), &iv, &r);
    }
    check_num(rc, r);
    return true;
  }
  if (name == "typmod") {
    int64_t t = i64(0);

    rc = ty_timestamp_adjust_typmod(&t, (int32_t)i64(1));
    check_num(rc, t);
    return true;
  }
  if (name == "to_tm") {
    ty_tm_t t;
    int32_t f = 0;

    rc = ty_timestamp_to_tm(i64(0), &t, &f);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(v.res[0] == tm_token(t, f));
    }
  } else if (name == "from_tm") {
    ty_tm_t t;
    int32_t f;
    int64_t r = 0;

    REQUIRE(7 == sscanf(v.args[0].c_str(), "%d:%d:%d:%d:%d:%d:%d", &t.year,
                        &t.mon, &t.mday, &t.hour, &t.min, &t.sec, &f));
    rc = ty_tm_to_timestamp(&t, f, &r);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(std::stoll(v.res[0]) == r);
    }
  } else if (name == "add_interval" || name == "sub_interval") {
    ty_interval_t iv = iv_of(v.args[1]);
    int64_t r = 0;

    rc = name == "add_interval" ? ty_timestamp_add_interval(i64(0), &iv, &r)
                                : ty_timestamp_sub_interval(i64(0), &iv, &r);
    check_num(rc, r);
  } else {
    ty_interval_t iv;

    rc = ty_timestamp_diff(i64(0), i64(1), &iv);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(v.res[0] == iv_token(iv));
    }
  }
  return true;
}

/* A float token: hex, nan, inf or -inf. */
double geo_f(const std::string &t) {
  if (t == "nan") {
    return NAN;
  }
  if (t == "inf") {
    return INFINITY;
  }
  if (t == "-inf") {
    return -INFINITY;
  }
  return strtod(t.c_str(), nullptr);
}

/* Two floats the same, NaN with NaN, and zero with zero of any sign. */
bool geo_same(double a, double b) {
  return std::isnan(a) ? std::isnan(b) : a == b;
}

/* A point token "x:y". */
ty_point_t pt_of(const std::string &t) {
  ty_point_t p;
  size_t c = t.find(':');

  p.x = geo_f(t.substr(0, c));
  p.y = geo_f(t.substr(c + 1));
  return p;
}

/* A box token "hx:hy:lx:ly". */
ty_box_t bx_of(const std::string &t) {
  ty_box_t b;
  std::vector<std::string> f;
  size_t s = 0, c;

  while ((c = t.find(':', s)) != std::string::npos) {
    f.push_back(t.substr(s, c - s));
    s = c + 1;
  }
  f.push_back(t.substr(s));
  b.high.x = geo_f(f[0]);
  b.high.y = geo_f(f[1]);
  b.low.x = geo_f(f[2]);
  b.low.y = geo_f(f[3]);
  return b;
}

/* Whether a result token (a point, a box, a float) equals the value. */
bool geo_check(const std::string &tok, const ty_point_t &p) {
  ty_point_t q = pt_of(tok);

  return geo_same(p.x, q.x) && geo_same(p.y, q.y);
}

bool geo_check(const std::string &tok, const ty_box_t &b) {
  ty_box_t q = bx_of(tok);

  return geo_same(b.high.x, q.high.x) && geo_same(b.high.y, q.high.y) &&
         geo_same(b.low.x, q.low.x) && geo_same(b.low.y, q.low.y);
}

/* Run one point or box vector. */
bool run_geo(const vec_t &v) {
  const std::string &op = v.op;
  const bool pt = op.compare(0, 3, "pt.") == 0,
             bx = op.compare(0, 3, "bx.") == 0;

  if (!pt && !bx) {
    return false;
  }
  const std::string name = op.substr(3);
  int rc = 0, flag = 0;
  double d = 0;
  ty_point_t p, q, rp;
  ty_box_t a, b, rb;

  INFO(v.line);
  /* the result: an error, or a value to compare */
  auto finish = [&](int rcode, auto check) {
    if (v.res[0] == "E") {
      CHECK(-1 == rcode);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rcode);
      CHECK(check());
    }
  };

  if (pt) {
    if (name == "hypot") {
      rc = ty_hypot(geo_f(v.args[0]), geo_f(v.args[1]), &d);
      finish(rc, [&] { return geo_same(d, geo_f(v.res[0])); });
      return true;
    }
    p = pt_of(v.args[0]);
    q = pt_of(v.args[1]);
    if (name == "eq" || name == "left" || name == "right" || name == "above" ||
        name == "below" || name == "vert" || name == "horiz") {
      flag = name == "eq"      ? ty_point_eq(&p, &q)
             : name == "left"  ? ty_point_left(&p, &q)
             : name == "right" ? ty_point_right(&p, &q)
             : name == "above" ? ty_point_above(&p, &q)
             : name == "below" ? ty_point_below(&p, &q)
             : name == "vert"  ? ty_point_vert(&p, &q)
                               : ty_point_horiz(&p, &q);
      CHECK(std::stoi(v.res[0]) == flag);
    } else if (name == "distance" || name == "slope" || name == "invslope") {
      rc = name == "distance" ? ty_point_distance(&p, &q, &d)
           : name == "slope"  ? ty_point_slope(&p, &q, &d)
                              : ty_point_invslope(&p, &q, &d);
      finish(rc, [&] { return geo_same(d, geo_f(v.res[0])); });
    } else {
      rc = name == "add"   ? ty_point_add(&p, &q, &rp)
           : name == "sub" ? ty_point_sub(&p, &q, &rp)
           : name == "mul" ? ty_point_mul(&p, &q, &rp)
                           : ty_point_div(&p, &q, &rp);
      finish(rc, [&] { return geo_check(v.res[0], rp); });
    }
    return true;
  }
  if (name == "construct") {
    p = pt_of(v.args[0]);
    q = pt_of(v.args[1]);
    ty_box_construct(&rb, &p, &q);
    CHECK(geo_check(v.res[0], rb));
    return true;
  }
  a = bx_of(v.args[0]);
  if (name == "normalize") {
    ty_box_normalize(&a);
    CHECK(geo_check(v.res[0], a));
  } else if (name == "width" || name == "height" || name == "area") {
    rc = name == "width"    ? ty_box_width(&a, &d)
         : name == "height" ? ty_box_height(&a, &d)
                            : ty_box_area(&a, &d);
    finish(rc, [&] { return geo_same(d, geo_f(v.res[0])); });
  } else if (name == "center") {
    rc = ty_box_center(&a, &rp);
    finish(rc, [&] { return geo_check(v.res[0], rp); });
  } else if (name == "add" || name == "sub" || name == "mul" || name == "div") {
    p = pt_of(v.args[1]);
    rc = name == "add"   ? ty_box_add(&a, &p, &rb)
         : name == "sub" ? ty_box_sub(&a, &p, &rb)
         : name == "mul" ? ty_box_mul(&a, &p, &rb)
                         : ty_box_div(&a, &p, &rb);
    finish(rc, [&] { return geo_check(v.res[0], rb); });
  } else {
    b = bx_of(v.args[1]);
    if (name == "distance") {
      rc = ty_box_distance(&a, &b, &d);
      finish(rc, [&] { return geo_same(d, geo_f(v.res[0])); });
    } else if (name == "intersect") {
      int got = ty_box_intersect(&a, &b, &rb);

      if (v.res[0] == "-") {
        CHECK(0 == got);
      } else {
        REQUIRE(1 == got);
        CHECK(geo_check(v.res[0], rb));
      }
    } else if (name == "lt" || name == "gt" || name == "eq" || name == "le" ||
               name == "ge") {
      rc = name == "lt"   ? ty_box_lt(&a, &b, &flag)
           : name == "gt" ? ty_box_gt(&a, &b, &flag)
           : name == "eq" ? ty_box_eq(&a, &b, &flag)
           : name == "le" ? ty_box_le(&a, &b, &flag)
                          : ty_box_ge(&a, &b, &flag);
      finish(rc, [&] { return std::stoi(v.res[0]) == flag; });
    } else {
      flag = name == "same"        ? ty_box_same(&a, &b)
             : name == "overlap"   ? ty_box_overlap(&a, &b)
             : name == "contain"   ? ty_box_contain_box(&a, &b)
             : name == "left"      ? ty_box_left(&a, &b)
             : name == "overleft"  ? ty_box_overleft(&a, &b)
             : name == "right"     ? ty_box_right(&a, &b)
             : name == "overright" ? ty_box_overright(&a, &b)
             : name == "below"     ? ty_box_below(&a, &b)
             : name == "overbelow" ? ty_box_overbelow(&a, &b)
             : name == "above"     ? ty_box_above(&a, &b)
             : name == "overabove" ? ty_box_overabove(&a, &b)
             : name == "below_eq"  ? ty_box_below_eq(&a, &b)
                                   : ty_box_above_eq(&a, &b);
      CHECK(std::stoi(v.res[0]) == flag);
    }
  }
  return true;
}

/* Run one text vector: the strings are the hex of UTF-8 bytes. */
bool run_text(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 5, "text.") != 0) {
    return false;
  }
  const std::string name = op.substr(5);
  auto str = [&](size_t i) {
    std::vector<uint8_t> b = bytes_of(v.args[i]);
    return std::string(reinterpret_cast<const char *>(b.data()), b.size());
  };
  auto tok = [](const char *p, size_t n) {
    return token_of(reinterpret_cast<const uint8_t *>(p), n);
  };
  auto num = [&](size_t i) { return std::stoll(v.args[i]); };
  /* a result that is a text, or an error */
  auto check = [&](int rc, const std::string &got) {
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(v.res[0] == got);
    }
  };
  std::string a = str(0), b, c;
  size_t off = 0, n = 0;
  int rc = 0;

  if (v.args.size() > 1 && name != "left" && name != "right" &&
      name != "repeat" && name != "chr" && name != "length" &&
      name != "substr" && name != "like" && name != "like_escape" &&
      name != "lower" && name != "upper" && name != "initcap") {
    b = str(1);
  }
  if (name == "chr") {
    char buf[8];

    rc = ty_text_chr((int32_t)num(0), buf, &n);
    check(rc, tok(buf, n));
    return true;
  }
  if (v.args.size() > 2 && name != "substr" && name != "overlay" &&
      name != "split_part" && name != "lpad" && name != "rpad" &&
      name != "trim") {
    c = str(2);
  }
  INFO(v.line);
  if (name == "length") {
    CHECK(std::stoll(v.res[0]) == (int64_t)ty_text_length(a.data(), a.size()));
  } else if (name == "cmp") {
    int r = ty_text_cmp(a.data(), a.size(), b.data(), b.size());
    CHECK(std::stoi(v.res[0]) == (r > 0) - (r < 0));
  } else if (name == "starts_with") {
    CHECK(std::stoi(v.res[0]) ==
          ty_text_starts_with(a.data(), a.size(), b.data(), b.size()));
  } else if (name == "position") {
    CHECK(std::stoi(v.res[0]) ==
          ty_text_position(a.data(), a.size(), b.data(), b.size()));
  } else if (name == "reverse") {
    std::string out(a.size() + 1, '\0');

    ty_text_reverse(a.data(), a.size(), &out[0]);
    CHECK(v.res[0] == tok(out.data(), a.size()));
  } else if (name == "replace") {
    int found = 0;
    size_t total = 0;

    rc = ty_text_replace_size(a.data(), a.size(), b.data(), b.size(), c.size(),
                              &total, &found);
    REQUIRE(0 == rc);
    if (!found) {
      CHECK(v.res[0] == tok(a.data(), a.size()));
    } else {
      std::string out(total + 1, '\0');

      ty_text_replace(a.data(), a.size(), b.data(), b.size(), c.data(),
                      c.size(), &out[0]);
      CHECK(v.res[0] == tok(out.data(), total));
    }
  } else if (name == "left" || name == "right") {
    if (name == "left") {
      ty_text_left(a.data(), a.size(), (int32_t)num(1), &off, &n);
    } else {
      ty_text_right(a.data(), a.size(), (int32_t)num(1), &off, &n);
    }
    CHECK(v.res[0] == tok(a.data() + off, n));
  } else if (name == "substr") {
    rc = ty_text_substr(a.data(), a.size(), (int32_t)num(1), (int32_t)num(2),
                        (int)num(3), &off, &n);
    check(rc, tok(a.data() + off, n));
  } else if (name == "overlay") {
    std::string r = str(1), out(2 * a.size() + r.size() + 1, '\0');

    rc = ty_text_overlay(a.data(), a.size(), r.data(), r.size(),
                         (int32_t)num(2), (int32_t)num(3), &out[0], &n);
    check(rc, tok(out.data(), n));
  } else if (name == "split_part") {
    std::string sep = str(1);

    rc = ty_text_split_part(a.data(), a.size(), sep.data(), sep.size(),
                            (int32_t)num(2), &off, &n);
    check(rc, tok(a.data() + off, n));
  } else if (name == "repeat") {
    rc = ty_text_repeat_size(a.size(), (int32_t)num(1), &n);
    if (rc == 0) {
      std::string out(n + 1, '\0');

      ty_text_repeat(a.data(), a.size(), (int32_t)num(1), &out[0]);
      check(rc, tok(out.data(), n));
    } else {
      check(rc, "");
    }
  } else if (name == "lpad" || name == "rpad") {
    std::string fill = str(2);
    int32_t eff = 0;

    rc = ty_text_pad_prepare(a.data(), a.size(), (int32_t)num(1), fill.size(),
                             &eff);
    if (rc == 0) {
      std::string out((size_t)eff * 4 + 1, '\0');

      if (name == "lpad") {
        ty_text_lpad(a.data(), a.size(), eff, fill.data(), fill.size(), &out[0],
                     &n);
      } else {
        ty_text_rpad(a.data(), a.size(), eff, fill.data(), fill.size(), &out[0],
                     &n);
      }
      check(rc, tok(out.data(), n));
    } else {
      check(rc, "");
    }
  } else if (name == "trim") {
    std::string set = str(1), mode = v.args[2];

    ty_text_trim(a.data(), a.size(), set.data(), set.size(), mode != "r",
                 mode != "l", &off, &n);
    CHECK(v.res[0] == tok(a.data() + off, n));
  } else if (name == "translate") {
    std::string out(a.size() * 4 + 1, '\0');

    ty_text_translate(a.data(), a.size(), b.data(), b.size(), c.data(),
                      c.size(), &out[0], &n);
    CHECK(v.res[0] == tok(out.data(), n));
  } else if (name == "lower" || name == "upper" || name == "initcap") {
    std::string out(a.size() + 1, '\0');

    if (name == "lower") {
      ty_ascii_lower(a.data(), a.size(), &out[0]);
    } else if (name == "upper") {
      ty_ascii_upper(a.data(), a.size(), &out[0]);
    } else {
      ty_ascii_initcap(a.data(), a.size(), &out[0]);
    }
    CHECK(v.res[0] == tok(out.data(), a.size()));
  } else if (name == "like") {
    std::string pat = str(1);

    rc = ty_text_like(a.data(), a.size(), pat.data(), pat.size(), 1);
    CHECK(std::stoi(v.res[0]) == rc);
  } else if (name == "like_escape") {
    std::string pat = a, esc = str(1), out(2 * pat.size() + 1, '\0');

    rc = ty_text_like_escape(pat.data(), pat.size(), esc.data(), esc.size(), 1,
                             &out[0], &n);
    check(rc, tok(out.data(), n));
  } else if (name == "ascii") {
    int32_t r = 0;

    rc = ty_text_ascii(a.data(), a.size(), &r);
    check_int(v, rc, r);
  } else {
    return false;
  }
  return true;
}

/*
 * The text of date, time, timestamp and interval (known.txt): what the old
 * digres printed in a style, as hex.
 */
bool run_datetime_text(const vec_t &v) {
  const std::string &op = v.op;
  char out[TY_DATETIME_TEXT_MAX];
  size_t n = 0;

  INFO(v.line);
  if (op == "iv.out") {
    int style = std::stoi(v.args[0]);
    long long m, d, t;

    REQUIRE(3 == sscanf(v.args[1].c_str(), "%lld:%lld:%lld", &m, &d, &t));
    ty_interval_t iv = {t, (int32_t)d, (int32_t)m};
    ty_itm_t itm;

    ty_interval_to_itm(&iv, &itm);
    n = ty_interval_format(&itm, style, out);
  } else {
    int style = std::stoi(v.args[0]), order = std::stoi(v.args[1]);
    long long x = std::stoll(v.args[2]);
    ty_tm_t tm = {};
    int32_t fsec = 0;

    if (op == "date.out") {
      ty_j2date((int)(x + TY_POSTGRES_EPOCH_JDATE), &tm.year, &tm.mon,
                &tm.mday);
      n = ty_date_format(&tm, style, order, out);
    } else if (op == "time.out") {
      tm.hour = (int32_t)(x / TY_USECS_PER_HOUR);
      tm.min = (int32_t)(x / TY_USECS_PER_MINUTE % 60);
      tm.sec = (int32_t)(x / TY_USECS_PER_SEC % 60);
      fsec = (int32_t)(x % TY_USECS_PER_SEC);
      n = ty_time_format(&tm, fsec, 0, 0, style, out);
    } else {
      REQUIRE(0 == ty_timestamp_to_tm(x, &tm, &fsec));
      n = ty_timestamp_format(&tm, fsec, 0, 0, nullptr, -1, style, order, out);
    }
  }
  CHECK(v.res[0] == token_of(reinterpret_cast<const uint8_t *>(out), n));
  return true;
}

/* The text of a timestamp result, ISO; or infinity. */
std::string ts_text(int64_t t) {
  if (t == TY_TIMESTAMP_NOEND) {
    return "infinity";
  }
  if (t == TY_TIMESTAMP_NOBEGIN) {
    return "-infinity";
  }
  ty_tm_t tm;
  int32_t fsec;
  char out[TY_DATETIME_TEXT_MAX];

  REQUIRE(0 == ty_timestamp_to_tm(t, &tm, &fsec));
  ty_timestamp_format(&tm, fsec, 0, 0, nullptr, -1, TY_DATES_ISO,
                      TY_DATEORDER_YMD, out);
  return out;
}

/* The text of an interval result, postgres style; or infinity. */
std::string iv_text(const ty_interval_t &iv) {
  if (ty_interval_is_noend(&iv)) {
    return "infinity";
  }
  if (ty_interval_is_nobegin(&iv)) {
    return "-infinity";
  }
  ty_itm_t itm;
  char out[TY_DATETIME_TEXT_MAX];

  ty_interval_to_itm(&iv, &itm);
  ty_interval_format(&itm, TY_INTSTYLE_POSTGRES, out);
  return out;
}

/* num / 10^scale as numeric prints it, with exactly scale decimals. */
std::string scaled_text(__int128 num, int scale) {
  bool neg = num < 0;
  unsigned __int128 u = neg ? -(unsigned __int128)num : (unsigned __int128)num;
  std::string digits;

  do {
    digits.insert(digits.begin(), (char)('0' + (int)(u % 10)));
    u /= 10;
  } while (u != 0);
  while ((int)digits.size() <= scale) {
    digits.insert(digits.begin(), '0');
  }
  if (scale > 0) {
    digits.insert(digits.size() - scale, ".");
  }
  return neg ? "-" + digits : digits;
}

/*
 * extract and date_part (known.txt): the exact value as numeric prints it,
 * and the float8 that date_part gives, or the kind of the error.
 */
bool run_part_known(const vec_t &v) {
  const std::string kind = v.op.substr(5);
  std::vector<uint8_t> ub = bytes_of(v.args[0]);
  std::string unit(reinterpret_cast<const char *>(ub.data()), ub.size());
  int code = 0;
  int found = ty_datetime_part_unit(unit.c_str(),
                                    kind == "iv" || kind == "time", &code);
  ty_part_t p;
  int rc = 0;

  INFO(v.line);
  if (v.res[0] == "E" && v.res[1] == "UNKNOWN") {
    CHECK(0 == found);
    return true;
  }
  REQUIRE(0 != found);
  if (kind == "ts") {
    rc = ty_timestamp_part(found == 2, code, std::stoll(v.args[1]), &p);
  } else if (kind == "date") {
    rc = ty_date_part(found == 2, code, (int32_t)std::stoll(v.args[1]), &p);
  } else if (kind == "time") {
    rc = ty_time_part(found == 2, code, std::stoll(v.args[1]), &p);
  } else {
    long long m, d, t;

    REQUIRE(3 == sscanf(v.args[1].c_str(), "%lld:%lld:%lld", &m, &d, &t));
    ty_interval_t iv = {t, (int32_t)d, (int32_t)m};

    rc = ty_interval_part(found == 2, code, &iv, &p);
  }
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
    return true;
  }
  REQUIRE(0 == rc);
  std::vector<uint8_t> nb = bytes_of(v.res[0]);
  std::string numtext(reinterpret_cast<const char *>(nb.data()), nb.size());
  std::vector<uint8_t> fb = bytes_of(v.res[1]);
  std::string ftext(reinterpret_cast<const char *>(fb.data()), fb.size());

  if (kind != "date") {
    /* date has no float8 date_part: its second column is its numeric */
    CHECK(strtod(ftext.c_str(), nullptr) == p.f);
  }
  switch (p.kind) {
  case TY_PART_INT:
    CHECK(numtext == std::to_string(p.num));
    break;
  case TY_PART_SCALED:
    CHECK(numtext == scaled_text(p.num, p.scale));
    break;
  case TY_PART_SUM:
    CHECK(numtext == scaled_text((__int128)p.num2 * 1000000 + p.num, 6));
    break;
  default:
    /* julian: the numeric has the division's scale; the day is the integer */
    CHECK(0 == numtext.compare(0, std::to_string(p.num2).size(),
                               std::to_string(p.num2)));
    break;
  }
  return true;
}

/*
 * date_trunc, age and date_bin of timestamps (known.txt): what the old digres
 * printed, or the kind of the error.
 */
bool run_ts_known(const vec_t &v) {
  const std::string &op = v.op;
  int64_t r = 0;
  ty_interval_t iv;
  int rc = 0;
  bool unknown = false;

  INFO(v.line);
  if (op == "iv.trunc") {
    std::vector<uint8_t> ub = bytes_of(v.args[0]);
    std::string unit(reinterpret_cast<const char *>(ub.data()), ub.size());
    int code = 0;
    long long m, d, t;

    for (char &c : unit) {
      c = (char)tolower((unsigned char)c);
    }
    if (v.args[1] == "inf" || v.args[1] == "-inf") {
      m = v.args[1] == "inf" ? INT32_MAX : INT32_MIN;
      d = m;
      t = v.args[1] == "inf" ? INT64_MAX : INT64_MIN;
    } else {
      REQUIRE(3 == sscanf(v.args[1].c_str(), "%lld:%lld:%lld", &m, &d, &t));
    }
    ty_interval_t in = {t, (int32_t)d, (int32_t)m};

    if (!ty_datetime_unit(unit.c_str(), &code)) {
      unknown = true;
    } else {
      rc = ty_interval_trunc(code, &in, &iv);
    }
  } else if (op == "ts.trunc") {
    std::vector<uint8_t> ub = bytes_of(v.args[0]);
    std::string unit(reinterpret_cast<const char *>(ub.data()), ub.size());
    int code = 0;

    for (char &c : unit) {
      c = (char)tolower((unsigned char)c);
    }
    if (!ty_datetime_unit(unit.c_str(), &code)) {
      unknown = true;
    } else {
      rc = ty_timestamp_trunc(code, std::stoll(v.args[1]), &r);
    }
  } else if (op == "ts.age") {
    rc = ty_timestamp_age(std::stoll(v.args[0]), std::stoll(v.args[1]), &iv);
  } else {
    long long m, d, t;

    REQUIRE(3 == sscanf(v.args[0].c_str(), "%lld:%lld:%lld", &m, &d, &t));
    ty_interval_t stride = {t, (int32_t)d, (int32_t)m};

    rc = ty_timestamp_bin(&stride, std::stoll(v.args[1]), std::stoll(v.args[2]),
                          &r);
  }
  if (v.res[0] == "E") {
    if (v.res[1] == "UNKNOWN") {
      CHECK(unknown);
    } else {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    }
    return true;
  }
  REQUIRE(!unknown);
  REQUIRE(0 == rc);
  std::string text =
      (op == "ts.age" || op == "iv.trunc") ? iv_text(iv) : ts_text(r);

  CHECK(v.res[0] ==
        token_of(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  return true;
}

/* An inet token "family:bits:hex" as a value. */
ty_inet_t inet_of(const std::string &tok) {
  ty_inet_t a;
  int fam = 0, bits = 0;
  char hex[40];

  memset(&a, 0, sizeof a);
  REQUIRE(3 == sscanf(tok.c_str(), "%d:%d:%39s", &fam, &bits, hex));
  a.family = fam == 4 ? TY_INET_V4 : TY_INET_V6;
  a.bits = (uint8_t)bits;
  for (size_t i = 0; i < strlen(hex) / 2; i++) {
    unsigned b;

    sscanf(hex + 2 * i, "%2x", &b);
    a.addr[i] = (uint8_t)b;
  }
  return a;
}

/* A value as its token. */
std::string inet_token(const ty_inet_t &a) {
  char t[64];
  int n =
      snprintf(t, sizeof t, "%d:%d:", a.family == TY_INET_V4 ? 4 : 6, a.bits);

  for (int i = 0; i < ty_inet_addrsize(&a); i++) {
    n += snprintf(t + n, sizeof t - (size_t)n, "%02x", a.addr[i]);
  }
  return t;
}

/* Run one inet vector; false if its op is not one. */
bool run_inet(const vec_t &v) {
  const std::string &op = v.op;

  /* inet.parse is the known-answer text vector, in run_known */
  if (op.compare(0, 5, "inet.") != 0 || op == "inet.parse") {
    return false;
  }
  const std::string name = op.substr(5);
  ty_inet_t a = inet_of(v.args[0]), b, r;
  int rc = 0;
  int64_t i64 = 0;
  auto sign = [](int c) { return (c > 0) - (c < 0); };

  memset(&r, 0, sizeof r);
  if (v.args.size() > 1 && name != "add" && name != "cidrmask") {
    b = inet_of(v.args[1]);
  }
  INFO(v.line);
  if (name == "cmp") {
    CHECK(std::stoi(v.res[0]) == sign(ty_inet_cmp(&a, &b)));
  } else if (name == "sub") {
    CHECK(std::stoi(v.res[0]) == ty_inet_sub(&a, &b));
  } else if (name == "subeq") {
    CHECK(std::stoi(v.res[0]) == ty_inet_subeq(&a, &b));
  } else if (name == "sup") {
    CHECK(std::stoi(v.res[0]) == ty_inet_sup(&a, &b));
  } else if (name == "supeq") {
    CHECK(std::stoi(v.res[0]) == ty_inet_supeq(&a, &b));
  } else if (name == "overlap") {
    CHECK(std::stoi(v.res[0]) == ty_inet_overlap(&a, &b));
  } else if (name == "broadcast") {
    ty_inet_broadcast(&a, &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "network") {
    ty_inet_network(&a, &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "netmask") {
    ty_inet_netmask(&a, &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "hostmask") {
    ty_inet_hostmask(&a, &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "not") {
    ty_inet_not(&a, &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "cidrmask") {
    ty_inet_set_masklen_cidr(&a, std::stoi(v.args[1]), &r);
    CHECK(v.res[0] == inet_token(r));
  } else if (name == "merge" || name == "and" || name == "or") {
    rc = name == "merge" ? ty_inet_merge(&a, &b, &r)
         : name == "and" ? ty_inet_and(&a, &b, &r)
                         : ty_inet_or(&a, &b, &r);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(v.res[0] == inet_token(r));
    }
  } else if (name == "add") {
    rc = ty_inet_add(&a, std::stoll(v.args[1]), &r);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(v.res[0] == inet_token(r));
    }
  } else if (name == "subinet") {
    rc = ty_inet_sub_inet(&a, &b, &i64);
    check_int(v, rc, i64);
  } else {
    return false;
  }
  return true;
}

/* The arguments and the result of a geometric known answer. */
struct geo_arg_t {
  ty_point_t p;
  ty_line_t l;
  ty_lseg_t s;
  ty_box_t b;
  ty_circle_t c;
  std::vector<ty_point_t> pts; /* the points of a path or a polygon */
  int closed = 0;              /* a path: closed, or open */
  ty_box_t pbox;               /* a polygon: the box that bounds it */
  int n = 0;                   /* a count of points */
};

struct geo_res_t {
  std::string text;
  bool null = false;
};

/* A float as the server prints it (extra_float_digits 1). */
std::string geo_num(double d) {
  char buf[TY_FLOAT_TEXT_SIZE];

  ty_float64_format(d, 1, buf);
  return buf;
}

std::string geo_point_text(const ty_point_t &p) {
  return "(" + geo_num(p.x) + "," + geo_num(p.y) + ")";
}

std::string geo_circle_text(const ty_circle_t &c) {
  return "<" + geo_point_text(c.center) + "," + geo_num(c.radius) + ">";
}

/* A path or a polygon: [(x,y),...] if it is open, else ((x,y),...). */
std::string geo_points_text(const std::vector<ty_point_t> &pts, int closed) {
  std::string t = closed ? "(" : "[";

  for (size_t i = 0; i < pts.size(); i++) {
    t += (i ? "," : "") + geo_point_text(pts[i]);
  }
  return t + (closed ? ")" : "]");
}

std::string geo_box_text(const ty_box_t &b) {
  return geo_point_text(b.high) + "," + geo_point_text(b.low);
}

/* A number up to one of ",)]}" or the end: NaN, Infinity or a decimal. */
double geo_read_num(const char *&c) {
  const char *e = c;
  double r;

  while (*e != '\0' && strchr(",)]}", *e) == nullptr) {
    e++;
  }
  std::string t(c, e);
  if (t == "NaN") {
    r = NAN;
  } else if (t == "Infinity") {
    r = INFINITY;
  } else if (t == "-Infinity") {
    r = -INFINITY;
  } else {
    r = strtod(t.c_str(), nullptr);
  }
  c = e;
  return r;
}

/* "(x,y)" */
void geo_read_point(const char *&c, ty_point_t &p) {
  c++;
  p.x = geo_read_num(c);
  c++;
  p.y = geo_read_num(c);
  c++;
}

/* Read the argument of a kind: P point, L line, S lseg, B box. */
void geo_read(char kind, const std::string &text, geo_arg_t &a) {
  const char *c = text.c_str();

  if (kind == 'P') {
    geo_read_point(c, a.p);
  } else if (kind == 'L') {
    c++;
    a.l.a = geo_read_num(c);
    c++;
    a.l.b = geo_read_num(c);
    c++;
    a.l.c = geo_read_num(c);
  } else if (kind == 'S') {
    c++;
    geo_read_point(c, a.s.p[0]);
    c++;
    geo_read_point(c, a.s.p[1]);
  } else if (kind == 'N') {
    a.n = atoi(c);
  } else if (kind == 'A' || kind == 'G') {
    /* "[(x,y),...]" is an open path, "((x,y),...)" a closed one, a polygon */
    a.closed = *c != '[';
    c++;
    while (*c == '(') {
      ty_point_t pt;

      geo_read_point(c, pt);
      a.pts.push_back(pt);
      if (*c == ',') {
        c++;
      }
    }
    if (kind == 'G') {
      ty_polygon_bound_box((int)a.pts.size(), a.pts.data(), &a.pbox);
    }
  } else if (kind == 'C') {
    c++;
    geo_read_point(c, a.c.center);
    c++;
    a.c.radius = geo_read_num(c);
  } else {
    geo_read_point(c, a.b.high);
    c++;
    geo_read_point(c, a.b.low);
    ty_box_normalize(&a.b);
  }
}

typedef int (*geo_fn)(const geo_arg_t &, const geo_arg_t &, geo_res_t &);

struct geo_op_t {
  const char *name;
  const char *kinds;
  geo_fn fn;
};

/* One operation: the call, in a lambda that has the arguments x and y. */
#define GEO_BOOL(NAME, K, CALL)                                             \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)x;                                                               \
     (void)y;                                                               \
     int v = 0;                                                             \
     int rc = CALL;                                                         \
     if (rc == 0) {                                                         \
       r.text = v ? "true" : "false";                                       \
     }                                                                      \
     return rc;                                                             \
   }}
#define GEO_PURE(NAME, K, EXPR)                                             \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)x;                                                               \
     (void)y;                                                               \
     r.text = (EXPR) ? "true" : "false";                                    \
     return 0;                                                              \
   }}
#define GEO_FLOAT(NAME, K, CALL)                                            \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)x;                                                               \
     (void)y;                                                               \
     double d = 0;                                                          \
     int rc = CALL;                                                         \
     if (rc == 0) {                                                         \
       r.text = geo_num(d);                                                 \
     }                                                                      \
     return rc;                                                             \
   }}
/* A point result, or NULL when hit is 0. */
#define GEO_POINT(NAME, K, CALL)                                            \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)x;                                                               \
     (void)y;                                                               \
     ty_point_t pt = {0, 0};                                                \
     int hit = 1;                                                           \
     int rc = CALL;                                                         \
     if (rc == 0) {                                                         \
       r.null = !hit;                                                       \
       r.text = geo_point_text(pt);                                         \
     }                                                                      \
     return rc;                                                             \
   }}

/* A path result: the points of the first argument, changed by CALL on v. */
#define GEO_PATH(NAME, K, CALL)                                             \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)y;                                                               \
     std::vector<ty_point_t> v = x.pts;                                     \
     int rc = CALL;                                                         \
     if (rc == 0) {                                                         \
       r.text = geo_points_text(v, x.closed);                               \
     }                                                                      \
     return rc;                                                             \
   }}

/* A circle result. */
#define GEO_CIRCLE(NAME, K, CALL)                                           \
  {NAME, K, [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int { \
     (void)x;                                                               \
     (void)y;                                                               \
     ty_circle_t c;                                                         \
     int rc = CALL;                                                         \
     if (rc == 0) {                                                         \
       r.text = geo_circle_text(c);                                         \
     }                                                                      \
     return rc;                                                             \
   }}

const geo_op_t geo_ops[] = {
    GEO_BOOL("line_intersect", "LL", ty_line_intersect(&x.l, &y.l, &v)),
    GEO_BOOL("line_parallel", "LL", ty_line_parallel(&x.l, &y.l, &v)),
    GEO_BOOL("line_perp", "LL", ty_line_perp(&x.l, &y.l, &v)),
    GEO_BOOL("line_eq", "LL", ty_line_eq(&x.l, &y.l, &v)),
    GEO_PURE("line_vertical", "L", ty_line_vertical(&x.l)),
    GEO_PURE("line_horizontal", "L", ty_line_horizontal(&x.l)),
    GEO_FLOAT("line_distance", "LL", ty_line_distance(&x.l, &y.l, &d)),
    GEO_POINT("line_interpt", "LL", ty_line_interpt(&x.l, &y.l, &pt, &hit)),
    {"line_construct_pp", "PP",
     [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int {
       ty_line_t l;
       int rc = ty_line_from_points(&x.p, &y.p, &l);

       if (rc == 0) {
         r.text =
             "{" + geo_num(l.a) + "," + geo_num(l.b) + "," + geo_num(l.c) + "}";
       }
       return rc;
     }},
    GEO_FLOAT("lseg_length", "S", ty_lseg_length(&x.s, &d)),
    GEO_BOOL("lseg_intersect", "SS", ty_lseg_intersect(&x.s, &y.s, &v)),
    GEO_BOOL("lseg_parallel", "SS", ty_lseg_parallel(&x.s, &y.s, &v)),
    GEO_BOOL("lseg_perp", "SS", ty_lseg_perp(&x.s, &y.s, &v)),
    GEO_PURE("lseg_vertical", "S", ty_lseg_vertical(&x.s)),
    GEO_PURE("lseg_horizontal", "S", ty_lseg_horizontal(&x.s)),
    GEO_PURE("lseg_eq", "SS", ty_lseg_eq(&x.s, &y.s)),
    GEO_PURE("lseg_ne", "SS", !ty_lseg_eq(&x.s, &y.s)),
    GEO_BOOL("lseg_lt", "SS", ty_lseg_lt(&x.s, &y.s, &v)),
    GEO_BOOL("lseg_le", "SS", ty_lseg_le(&x.s, &y.s, &v)),
    GEO_BOOL("lseg_gt", "SS", ty_lseg_gt(&x.s, &y.s, &v)),
    GEO_BOOL("lseg_ge", "SS", ty_lseg_ge(&x.s, &y.s, &v)),
    GEO_FLOAT("lseg_distance", "SS",
              ty_lseg_lseg_distance(&x.s, &y.s, nullptr, &d)),
    {"lseg_center", "S",
     [](const geo_arg_t &x, const geo_arg_t &, geo_res_t &r) -> int {
       ty_point_t pt;
       int rc = ty_lseg_center(&x.s, &pt);

       if (rc == 0) {
         r.text = geo_point_text(pt);
       }
       return rc;
     }},
    GEO_POINT("lseg_interpt", "SS", ty_lseg_interpt(&x.s, &y.s, &pt, &hit)),
    {"box_diagonal", "B",
     [](const geo_arg_t &x, const geo_arg_t &, geo_res_t &r) -> int {
       ty_lseg_t s;

       ty_box_diagonal(&x.b, &s);
       r.text =
           "[" + geo_point_text(s.p[0]) + "," + geo_point_text(s.p[1]) + "]";
       return 0;
     }},
    GEO_FLOAT("dist_pl", "PL", ty_point_line_distance(&x.p, &y.l, nullptr, &d)),
    GEO_FLOAT("dist_lp", "LP", ty_point_line_distance(&y.p, &x.l, nullptr, &d)),
    GEO_FLOAT("dist_ps", "PS", ty_point_lseg_distance(&x.p, &y.s, nullptr, &d)),
    GEO_FLOAT("dist_sp", "SP", ty_point_lseg_distance(&y.p, &x.s, nullptr, &d)),
    GEO_FLOAT("dist_pb", "PB", ty_point_box_distance(&x.p, &y.b, nullptr, &d)),
    GEO_FLOAT("dist_bp", "BP", ty_point_box_distance(&y.p, &x.b, nullptr, &d)),
    GEO_FLOAT("dist_sl", "SL", ty_lseg_line_distance(&x.s, &y.l, nullptr, &d)),
    GEO_FLOAT("dist_ls", "LS", ty_lseg_line_distance(&y.s, &x.l, nullptr, &d)),
    GEO_FLOAT("dist_sb", "SB", ty_lseg_box_distance(&x.s, &y.b, nullptr, &d)),
    GEO_FLOAT("dist_bs", "BS", ty_lseg_box_distance(&y.s, &x.b, nullptr, &d)),
    GEO_POINT("close_pl", "PL", ty_point_line_close(&x.p, &y.l, &pt, &hit)),
    GEO_POINT("close_ps", "PS", ty_point_lseg_close(&x.p, &y.s, &pt, &hit)),
    GEO_POINT("close_lseg", "SS", ty_lseg_lseg_close(&x.s, &y.s, &pt, &hit)),
    GEO_POINT("close_pb", "PB", ty_point_box_close(&x.p, &y.b, &pt, &hit)),
    GEO_POINT("close_ls", "LS", ty_lseg_line_close(&x.l, &y.s, &pt, &hit)),
    GEO_POINT("close_sb", "SB", ty_lseg_box_close(&x.s, &y.b, &pt, &hit)),
    GEO_BOOL("on_pl", "PL", ty_line_contain_point(&y.l, &x.p, &v)),
    GEO_BOOL("on_ps", "PS", ty_lseg_contain_point(&y.s, &x.p, &v)),
    GEO_PURE("on_pb", "PB", ty_box_contain_point(&y.b, &x.p)),
    GEO_BOOL("on_sl", "SL", ty_lseg_on_line(&x.s, &y.l, &v)),
    GEO_PURE("on_sb", "SB", ty_box_contain_lseg(&y.b, &x.s)),
    GEO_PURE("box_contain_pt", "BP", ty_box_contain_point(&x.b, &y.p)),
    GEO_BOOL("inter_sl", "SL", ty_lseg_interpt_line(&x.s, &y.l, nullptr, &v)),
    GEO_BOOL("inter_sb", "SB", ty_box_interpt_lseg(&y.b, &x.s, nullptr, &v)),
    GEO_BOOL("inter_lb", "LB", ty_box_interpt_line(&y.b, &x.l, &v)),
    {"path_area", "A",
     [](const geo_arg_t &x, const geo_arg_t &, geo_res_t &r) -> int {
       double d;

       if (!x.closed) {
         r.null = true;
         return 0;
       }
       int rc = ty_path_area((int)x.pts.size(), x.pts.data(), &d);

       if (rc == 0) {
         r.text = geo_num(d);
       }
       return rc;
     }},
    GEO_BOOL("path_inter", "AA",
             ty_path_intersect((int)x.pts.size(), x.pts.data(), x.closed,
                               (int)y.pts.size(), y.pts.data(), y.closed, &v)),
    {"path_distance", "AA",
     [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int {
       double d;
       int hit;
       int rc = ty_path_distance((int)x.pts.size(), x.pts.data(), x.closed,
                                 (int)y.pts.size(), y.pts.data(), y.closed, &d,
                                 &hit);

       if (rc == 0) {
         r.null = !hit;
         r.text = geo_num(d);
       }
       return rc;
     }},
    GEO_FLOAT("path_length", "A",
              ty_path_length((int)x.pts.size(), x.pts.data(), x.closed, &d)),
    GEO_PATH("path_add_pt", "AP", ty_points_add((int)v.size(), v.data(), &y.p)),
    GEO_PATH("path_sub_pt", "AP", ty_points_sub((int)v.size(), v.data(), &y.p)),
    GEO_PATH("path_mul_pt", "AP", ty_points_mul((int)v.size(), v.data(), &y.p)),
    GEO_PATH("path_div_pt", "AP", ty_points_div((int)v.size(), v.data(), &y.p)),
    GEO_BOOL(
        "on_ppath", "PA",
        ty_point_on_path(&x.p, (int)y.pts.size(), y.pts.data(), y.closed, &v)),
    GEO_FLOAT("dist_ppath", "PA",
              ty_point_path_distance(&x.p, (int)y.pts.size(), y.pts.data(),
                                     y.closed, &d)),
    GEO_FLOAT("dist_pathp", "AP",
              ty_point_path_distance(&y.p, (int)x.pts.size(), x.pts.data(),
                                     x.closed, &d)),
    GEO_PURE("poly_left", "GG", ty_polygon_left(&x.pbox, &y.pbox)),
    GEO_PURE("poly_overleft", "GG", ty_polygon_overleft(&x.pbox, &y.pbox)),
    GEO_PURE("poly_right", "GG", ty_polygon_right(&x.pbox, &y.pbox)),
    GEO_PURE("poly_overright", "GG", ty_polygon_overright(&x.pbox, &y.pbox)),
    GEO_PURE("poly_below", "GG", ty_polygon_below(&x.pbox, &y.pbox)),
    GEO_PURE("poly_overbelow", "GG", ty_polygon_overbelow(&x.pbox, &y.pbox)),
    GEO_PURE("poly_above", "GG", ty_polygon_above(&x.pbox, &y.pbox)),
    GEO_PURE("poly_overabove", "GG", ty_polygon_overabove(&x.pbox, &y.pbox)),
    GEO_PURE("poly_same", "GG",
             ty_polygon_same((int)x.pts.size(), x.pts.data(), (int)y.pts.size(),
                             y.pts.data())),
    GEO_BOOL("poly_overlap", "GG",
             ty_polygon_overlap((int)x.pts.size(), x.pts.data(), &x.pbox,
                                (int)y.pts.size(), y.pts.data(), &y.pbox, &v)),
    GEO_BOOL("poly_contain", "GG",
             ty_polygon_contain((int)x.pts.size(), x.pts.data(), &x.pbox,
                                (int)y.pts.size(), y.pts.data(), &y.pbox,
                                nullptr, &v)),
    GEO_BOOL("poly_contained", "GG",
             ty_polygon_contain((int)y.pts.size(), y.pts.data(), &y.pbox,
                                (int)x.pts.size(), x.pts.data(), &x.pbox,
                                nullptr, &v)),
    GEO_BOOL("poly_contain_pt", "GP",
             (v = ty_point_in_polygon(&y.p, (int)x.pts.size(), x.pts.data())) <
                     0
                 ? -1
                 : (v = v != 0, 0)),
    GEO_BOOL("pt_contained_poly", "PG",
             (v = ty_point_in_polygon(&x.p, (int)y.pts.size(), y.pts.data())) <
                     0
                 ? -1
                 : (v = v != 0, 0)),
    GEO_FLOAT("poly_distance", "GG",
              ty_polygon_distance((int)x.pts.size(), x.pts.data(), &x.pbox,
                                  (int)y.pts.size(), y.pts.data(), &y.pbox,
                                  &d)),
    {"poly_center", "G",
     [](const geo_arg_t &x, const geo_arg_t &, geo_res_t &r) -> int {
       ty_circle_t c;
       int rc = ty_polygon_to_circle((int)x.pts.size(), x.pts.data(), &c);

       if (rc == 0) {
         r.text = geo_point_text(c.center);
       }
       return rc;
     }},
    GEO_CIRCLE("poly_circle", "G",
               ty_polygon_to_circle((int)x.pts.size(), x.pts.data(), &c)),
    GEO_FLOAT(
        "dist_ppoly", "PG",
        ty_point_polygon_distance(&x.p, (int)y.pts.size(), y.pts.data(), &d)),
    GEO_FLOAT(
        "dist_polyp", "GP",
        ty_point_polygon_distance(&y.p, (int)x.pts.size(), x.pts.data(), &d)),
    GEO_FLOAT(
        "dist_cpoly", "CG",
        ty_circle_polygon_distance(&x.c, (int)y.pts.size(), y.pts.data(), &d)),
    GEO_FLOAT(
        "dist_polyc", "GC",
        ty_circle_polygon_distance(&y.c, (int)x.pts.size(), x.pts.data(), &d)),
    {"circle_poly", "NC",
     [](const geo_arg_t &x, const geo_arg_t &y, geo_res_t &r) -> int {
       std::vector<ty_point_t> v((size_t)x.n);
       int rc = ty_circle_to_polygon_points(x.n, &y.c, v.data());

       if (rc == 0) {
         r.text = geo_points_text(v, 1);
       }
       return rc;
     }},
    GEO_PURE("circle_same", "CC", ty_circle_same(&x.c, &y.c)),
    GEO_BOOL("circle_overlap", "CC", ty_circle_overlap(&x.c, &y.c, &v)),
    GEO_BOOL("circle_overleft", "CC", ty_circle_overleft(&x.c, &y.c, &v)),
    GEO_BOOL("circle_left", "CC", ty_circle_left(&x.c, &y.c, &v)),
    GEO_BOOL("circle_right", "CC", ty_circle_right(&x.c, &y.c, &v)),
    GEO_BOOL("circle_overright", "CC", ty_circle_overright(&x.c, &y.c, &v)),
    GEO_BOOL("circle_contained", "CC", ty_circle_contained(&x.c, &y.c, &v)),
    GEO_BOOL("circle_contain", "CC", ty_circle_contain(&x.c, &y.c, &v)),
    GEO_BOOL("circle_below", "CC", ty_circle_below(&x.c, &y.c, &v)),
    GEO_BOOL("circle_above", "CC", ty_circle_above(&x.c, &y.c, &v)),
    GEO_BOOL("circle_overbelow", "CC", ty_circle_overbelow(&x.c, &y.c, &v)),
    GEO_BOOL("circle_overabove", "CC", ty_circle_overabove(&x.c, &y.c, &v)),
    GEO_BOOL("circle_eq", "CC", ty_circle_eq(&x.c, &y.c, &v)),
    GEO_BOOL("circle_ne", "CC", ty_circle_ne(&x.c, &y.c, &v)),
    GEO_BOOL("circle_lt", "CC", ty_circle_lt(&x.c, &y.c, &v)),
    GEO_BOOL("circle_gt", "CC", ty_circle_gt(&x.c, &y.c, &v)),
    GEO_BOOL("circle_le", "CC", ty_circle_le(&x.c, &y.c, &v)),
    GEO_BOOL("circle_ge", "CC", ty_circle_ge(&x.c, &y.c, &v)),
    GEO_CIRCLE("circle_add_pt", "CP", ty_circle_add(&x.c, &y.p, &c)),
    GEO_CIRCLE("circle_sub_pt", "CP", ty_circle_sub(&x.c, &y.p, &c)),
    GEO_CIRCLE("circle_mul_pt", "CP", ty_circle_mul(&x.c, &y.p, &c)),
    GEO_CIRCLE("circle_div_pt", "CP", ty_circle_div(&x.c, &y.p, &c)),
    GEO_FLOAT("circle_area", "C", ty_circle_area(&x.c, &d)),
    GEO_FLOAT("circle_diameter", "C", ty_circle_diameter(&x.c, &d)),
    GEO_FLOAT("circle_distance", "CC", ty_circle_distance(&x.c, &y.c, &d)),
    GEO_BOOL("circle_contain_pt", "CP",
             ty_circle_contain_point(&x.c, &y.p, &v)),
    GEO_BOOL("pt_contained_circle", "PC",
             ty_circle_contain_point(&y.c, &x.p, &v)),
    GEO_FLOAT("dist_pc", "PC", ty_point_circle_distance(&x.p, &y.c, &d)),
    GEO_FLOAT("dist_cpoint", "CP", ty_point_circle_distance(&y.p, &x.c, &d)),
    GEO_CIRCLE("box_circle", "B", ty_box_to_circle(&x.b, &c)),
    {"circle_box", "C",
     [](const geo_arg_t &x, const geo_arg_t &, geo_res_t &r) -> int {
       ty_box_t b;
       int rc = ty_circle_to_box(&x.c, &b);

       if (rc == 0) {
         r.text = geo_box_text(b);
       }
       return rc;
     }},
};

/*
 * Run a known answer of line, lseg and their operators with point and box:
 * "geo.NAME HEX => HEX", the HEX of the argument texts joined with "|".
 */
bool run_geo_known(const vec_t &v) {
  const std::string name = v.op.substr(4);

  for (const geo_op_t &o : geo_ops) {
    if (name != o.name) {
      continue;
    }
    std::vector<uint8_t> tb = bytes_of(v.args[0]);
    std::string all(reinterpret_cast<const char *>(tb.data()), tb.size());
    geo_arg_t a[2];
    geo_res_t r;
    size_t bar = all.find('|');

    INFO(v.line);
    geo_read(o.kinds[0], all.substr(0, bar), a[0]);
    if (o.kinds[1] != '\0') {
      REQUIRE(bar != std::string::npos);
      geo_read(o.kinds[1], all.substr(bar + 1), a[1]);
    }
    int rc = o.fn(a[0], a[1], r);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      /* The server says 22003 for overflow and for underflow. */
      if (v.res[1] == "RANGE") {
        CHECK((ty_errnum() == TY_ERR_RANGE || ty_errnum() == TY_ERR_UNDERFLOW));
      } else {
        CHECK(errnum_of(v.res[1]) == ty_errnum());
      }
    } else {
      REQUIRE(0 == rc);
      std::string want;

      if (v.res[0] != "-") {
        std::vector<uint8_t> wb = bytes_of(v.res[0]);
        want.assign(reinterpret_cast<const char *>(wb.data()), wb.size());
      }
      CHECK(want == (r.null ? std::string() : r.text));
    }
    return true;
  }
  return false;
}

/* Run a vector of what PostgreSQL did with a text (known.txt): the text is hex.
 */
bool run_known(const vec_t &v) {
  const std::string &op = v.op;

  if (op == "inet.parse" || op == "cidr.parse") {
    return run_inet_text(v);
  }
  if (op == "tid.parse" || op == "lsn.parse" || op == "char.parse") {
    return run_small_text(v);
  }
  if (op == "money.parse") {
    std::vector<uint8_t> tb = bytes_of(v.args[0]);
    std::string z(reinterpret_cast<const char *>(tb.data()), tb.size());
    int64_t r = 0;
    int rc = ty_money_parse(z.c_str(), &ty_money_fmt_c, &r);
    char out[128];

    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      ty_money_format(r, &ty_money_fmt_c, out, sizeof out);
      CHECK(v.res[0] == std::string(out));
    }
    return true;
  }
  if (op == "ts.trunc" || op == "ts.age" || op == "ts.bin" ||
      op == "iv.trunc") {
    return run_ts_known(v);
  }
  if (op.compare(0, 4, "geo.") == 0) {
    return run_geo_known(v);
  }
  if (op.compare(0, 5, "part.") == 0) {
    return run_part_known(v);
  }
  if (op == "iv.parse") {
    std::vector<uint8_t> tb = bytes_of(v.args[1]);
    std::string z(reinterpret_cast<const char *>(tb.data()), tb.size());
    int style = std::stoi(v.args[0]), dterr = 0;
    ty_interval_t iv;
    int rc = ty_interval_parse(z.c_str(), 0x7FFF, style, &iv, &dterr);

    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(std::stoi(v.res[1]) == dterr);
    } else {
      REQUIRE(0 == rc);
      std::string text;

      if (ty_interval_is_noend(&iv)) {
        text = "infinity";
      } else if (ty_interval_is_nobegin(&iv)) {
        text = "-infinity";
      } else {
        ty_itm_t itm;
        char out[TY_DATETIME_TEXT_MAX];

        ty_interval_to_itm(&iv, &itm);
        ty_interval_format(&itm, style, out);
        text = out;
      }
      CHECK(v.res[0] == token_of(reinterpret_cast<const uint8_t *>(text.data()),
                                 text.size()));
    }
    return true;
  }
  if (op == "date.out" || op == "time.out" || op == "ts.out" ||
      op == "iv.out") {
    return run_datetime_text(v);
  }
  if (op == "like.pair") {
    std::vector<uint8_t> tb = bytes_of(v.args[0]), pb = bytes_of(v.args[1]);
    int rc =
        ty_text_like(reinterpret_cast<const char *>(tb.data()), tb.size(),
                     reinterpret_cast<const char *>(pb.data()), pb.size(), 1);

    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      CHECK(std::stoi(v.res[0]) == rc);
    }
    return true;
  }
  if (op != "macaddr.parse" && op != "macaddr8.parse") {
    return false;
  }
  std::vector<uint8_t> t = bytes_of(v.args[0]);
  std::string z(reinterpret_cast<const char *>(t.data()), t.size());
  char out[TY_MACADDR8_TEXT_SIZE];
  int rc;

  if (op == "macaddr.parse") {
    ty_macaddr_t a;
    rc = ty_macaddr_parse(z.c_str(), &a);
    if (rc == 0) {
      ty_macaddr_format(&a, out);
    }
  } else {
    ty_macaddr8_t a;
    rc = ty_macaddr8_parse(z.c_str(), &a);
    if (rc == 0) {
      ty_macaddr8_format(&a, out);
    }
  }
  INFO(v.line);
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    CHECK(errnum_of(v.res[1]) == ty_errnum());
  } else {
    REQUIRE(0 == rc);
    CHECK(v.res[0] == std::string(out));
  }
  return true;
}

/* Run a float text vector: parse (the text is hex) or format (known). */
bool run_float_text(const vec_t &v) {
  const std::string &op = v.op;

  if (op == "f32.parse" || op == "f64.parse") {
    std::vector<uint8_t> t = bytes_of(v.args[0]);
    std::string z(reinterpret_cast<const char *>(t.data()), t.size());
    double r = 0;
    float f = 0;
    int rc = 0;

    /* the text has no NUL of its own: one ends the string for strtod */
    if (z.find('\0') != std::string::npos) {
      return true;
    }
    if (op == "f32.parse") {
      rc = ty_float32_parse(z.c_str(), &f, nullptr, nullptr);
      r = f;
    } else {
      rc = ty_float64_parse(z.c_str(), &r, nullptr, nullptr);
    }
    check_float(v, rc, r);
    return true;
  }
  if (op == "f32.fmt" || op == "f64.fmt") {
    char out[TY_FLOAT_TEXT_SIZE];
    int digits = std::stoi(v.args[1]);
    size_t n = op == "f32.fmt"
                   ? ty_float32_format((float)flt(v.args[0]), digits, out)
                   : ty_float64_format(flt(v.args[0]), digits, out);
    INFO(v.line);
    CHECK(v.res[0] == std::string(out, n));
    CHECK(strlen(out) == n);
    return true;
  }
  return false;
}

/* Run one bytea vector; false if its op is not one of them. */
bool run_bytea(const vec_t &v) {
  const std::string &op = v.op;

  if (op.compare(0, 6, "bytea.") != 0) {
    return false;
  }
  std::string name = op.substr(6);
  auto arg = [&](size_t i) { return bytes_of(v.args[i]); };
  auto num = [&](size_t i) { return std::stoll(v.args[i]); };

  if (name == "parse") {
    std::string t = text_of(v.args[0]);
    std::vector<uint8_t> out(t.size() + 1);
    size_t n = 0;
    int rc = ty_bytea_parse(t.data(), t.size(), out.data(), &n);
    out.resize(n);
    check_bytes(v, rc, out);
  } else if (name == "fmthex") {
    std::vector<uint8_t> a = arg(0);
    std::string out(ty_bytea_hex_size(a.size()), '\0');
    ty_bytea_format_hex(a.data(), a.size(), &out[0]);
    INFO(v.line);
    CHECK(v.res[0] == out);
  } else if (name == "fmtesc") {
    std::vector<uint8_t> a = arg(0);
    std::string out(ty_bytea_escape_size(a.data(), a.size()), '\0');
    ty_bytea_format_escape(a.data(), a.size(), &out[0]);
    INFO(v.line);
    CHECK(text_of(v.res[0]) == out);
  } else if (name == "cmp") {
    std::vector<uint8_t> a = arg(0), b = arg(1);
    int c = ty_bytea_cmp(a.data(), a.size(), b.data(), b.size());
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
  } else if (name == "eq" || name == "ne" || name == "lt" || name == "le" ||
             name == "gt" || name == "ge") {
    std::vector<uint8_t> a = arg(0), b = arg(1);
    const uint8_t *pa = a.data(), *pb = b.data();
    int got = name == "eq"   ? ty_bytea_eq(pa, a.size(), pb, b.size())
              : name == "ne" ? ty_bytea_ne(pa, a.size(), pb, b.size())
              : name == "lt" ? ty_bytea_lt(pa, a.size(), pb, b.size())
              : name == "le" ? ty_bytea_le(pa, a.size(), pb, b.size())
              : name == "gt" ? ty_bytea_gt(pa, a.size(), pb, b.size())
                             : ty_bytea_ge(pa, a.size(), pb, b.size());
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == got);
  } else if (name == "enc" || name == "dec") {
    ty_bytea_enc_t enc;
    REQUIRE(0 == ty_bytea_encoding(v.args[0].c_str(), &enc));
    std::vector<uint8_t> in = bytes_of(v.args[1]), out;
    int rc = 0;
    if (name == "enc") {
      out.resize(ty_bytea_encode_size(enc, in.data(), in.size()));
      size_t n = ty_bytea_encode(enc, in.data(), in.size(),
                                 reinterpret_cast<char *>(out.data()));
      REQUIRE(n <= out.size());
      out.resize(n);
    } else {
      size_t size = 0, n = 0;
      rc = ty_bytea_decode_size(enc, reinterpret_cast<const char *>(in.data()),
                                in.size(), &size);
      if (rc == 0) {
        out.resize(size);
        rc = ty_bytea_decode(enc, reinterpret_cast<const char *>(in.data()),
                             in.size(), out.data(), &n);
        out.resize(rc == 0 ? n : 0);
      }
    }
    check_bytes(v, rc, out);
  } else if (name == "cat") {
    std::vector<uint8_t> a = arg(0), b = arg(1), out(a.size() + b.size());
    ty_bytea_cat(a.data(), a.size(), b.data(), b.size(), out.data());
    check_bytes(v, 0, out);
  } else if (name == "reverse") {
    std::vector<uint8_t> a = arg(0), out(a.size());
    ty_bytea_reverse(a.data(), a.size(), out.data());
    check_bytes(v, 0, out);
  } else if (name == "position") {
    std::vector<uint8_t> a = arg(0), b = arg(1);
    INFO(v.line);
    CHECK(std::stoll(v.res[0]) ==
          ty_bytea_position(a.data(), a.size(), b.data(), b.size()));
  } else if (name == "bitcount") {
    std::vector<uint8_t> a = arg(0);
    INFO(v.line);
    CHECK(std::stoll(v.res[0]) == ty_bytea_bit_count(a.data(), a.size()));
  } else if (name == "getbyte" || name == "getbit") {
    std::vector<uint8_t> a = arg(0);
    int32_t r = 0;
    int rc = name == "getbyte"
                 ? ty_bytea_get_byte(a.data(), a.size(), (int32_t)num(1), &r)
                 : ty_bytea_get_bit(a.data(), a.size(), num(1), &r);
    check_int(v, rc, r);
  } else if (name == "setbyte" || name == "setbit") {
    std::vector<uint8_t> a = arg(0);
    int rc =
        name == "setbyte"
            ? ty_bytea_set_byte(a.data(), a.size(), (int32_t)num(1),
                                (int32_t)num(2))
            : ty_bytea_set_bit(a.data(), a.size(), num(1), (int32_t)num(2));
    check_bytes(v, rc, a);
  } else if (name == "substr") {
    int32_t off = 0, count = 0;
    int rc = ty_bytea_substr_range((int32_t)num(0), (int32_t)num(1),
                                   (int)num(2), &off, &count);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(std::stoll(v.res[0]) == off);
      CHECK(std::stoll(v.res[1]) == count);
    }
  } else if (name == "overlay") {
    std::vector<uint8_t> a = arg(0), b = arg(1), out;
    size_t size = 0;
    int rc = ty_bytea_overlay_size(a.size(), b.size(), (int32_t)num(2),
                                   (int32_t)num(3), &size);
    if (rc == 0) {
      out.resize(size);
      rc = ty_bytea_overlay(a.data(), a.size(), b.data(), b.size(),
                            (int32_t)num(2), (int32_t)num(3), out.data());
    }
    check_bytes(v, rc, out);
  } else if (name == "toint16" || name == "toint32" || name == "toint64") {
    std::vector<uint8_t> a = arg(0);
    int16_t r16 = 0;
    int32_t r32 = 0;
    int64_t r64 = 0;
    int rc = name == "toint16"   ? ty_bytea_to_int16(a.data(), a.size(), &r16)
             : name == "toint32" ? ty_bytea_to_int32(a.data(), a.size(), &r32)
                                 : ty_bytea_to_int64(a.data(), a.size(), &r64);
    check_int(v, rc, name == "toint16" ? r16 : name == "toint32" ? r32 : r64);
  } else if (name == "fromint16") {
    uint8_t out[2];
    ty_int16_to_bytea((int16_t)num(0), out);
    INFO(v.line);
    CHECK(v.res[0] == token_of(out, 2));
  } else if (name == "fromint32") {
    uint8_t out[4];
    ty_int32_to_bytea((int32_t)num(0), out);
    INFO(v.line);
    CHECK(v.res[0] == token_of(out, 4));
  } else if (name == "fromint64") {
    uint8_t out[8];
    ty_int64_to_bytea(num(0), out);
    INFO(v.line);
    CHECK(v.res[0] == token_of(out, 8));
  } else {
    return false;
  }
  return true;
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
#define RUN_INT(N, T) \
bool run_i##N(const vec_t &v) { \
  const std::string p = "i" #N "."; \
  if (v.op.compare(0, p.size(), p) != 0) { \
    return false; \
  } \
  const std::string name = v.op.substr(p.size()); \
  T a = (T)std::stoll(v.args[0]); \
  T b = v.args.size() > 1 ? (T)std::stoll(v.args[1]) : 0; \
  int n = v.args.size() > 1 ? (int)std::stoll(v.args[1]) : 0; \
  T r = 0; \
  if (name == "add") { \
    check_int(v, ty_int##N##_add(a, b, &r), r); \
  } \
  else if (name == "sub") { \
    check_int(v, ty_int##N##_sub(a, b, &r), r); \
  } \
  else if (name == "mul") { \
    check_int(v, ty_int##N##_mul(a, b, &r), r); \
  } \
  else if (name == "div") { \
    check_int(v, ty_int##N##_div(a, b, &r), r); \
  } \
  else if (name == "mod") { \
    check_int(v, ty_int##N##_mod(a, b, &r), r); \
  } \
  else if (name == "gcd") { \
    check_int(v, ty_int##N##_gcd(a, b, &r), r); \
  } \
  else if (name == "lcm") { \
    check_int(v, ty_int##N##_lcm(a, b, &r), r); \
  } \
  else if (name == "neg") { \
    check_int(v, ty_int##N##_neg(a, &r), r); \
  } \
  else if (name == "abs") { \
    check_int(v, ty_int##N##_abs(a, &r), r); \
  } \
  else if (name == "inc") { \
    check_int(v, ty_int##N##_inc(a, &r), r); \
  } \
  else if (name == "dec") { \
    check_int(v, ty_int##N##_dec(a, &r), r); \
  } \
  else if (name == "shl") { \
    check_int(v, ty_int##N##_shl(a, n, &r), r); \
  } \
  else if (name == "shr") { \
    check_int(v, ty_int##N##_shr(a, n, &r), r); \
  } \
  else if (name == "cmp") { \
    check_int(v, 0, ty_int##N##_cmp(a, b)); \
  } \
  else if (name == "eq") { \
    check_int(v, 0, ty_int##N##_eq(a, b)); \
  } \
  else if (name == "ne") { \
    check_int(v, 0, ty_int##N##_ne(a, b)); \
  } \
  else if (name == "lt") { \
    check_int(v, 0, ty_int##N##_lt(a, b)); \
  } \
  else if (name == "le") { \
    check_int(v, 0, ty_int##N##_le(a, b)); \
  } \
  else if (name == "gt") { \
    check_int(v, 0, ty_int##N##_gt(a, b)); \
  } \
  else if (name == "ge") { \
    check_int(v, 0, ty_int##N##_ge(a, b)); \
  } \
  else if (name == "larger") { \
    check_int(v, 0, ty_int##N##_larger(a, b)); \
  } \
  else if (name == "smaller") { \
    check_int(v, 0, ty_int##N##_smaller(a, b)); \
  } \
  else if (name == "and") { \
    check_int(v, 0, ty_int##N##_and(a, b)); \
  } \
  else if (name == "or") { \
    check_int(v, 0, ty_int##N##_or(a, b)); \
  } \
  else if (name == "xor") { \
    check_int(v, 0, ty_int##N##_xor(a, b)); \
  } \
  else if (name == "not") { \
    check_int(v, 0, ty_int##N##_not(a)); \
  } \
  else { \
    return false; \
  } \
  return true; \
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
  } else if (op == "uuid.parse") {
    ty_uuid_t u = {};
    std::string text = v.args[0] == "\"\"" ? "" : v.args[0];
    rc = ty_uuid_parse(text.data(), text.size(), &u);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(TY_ERR_INVALID == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(hex_of(u) == v.res[0]);
    }
  } else if (op == "uuid.format") {
    ty_uuid_t u = uuid_of(v.args[0]);
    char out[TY_UUID_TEXT_SIZE];
    ty_uuid_format(&u, out);
    INFO(v.line);
    CHECK(std::string(out) == v.res[0]);
  } else if (op == "uuid.inc" || op == "uuid.dec") {
    ty_uuid_t u = uuid_of(v.args[0]), r = {};
    int rc = op == "uuid.inc" ? ty_uuid_increment(&u, &r)
                              : ty_uuid_decrement(&u, &r);
    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(TY_ERR_RANGE == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      CHECK(hex_of(r) == v.res[0]);
    }
  } else if (op == "uuid.cmp") {
    ty_uuid_t a = uuid_of(v.args[0]), b = uuid_of(v.args[1]);
    int c = ty_uuid_cmp(&a, &b);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
  } else if (op == "uuid.version") {
    ty_uuid_t u = uuid_of(v.args[0]);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == ty_uuid_version(&u));
  } else if (op == "uuid.unixus") {
    ty_uuid_t u = uuid_of(v.args[0]);
    int64_t us = 0;
    rc = ty_uuid_unix_us(&u, &us);
    INFO(v.line);
    if (v.res[0] == "NONE") {
      CHECK(1 == rc);
    } else {
      REQUIRE(0 == rc);
      CHECK(std::stoll(v.res[0]) == us);
    }
  } else if (op == "i16.parse" || op == "i32.parse" || op == "i64.parse") {
    std::vector<uint8_t> t = bytes_of(v.args[0]);
    const char *p = reinterpret_cast<const char *>(t.data());
    int16_t r16 = 0;
    int32_t r32 = 0;
    int64_t r64 = 0;
    int rc = op == "i16.parse"   ? ty_int16_parse(p, t.size(), &r16)
             : op == "i32.parse" ? ty_int32_parse(p, t.size(), &r32)
                                 : ty_int64_parse(p, t.size(), &r64);
    check_int(v, rc, op == "i16.parse" ? r16 : op == "i32.parse" ? r32 : r64);
    /* The same text, as a string that ends at its NUL, reads the same. */
    if (std::find(t.begin(), t.end(), 0) == t.end()) {
      std::string z(p, t.size());
      int16_t z16 = 0;
      int32_t z32 = 0;
      int64_t z64 = 0;
      int zrc = op == "i16.parse"   ? ty_int16_parse_z(z.c_str(), &z16)
                : op == "i32.parse" ? ty_int32_parse_z(z.c_str(), &z32)
                                    : ty_int64_parse_z(z.c_str(), &z64);
      check_int(v, zrc,
                op == "i16.parse"   ? z16
                : op == "i32.parse" ? z32
                                    : z64);
    }
  } else if (op == "i16.fmt" || op == "i32.fmt" || op == "i64.fmt") {
    char out[TY_INT64_TEXT_SIZE];
    size_t n =
        op == "i16.fmt"   ? ty_int16_format((int16_t)std::stoll(v.args[0]), out)
        : op == "i32.fmt" ? ty_int32_format((int32_t)std::stoll(v.args[0]), out)
                          : ty_int64_format(std::stoll(v.args[0]), out);
    INFO(v.line);
    CHECK(v.res[0] == std::string(out, n));
    CHECK(strlen(out) == n);
  } else if (op == "u32.fmt" || op == "u64.fmt") {
    char out[TY_INT64_TEXT_SIZE];
    size_t n = op == "u32.fmt"
                   ? ty_uint32_format_n((uint32_t)std::stoull(v.args[0]), out)
                   : ty_uint64_format_n(std::stoull(v.args[0]), out);
    INFO(v.line);
    CHECK(v.res[0] == std::string(out, n));
  } else if (op == "int.inrange") {
    int r = 0;
    rc = ty_int_in_range(std::stoll(v.args[0]), std::stoll(v.args[1]),
                         std::stoll(v.args[2]), (int)std::stoll(v.args[3]),
                         (int)std::stoll(v.args[4]), &r);
    check_int(v, rc, r);
  } else if (op == "bool.parse") {
    bool b = false;
    rc = ty_bool_parse(v.args[0].data(), v.args[0].size(), &b);
    check_int(v, rc, b ? 1 : 0);
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
  {
    /* what PostgreSQL printed for float text, next to the vectors */
    std::string dir = path ? path : "test/vectors.txt";
    std::vector<vec_t> text = read_vectors(
        (dir.substr(0, dir.rfind('/') + 1) + "float_text.txt").c_str());
    vecs.insert(vecs.end(), text.begin(), text.end());
    std::vector<vec_t> known =
        read_vectors((dir.substr(0, dir.rfind('/') + 1) + "known.txt").c_str());
    vecs.insert(vecs.end(), known.begin(), known.end());
  }
  REQUIRE(vecs.size() > 1000);
  // 2. Run each through ty, by its operation.
  // 3. Check the number and its scale, or the integer, or the error kind.
  for (const vec_t &v : vecs) {
    bool known = run_geo(v) || run_timetz(v) || run_datetime(v) ||
                 run_interval(v) || run_money(v) || run_bpchar(v) ||
                 run_text(v) || run_inet(v) || run_bits(v) || run_known(v) ||
                 run_float_text(v) || run_float(v) || run_hash(v) ||
                 run_bytea(v) || run_int_convert(v) || run64(v) || run128(v) ||
                 run_i16(v) || run_i32(v) || run_i64(v);
    INFO(v.line);
    REQUIRE(known);
  }
}
