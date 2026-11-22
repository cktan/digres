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
#include "ty_bool.h"
#include "ty_bytea.h"
#include "ty_decimal.h"
#include "ty_float.h"
#include "ty_hash.h"
#include "ty_datetime.h"
#include "ty_interval.h"
#include "text_raw.h"
#include "ty_text.h"
#include "vl.h"
#include "ty_int.h"
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
    got = op == "hash.bytes"
              ? ty_hash_bytes(k.data(), k.size(), seed)
              : ty_bytea_hash(
                    vl_t(std::string(reinterpret_cast<const char *>(k.data()),
                                     k.size())),
                    seed);
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
    CHECK(std::stoll(v.res[0]) == (int64_t)ty_bpchar_truelen(vl_t(a)));
  } else if (op == "bpchar.cmp") {
    std::vector<uint8_t> bb = bytes_of(v.args[1]);
    std::string b(reinterpret_cast<const char *>(bb.data()), bb.size());
    int c = ty_bpchar_cmp(vl_t(a), vl_t(b, true));

    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
    CHECK(ty_bpchar_eq(vl_t(a), vl_t(b)) == (c == 0));
  } else if (op == "text.cliplen") {
    CHECK(
        std::stoll(v.res[0]) ==
        (int64_t)tyr_text_cliplen(a.data(), a.size(), std::stoull(v.args[1])));
  } else if (op == "text.clipbytes") {
    CHECK(std::stoll(v.res[0]) ==
          (int64_t)tyr_text_clipbytes(a.data(), a.size(),
                                      std::stoull(v.args[1])));
  } else if (op == "bpchar.fit") {
    vl_t A(a);
    vl_out_t o(A);
    int rc = ty_bpchar_fit(A, std::stoull(v.args[1]), std::stoi(v.args[2]),
                           std::stoi(v.args[3]), &o);

    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      REQUIRE(0 == rc);
      /* known as the bytes kept, and the bytes of the result */
      std::string r = o.str();
      CHECK(std::stoll(v.res[1]) == (int64_t)r.size());
      CHECK(std::stoll(v.res[0]) <= (int64_t)r.size());
      CHECK(0 == memcmp(r.data(), a.data(), (size_t)std::stoll(v.res[0])));
    }
  } else {
    char out[TY_NAMEDATALEN];

    ty_name_parse(a.data(), a.size(), out);
    CHECK(v.res[0] ==
          token_of(reinterpret_cast<const uint8_t *>(out), strlen(out)));
  }
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
  int rc = 0;
  /* the two forms of a header are used in turn */
  static unsigned form = 0;
  bool lg = (form++ & 1) != 0;

  if (v.args.size() > 1 && name != "left" && name != "right" &&
      name != "repeat" && name != "chr" && name != "length" &&
      name != "substr" && name != "like" && name != "like_escape" &&
      name != "lower" && name != "upper" && name != "initcap") {
    b = str(1);
  }
  /* a result that is a varlena, or an error; frees the result */
  auto vcheck = [&](int r, vl_out_t &o) {
    if (r == 0) {
      std::string got = o.str();

      check(r, tok(got.data(), got.size()));
    } else {
      check(r, "");
    }
  };
  if (name == "chr") {
    vl_out_t o;

    rc = ty_text_chr((int32_t)num(0), &o);
    vcheck(rc, o);
    return true;
  }
  if (v.args.size() > 2 && name != "substr" && name != "overlay" &&
      name != "split_part" && name != "lpad" && name != "rpad" &&
      name != "trim") {
    c = str(2);
  }
  INFO(v.line);
  vl_t A(a, lg), B(b, lg), C(c, lg);
  vl_out_t o(A);

  if (name == "length") {
    CHECK(std::stoll(v.res[0]) == (int64_t)ty_text_length(A));
  } else if (name == "cmp") {
    int r = ty_text_cmp(A, B);
    CHECK(std::stoi(v.res[0]) == (r > 0) - (r < 0));
    CHECK(ty_text_eq(A, B) == (r == 0));
  } else if (name == "starts_with") {
    CHECK(std::stoi(v.res[0]) == ty_text_starts_with(A, B));
  } else if (name == "position") {
    CHECK(std::stoi(v.res[0]) == ty_text_position(A, B));
  } else if (name == "reverse") {
    rc = ty_text_reverse(A, &o);
    vcheck(rc, o);
  } else if (name == "replace") {
    rc = ty_text_replace(A, B, C, &o);
    vcheck(rc, o);
  } else if (name == "left" || name == "right") {
    rc = name == "left" ? ty_text_left(A, (int32_t)num(1), &o)
                        : ty_text_right(A, (int32_t)num(1), &o);
    vcheck(rc, o);
  } else if (name == "substr") {
    rc = ty_text_substr(A, (int32_t)num(1), (int32_t)num(2), (int)num(3), &o);
    vcheck(rc, o);
  } else if (name == "overlay") {
    vl_t R(str(1), lg);

    rc = ty_text_overlay(A, R, (int32_t)num(2), (int32_t)num(3), &o);
    vcheck(rc, o);
  } else if (name == "split_part") {
    vl_t sep(str(1), lg);

    rc = ty_text_split_part(A, sep, (int32_t)num(2), &o);
    vcheck(rc, o);
  } else if (name == "repeat") {
    rc = ty_text_repeat(A, (int32_t)num(1), &o);
    vcheck(rc, o);
  } else if (name == "lpad" || name == "rpad") {
    vl_t fill(str(2), lg);

    rc = name == "lpad" ? ty_text_lpad(A, (int32_t)num(1), fill, &o)
                        : ty_text_rpad(A, (int32_t)num(1), fill, &o);
    vcheck(rc, o);
  } else if (name == "trim") {
    vl_t set(str(1), lg);
    std::string mode = v.args[2];

    rc = ty_text_trim(A, set, mode != "r", mode != "l", &o);
    vcheck(rc, o);
  } else if (name == "translate") {
    rc = ty_text_translate(A, B, C, &o);
    vcheck(rc, o);
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
    vl_t pat(str(1), lg);

    rc = ty_text_like(A, pat, 1);
    CHECK(std::stoi(v.res[0]) == rc);
  } else if (name == "like_escape") {
    vl_t esc(str(1), lg);

    rc = ty_text_like_escape(A, esc, 1, &o);
    vcheck(rc, o);
  } else if (name == "ascii") {
    int32_t r = 0;

    rc = ty_text_ascii(A, &r);
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

/* A host's zone hooks, as the tests' server has them (its zone is UTC). */
struct fake_zone_t {
  const char *name;
  int fixed;
  long gmtoff; /* seconds east of Greenwich, as pg_get_timezone_offset */
};

const fake_zone_t kFakeZones[] = {
    {"america/new_york", 0, 0},
    {"europe/london", 0, 0},
    {"utc", 1, 0},
    {"gmt", 1, 0},
    {"etc/gmt-5", 1, 18000},
    /* what pg_tzset takes of a text that is almost a POSIX spec */
    {"bt04:05:06.5", 1, -(4 * 3600 + 5 * 60 + 6)},
    {"tt04:057ttues:day", 0, 0},
};

/*
 * A POSIX zone spec as pg_tzset takes it: a name of letters, an offset
 * ([+-]hours[:min[:sec]], hours up to 167), and maybe the name of a daylight
 * time. Returns 0 if the text is not one, 1 for one with a fixed offset, 2
 * for one with a daylight time.
 */
int posix_zone(const char *p) {
  int n = 0, v;

  while (isalpha((unsigned char)*p)) {
    p++, n++;
  }
  if (n < 1) {
    return 0;
  }
  if (*p == '+' || *p == '-') {
    p++;
  }
  static const int limit[] = {167, 59, 60};

  for (int part = 0; part < 3; part++) {
    if (!isdigit((unsigned char)*p)) {
      return 0;
    }
    for (v = 0; isdigit((unsigned char)*p); p++) {
      v = v * 10 + (*p - '0');
      if (v > 1000) {
        return 0;
      }
    }
    if (v > limit[part]) {
      return 0;
    }
    if (*p != ':') {
      break;
    }
    p++;
  }
  if (*p == '\0') {
    return 1;
  }
  for (; isalpha((unsigned char)*p); p++) {
  }
  return *p == '\0' ? 2 : 0;
}

/* Is the lower-case word a zone: a name of the list, or a POSIX spec. */
const void *fake_zone(void *, const char *name) {
  static thread_local fake_zone_t ring[8];
  static thread_local int next = 0;

  for (const auto &z : kFakeZones) {
    if (strcmp(z.name, name) == 0) {
      return &z;
    }
  }
  int k = posix_zone(name);

  if (k == 0) {
    return nullptr;
  }
  // A POSIX offset is west of Greenwich: "gmt+8" is 8 hours west.
  const char *p = name;
  while (isalpha((unsigned char)*p)) {
    p++;
  }
  int sign = 1;
  if (*p == '+' || *p == '-') {
    sign = *p == '-' ? -1 : 1;
    p++;
  }
  long v[3] = {0, 0, 0};
  for (int part = 0; part < 3 && isdigit((unsigned char)*p); part++) {
    for (; isdigit((unsigned char)*p); p++) {
      v[part] = v[part] * 10 + (*p - '0');
    }
    if (*p == ':') {
      p++;
    }
  }
  fake_zone_t &z = ring[next++ % 8];

  z = {"posix", k == 1, -sign * (v[0] * 3600 + v[1] * 60 + v[2])};
  return &z;
}

int fake_zone_fixed(void *, const void *tz, long *gmtoff) {
  const fake_zone_t *z = static_cast<const fake_zone_t *>(tz);

  *gmtoff = z->gmtoff;
  return z->fixed;
}

/* The abbreviations of the server's default list that the vectors use. */
int fake_abbrev(void *, int, const char *w, int *type, int *offset,
                const void **tz, ty_dtin_extra_t *) {
  static const struct {
    const char *w;
    int type, offset;
  } t[] = {{"pst", TY_ABBREV_TZ, -28800},  {"est", TY_ABBREV_TZ, -18000},
           {"edt", TY_ABBREV_DTZ, -14400}, {"pdt", TY_ABBREV_DTZ, -25200},
           {"utc", TY_ABBREV_TZ, 0},       {"gmt", TY_ABBREV_TZ, 0},
           {"z", TY_ABBREV_TZ, 0}};

  *type = TY_ABBREV_NONE;
  *offset = 0;
  *tz = nullptr;
  if (strcmp(w, "zulu") == 0) {
    *type = TY_ABBREV_TZ;
    return 0;
  }
  for (const auto &e : t) {
    if (strcmp(e.w, w) == 0) {
      *type = e.type;
      *offset = e.offset;
      break;
    }
  }
  return 0;
}

void fake_now(void *, ty_tm_t *tm, int32_t *fsec, int *tz) {
  *tm = {2030, 6, 15, 12, 34, 56};
  *fsec = 789000;
  *tz = 0;
}

/* The offset of a zone, west of Greenwich; the session's if tz is NULL. */
int fake_offset(void *ctx, const ty_tm_t *, const void *tz) {
  if (tz == nullptr) {
    return *static_cast<int *>(ctx);
  }
  return (int)-static_cast<const fake_zone_t *>(tz)->gmtoff;
}

/* No zone of the vectors has a dynamic abbreviation. */
int fake_abbrev_offset(void *, const ty_tm_t *, const char *, const void *) {
  return 0;
}

const ty_dtzone_t kFakeHost = {nullptr,         fake_abbrev, fake_zone,
                               fake_zone_fixed, fake_now,    nullptr,
                               nullptr};

/* The same with the offsets, for the types that have a zone; ctx is the
 * offset of the session's zone, west of Greenwich. */
ty_dtzone_t fake_host_tz(int *session_west) {
  return {session_west, fake_abbrev, fake_zone,         fake_zone_fixed,
          fake_now,     fake_offset, fake_abbrev_offset};
}

/* Run a text in of date, time or timestamp (known.txt): the text is hex. */
bool run_dtin_known(const vec_t &v) {
  std::vector<uint8_t> tb = bytes_of(v.args[2]);
  std::string z(reinterpret_cast<const char *>(tb.data()), tb.size());
  const std::string &kind = v.args[0];
  int order = std::stoi(v.args[1]), dterr = 0, rc;
  ty_dtin_extra_t extra;
  int32_t d = 0;
  int64_t t = 0;

  INFO(v.line);
  if (z.find('\0') != std::string::npos) {
    return true;
  }
  if (kind == "date") {
    rc = ty_date_parse(z.c_str(), order, &kFakeHost, &d, &dterr, &extra);
  } else if (kind == "time") {
    rc = ty_time_parse(z.c_str(), order, &kFakeHost, &t, &dterr, &extra);
  } else {
    rc = ty_timestamp_parse(z.c_str(), order, &kFakeHost, &t, &dterr, &extra);
  }
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    if (v.res[1] == "R") {
      CHECK(0 == dterr);
      CHECK(TY_ERR_RANGE == ty_errnum());
    } else {
      CHECK(std::stoi(v.res[1]) == dterr);
      CHECK(TY_ERR_INVALID == ty_errnum());
    }
    return true;
  }
  CHECK(0 == rc);
  if (rc != 0) {
    return true;
  }
  std::string text;
  char out[TY_DATETIME_TEXT_MAX];
  ty_tm_t tm = {};
  int32_t fsec = 0;

  if (kind == "date") {
    if (d == TY_DATE_NOEND) {
      text = "infinity";
    } else if (d == TY_DATE_NOBEGIN) {
      text = "-infinity";
    } else {
      ty_j2date(d + TY_POSTGRES_EPOCH_JDATE, &tm.year, &tm.mon, &tm.mday);
      ty_date_format(&tm, TY_DATES_ISO, TY_DATEORDER_YMD, out);
      text = out;
    }
  } else if (kind == "time") {
    tm.hour = (int32_t)(t / TY_USECS_PER_HOUR);
    tm.min = (int32_t)(t / TY_USECS_PER_MINUTE % 60);
    tm.sec = (int32_t)(t / TY_USECS_PER_SEC % 60);
    fsec = (int32_t)(t % TY_USECS_PER_SEC);
    ty_time_format(&tm, fsec, 0, 0, TY_DATES_ISO, out);
    text = out;
  } else {
    text = ts_text(t);
  }
  CHECK(v.res[0] ==
        token_of(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  return true;
}

/*
 * Run a text in of timestamptz or timetz (known.txt): kind, date order, the
 * session's zone (0 UTC, 1 Etc/GMT-5), and the text in hex.
 */
bool run_dtzin_known(const vec_t &v) {
  std::vector<uint8_t> tb = bytes_of(v.args[3]);
  std::string z(reinterpret_cast<const char *>(tb.data()), tb.size());
  const std::string &kind = v.args[0];
  int order = std::stoi(v.args[1]), dterr = 0, rc;
  int session_west = std::stoi(v.args[2]) == 0 ? 0 : -18000;
  ty_dtzone_t host = fake_host_tz(&session_west);
  ty_dtin_extra_t extra;
  int64_t t = 0;
  ty_timetz_t tz = {};

  INFO(v.line);
  // The zone of this text has a daylight time: what pg_tzset knows of its
  // rules is the server's.
  std::string low = z;

  for (char &c : low) {
    c = (char)tolower((unsigned char)c);
  }
  if (z.find('\0') != std::string::npos ||
      low.find("ttues:day") != std::string::npos) {
    return true;
  }
  if (kind == "timestamptz") {
    rc = ty_timestamptz_parse(z.c_str(), order, &host, &t, &dterr, &extra);
  } else {
    rc = ty_timetz_parse(z.c_str(), order, &host, &tz, &dterr, &extra);
  }
  if (v.res[0] == "E") {
    CHECK(-1 == rc);
    if (v.res[1] == "R") {
      CHECK(0 == dterr);
      CHECK(TY_ERR_RANGE == ty_errnum());
    } else {
      CHECK(std::stoi(v.res[1]) == dterr);
      CHECK(TY_ERR_INVALID == ty_errnum());
    }
    return true;
  }
  CHECK(0 == rc);
  if (rc != 0) {
    return true;
  }
  std::string text;

  if (kind == "timestamptz") {
    text = ts_text(t);
  } else {
    char out[TY_DATETIME_TEXT_MAX];
    ty_tm_t tm = {};

    tm.hour = (int32_t)(tz.time / TY_USECS_PER_HOUR);
    tm.min = (int32_t)(tz.time / TY_USECS_PER_MINUTE % 60);
    tm.sec = (int32_t)(tz.time / TY_USECS_PER_SEC % 60);
    ty_time_format(&tm, (int32_t)(tz.time % TY_USECS_PER_SEC), 1, tz.zone,
                   TY_DATES_ISO, out);
    text = out;
  }
  CHECK(v.res[0] ==
        token_of(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  return true;
}

/* Run a vector of what PostgreSQL did with a text (known.txt): the text is hex.
 */
bool run_known(const vec_t &v) {
  const std::string &op = v.op;

  if (op == "ts.trunc" || op == "ts.age" || op == "ts.bin" ||
      op == "iv.trunc") {
    return run_ts_known(v);
  }
  if (op.compare(0, 5, "part.") == 0) {
    return run_part_known(v);
  }
  if (op == "dt.in") {
    return run_dtin_known(v);
  }
  if (op == "dtz.in") {
    return run_dtzin_known(v);
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
    int rc = ty_text_like(
        vl_t(std::string(reinterpret_cast<const char *>(tb.data()), tb.size())),
        vl_t(std::string(reinterpret_cast<const char *>(pb.data()), pb.size())),
        1);

    INFO(v.line);
    if (v.res[0] == "E") {
      CHECK(-1 == rc);
      CHECK(errnum_of(v.res[1]) == ty_errnum());
    } else {
      CHECK(std::stoi(v.res[0]) == rc);
    }
    return true;
  }
  return false;
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

/* A string of the bytes of a vector. */
static std::string str_of(const std::vector<uint8_t> &b) {
  return std::string(reinterpret_cast<const char *>(b.data()), b.size());
}

/* The bytes of a varlena that ty made. */
static std::vector<uint8_t> bytes_of_vl(const ty_varlena_t *p) {
  const uint8_t *d = reinterpret_cast<const uint8_t *>(ty_vl_data(p));

  return std::vector<uint8_t>(d, d + ty_vl_len(p));
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
  /* the two forms of a header are used in turn */
  static unsigned form = 0;
  bool lg = (form++ & 1) != 0;
  /* a result that is a varlena, or an error */
  auto vcheck = [&](int rc, vl_out_t &o) {
    check_bytes(v, rc, rc == 0 ? bytes_of_vl(o.p) : std::vector<uint8_t>());
  };

  if (name == "parse") {
    std::string t = text_of(v.args[0]);
    vl_out_t o;
    int rc = ty_bytea_parse(t.data(), t.size(), &o);
    vcheck(rc, o);
  } else if (name == "fmthex") {
    vl_t a(str_of(arg(0)), lg);
    std::string out(ty_bytea_hex_size(a), '\0');
    ty_bytea_format_hex(a, &out[0]);
    INFO(v.line);
    CHECK(v.res[0] == out);
  } else if (name == "fmtesc") {
    vl_t a(str_of(arg(0)), lg);
    std::string out(ty_bytea_escape_size(a), '\0');
    ty_bytea_format_escape(a, &out[0]);
    INFO(v.line);
    CHECK(text_of(v.res[0]) == out);
  } else if (name == "cmp") {
    vl_t a(str_of(arg(0)), lg), b(str_of(arg(1)), !lg);
    int c = ty_bytea_cmp(a, b);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == (c > 0) - (c < 0));
  } else if (name == "eq" || name == "ne" || name == "lt" || name == "le" ||
             name == "gt" || name == "ge") {
    vl_t a(str_of(arg(0)), lg), b(str_of(arg(1)), !lg);
    int got = name == "eq"   ? ty_bytea_eq(a, b)
              : name == "ne" ? ty_bytea_ne(a, b)
              : name == "lt" ? ty_bytea_lt(a, b)
              : name == "le" ? ty_bytea_le(a, b)
              : name == "gt" ? ty_bytea_gt(a, b)
                             : ty_bytea_ge(a, b);
    INFO(v.line);
    CHECK(std::stoi(v.res[0]) == got);
  } else if (name == "enc" || name == "dec") {
    ty_bytea_enc_t enc;
    REQUIRE(0 == ty_bytea_encoding(v.args[0].c_str(), &enc));
    vl_t in(str_of(bytes_of(v.args[1])), lg);
    vl_out_t o;
    int rc = name == "enc" ? ty_bytea_encode(enc, in, &o)
                           : ty_bytea_decode(enc, in, &o);
    vcheck(rc, o);
  } else if (name == "cat") {
    vl_t a(str_of(arg(0)), lg), b(str_of(arg(1)), !lg);
    vl_out_t o;
    int rc = ty_bytea_cat(a, b, &o);
    vcheck(rc, o);
  } else if (name == "reverse") {
    vl_t a(str_of(arg(0)), lg);
    vl_out_t o;
    int rc = ty_bytea_reverse(a, &o);
    vcheck(rc, o);
  } else if (name == "position") {
    vl_t a(str_of(arg(0)), lg), b(str_of(arg(1)), !lg);
    INFO(v.line);
    CHECK(std::stoll(v.res[0]) == ty_bytea_position(a, b));
  } else if (name == "bitcount") {
    vl_t a(str_of(arg(0)), lg);
    INFO(v.line);
    CHECK(std::stoll(v.res[0]) == ty_bytea_bit_count(a));
  } else if (name == "getbyte" || name == "getbit") {
    vl_t a(str_of(arg(0)), lg);
    int32_t r = 0;
    int rc = name == "getbyte" ? ty_bytea_get_byte(a, (int32_t)num(1), &r)
                               : ty_bytea_get_bit(a, num(1), &r);
    check_int(v, rc, r);
  } else if (name == "setbyte" || name == "setbit") {
    vl_t a(str_of(arg(0)), lg);
    vl_out_t o;
    int rc = name == "setbyte"
                 ? ty_bytea_set_byte(a, (int32_t)num(1), (int32_t)num(2), &o)
                 : ty_bytea_set_bit(a, num(1), (int32_t)num(2), &o);
    vcheck(rc, o);
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
    vl_t a(str_of(arg(0)), lg), b(str_of(arg(1)), !lg);
    vl_out_t o;
    int rc = ty_bytea_overlay(a, b, (int32_t)num(2), (int32_t)num(3), &o);
    vcheck(rc, o);
  } else if (name == "toint16" || name == "toint32" || name == "toint64") {
    vl_t a(str_of(arg(0)), lg);
    int16_t r16 = 0;
    int32_t r32 = 0;
    int64_t r64 = 0;
    int rc = name == "toint16"   ? ty_bytea_to_int16(a, &r16)
             : name == "toint32" ? ty_bytea_to_int32(a, &r32)
                                 : ty_bytea_to_int64(a, &r64);
    check_int(v, rc, name == "toint16" ? r16 : name == "toint32" ? r32 : r64);
  } else if (name == "fromint16" || name == "fromint32" ||
             name == "fromint64") {
    vl_out_t o;
    int rc = name == "fromint16"   ? ty_int16_to_bytea((int16_t)num(0), &o)
             : name == "fromint32" ? ty_int32_to_bytea((int32_t)num(0), &o)
                                   : ty_int64_to_bytea(num(0), &o);
    REQUIRE(0 == rc);
    std::vector<uint8_t> out = bytes_of_vl(o.p);
    INFO(v.line);
    CHECK(v.res[0] == token_of(out.data(), out.size()));
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
    bool known = run_timetz(v) || run_datetime(v) || run_interval(v) ||
                 run_bpchar(v) || run_text(v) || run_known(v) ||
                 run_float_text(v) || run_float(v) || run_hash(v) ||
                 run_bytea(v) || run_int_convert(v) || run64(v) || run128(v) ||
                 run_i16(v) || run_i32(v) || run_i64(v);
    INFO(v.line);
    REQUIRE(known);
  }
}
