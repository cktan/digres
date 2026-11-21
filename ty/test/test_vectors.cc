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
#include "ty_hash.h"
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
  REQUIRE(vecs.size() > 1000);
  // 2. Run each through ty, by its operation.
  // 3. Check the number and its scale, or the integer, or the error kind.
  for (const vec_t &v : vecs) {
    bool known = run_hash(v) || run_bytea(v) || run_int_convert(v) ||
                 run64(v) || run128(v) || run_i16(v) || run_i32(v) ||
                 run_i64(v);
    INFO(v.line);
    REQUIRE(known);
  }
}
