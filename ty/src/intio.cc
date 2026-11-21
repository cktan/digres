/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * intio.cc -- the text of the integers: read int16, int32 and int64 from
 * text, and write them. Written for ty, from the forms PostgreSQL reads and
 * writes (ty_int.h).
 */
#include "error.h"
#include "ty_int.h"

#include <string.h>

#include <array>

namespace {

/* The two digits of each number 0 to 99, as 200 characters. */
constexpr std::array<char, 200> make_digit_pairs() {
  std::array<char, 200> t{};

  for (int i = 0; i < 100; i++) {
    t[2 * i] = (char)('0' + i / 10);
    t[2 * i + 1] = (char)('0' + i % 10);
  }
  return t;
}

constexpr std::array<char, 200> digit_pairs = make_digit_pairs();

/* How many decimal digits v has, 1 to 20, with no loop. */
inline int decimal_length64(uint64_t v) {
  static const uint64_t pow10[] = {1ULL,
                                   10ULL,
                                   100ULL,
                                   1000ULL,
                                   10000ULL,
                                   100000ULL,
                                   1000000ULL,
                                   10000000ULL,
                                   100000000ULL,
                                   1000000000ULL,
                                   10000000000ULL,
                                   100000000000ULL,
                                   1000000000000ULL,
                                   10000000000000ULL,
                                   100000000000000ULL,
                                   1000000000000000ULL,
                                   10000000000000000ULL,
                                   100000000000000000ULL,
                                   1000000000000000000ULL,
                                   10000000000000000000ULL};
  /* 1233 / 4096 is log10(2): the bits of v give its digits to within one */
  v |= 1; /* so that zero has one digit; a power of ten is even, so no other
             changes */
  int t = ((64 - __builtin_clzll(v)) * 1233) >> 12;

  return t + 1 - (v < pow10[t]);
}

/*
 * Read an integer of type T from the text at s; U is its unsigned type. The
 * text is len bytes, or, if Z, a string that ends at its NUL (the rules of
 * ty_int.h):
 * 1. Skip the space at the start, then a sign.
 * 2. After "0x", "0o" or "0b" read hexadecimal, octal or binary digits; else
 *    decimal. A single underscore may stand between digits (and, after a
 *    prefix, before the first).
 * 3. Add each digit to an unsigned number, and stop with TY_ERR_RANGE as
 *    soon as another digit would pass what the type holds; so range is
 *    reported before a fault further on.
 * 4. Require a digit, then allow space, and nothing else, at the end.
 * 5. Make the number negative, or check it against the largest value.
 * Most text is an optional '-' and digits to the end, which a first loop
 * reads with no more tests than it needs; anything else restarts at 1.
 */
template <typename T, typename U, bool Z>
int parse_int(const char *s, size_t len, T *r) {
  const U min_mag = (U)1 << (sizeof(T) * 8 - 1); /* the magnitude of MIN */
  const char *end = Z ? nullptr : s + len;

  /* The byte at p, or NUL at the end: the text need not end in NUL. */
  auto at = [&](const char *p) -> unsigned char {
    return (Z || p < end) ? (unsigned char)*p : 0;
  };
  auto at_end = [&](const char *p) { return Z ? *p == '\0' : p == end; };
  auto space = [](unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
  };
  auto hexval = [](unsigned char c) -> int {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
  };

  /* The fast path: an optional '-' and decimal digits to the end. */
  {
    const char *p = s;
    bool neg = false;
    U v = 0;

    if (at(p) == '-') {
      p++;
      neg = true;
    }
    unsigned d = (unsigned)at(p) - '0';
    if (d < 10) {
      do {
        if (v > min_mag / 10) {
          return ty_err_range();
        }
        v = (U)(v * 10 + d);
        p++;
        d = (unsigned)at(p) - '0';
      } while (d < 10);
      if (at_end(p)) {
        if (neg ? v > min_mag : v > (U)(min_mag - 1)) {
          return ty_err_range();
        }
        *r = neg ? (T)(U)(0 - v) : (T)v;
        return 0;
      }
    }
  }

  /* The slow path. */
  const char *p = s;
  bool neg = false;
  U tmp = 0;

  while (space(at(p))) {
    p++;
  }
  if (at(p) == '-') {
    neg = true;
    p++;
  } else if (at(p) == '+') {
    p++;
  }

  unsigned base = 10;
  if (at(p) == '0' && (at(p + 1) == 'x' || at(p + 1) == 'X')) {
    base = 16;
    p += 2;
  } else if (at(p) == '0' && (at(p + 1) == 'o' || at(p + 1) == 'O')) {
    base = 8;
    p += 2;
  } else if (at(p) == '0' && (at(p + 1) == 'b' || at(p + 1) == 'B')) {
    base = 2;
    p += 2;
  }
  const char *first = p;
  for (;;) {
    int d = hexval(at(p));
    if (d >= 0 && (unsigned)d < base) {
      if (tmp > min_mag / base) {
        return ty_err_range();
      }
      tmp = (U)(tmp * base + (unsigned)d);
      p++;
    } else if (at(p) == '_') {
      /* in base 10 it may not be first; and a digit must follow */
      if (base == 10 && p == first) {
        return ty_err_invalid("invalid integer text");
      }
      p++;
      int nd = hexval(at(p));
      if (nd < 0 || (unsigned)nd >= base) {
        return ty_err_invalid("invalid integer text");
      }
    } else {
      break;
    }
  }
  if (p == first) {
    return ty_err_invalid("invalid integer text");
  }
  while (space(at(p))) {
    p++;
  }
  if (!at_end(p)) {
    return ty_err_invalid("invalid integer text");
  }
  if (neg ? tmp > min_mag : tmp > (U)(min_mag - 1)) {
    return ty_err_range();
  }
  *r = neg ? (T)(U)(0 - tmp) : (T)tmp;
  return 0;
}

/* The digits of v at out, with no NUL; the number of them. */
inline size_t format_u64(uint64_t v, char *out) {
  int len = decimal_length64(v);
  char *p = out + len;

  while (v >= 100) {
    uint64_t q = v / 100;
    unsigned r = (unsigned)(v - q * 100);

    p -= 2;
    memcpy(p, &digit_pairs[2 * r], 2);
    v = q;
  }
  if (v >= 10) {
    memcpy(p - 2, &digit_pairs[2 * v], 2);
  } else {
    p[-1] = (char)('0' + v);
  }
  return (size_t)len;
}

/* The same for 32 bits, whose divides are cheaper. */
inline size_t format_u32(uint32_t v, char *out) {
  int len = decimal_length64(v);
  char *p = out + len;

  while (v >= 100) {
    uint32_t q = v / 100;
    unsigned r = v - q * 100;

    p -= 2;
    memcpy(p, &digit_pairs[2 * r], 2);
    v = q;
  }
  if (v >= 10) {
    memcpy(p - 2, &digit_pairs[2 * v], 2);
  } else {
    p[-1] = (char)('0' + v);
  }
  return (size_t)len;
}

} // namespace

/* Read an int16. */
int ty_int16_parse(const char *s, size_t len, int16_t *r) {
  return parse_int<int16_t, uint16_t, false>(s, len, r);
}

/* Read an int32. */
int ty_int32_parse(const char *s, size_t len, int32_t *r) {
  return parse_int<int32_t, uint32_t, false>(s, len, r);
}

/* Read an int64. */
int ty_int64_parse(const char *s, size_t len, int64_t *r) {
  return parse_int<int64_t, uint64_t, false>(s, len, r);
}

/* Read an int16 from a string that ends at its NUL. */
int ty_int16_parse_z(const char *s, int16_t *r) {
  return parse_int<int16_t, uint16_t, true>(s, 0, r);
}

/* Read an int32 from a string that ends at its NUL. */
int ty_int32_parse_z(const char *s, int32_t *r) {
  return parse_int<int32_t, uint32_t, true>(s, 0, r);
}

/* Read an int64 from a string that ends at its NUL. */
int ty_int64_parse_z(const char *s, int64_t *r) {
  return parse_int<int64_t, uint64_t, true>(s, 0, r);
}

/* The digits of an unsigned 32-bit value, with no NUL. */
size_t ty_uint32_format_n(uint32_t v, char *out) { return format_u32(v, out); }

/* The digits of an unsigned 64-bit value, with no NUL. */
size_t ty_uint64_format_n(uint64_t v, char *out) { return format_u64(v, out); }

/* Write an int16 and a NUL. */
size_t ty_int16_format(int16_t v, char *out) { return ty_int32_format(v, out); }

/* Write an int32 and a NUL: a minus sign, then the digits of its magnitude. */
size_t ty_int32_format(int32_t v, char *out) {
  uint32_t u = (uint32_t)v;
  size_t len = 0;

  if (v < 0) {
    u = 0 - u;
    out[len++] = '-';
  }
  len += format_u32(u, out + len);
  out[len] = '\0';
  return len;
}

/* Write an int64 and a NUL. */
size_t ty_int64_format(int64_t v, char *out) {
  uint64_t u = (uint64_t)v;
  size_t len = 0;

  if (v < 0) {
    u = 0 - u;
    out[len++] = '-';
  }
  len += format_u64(u, out + len);
  out[len] = '\0';
  return len;
}
