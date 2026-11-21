/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * bytea.cc -- the operations of bytea.
 */
#include "ty_bytea.h"

#include <limits.h>

#include <array>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hash.h"

namespace {

/* The two lowercase hexadecimal digits of each byte value, 512 characters. */
constexpr std::array<char, 512> make_hex_table() {
  std::array<char, 512> t{};
  const char digits[] = "0123456789abcdef";

  for (int i = 0; i < 256; i++) {
    t[2 * i] = digits[i >> 4];
    t[2 * i + 1] = digits[i & 15];
  }
  return t;
}

constexpr std::array<char, 512> hex_table = make_hex_table();

/* The value of a hexadecimal digit, or -1 if c is not one. */
int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* How many bytes the UTF-8 character at s has, of the n left; at least 1. */
size_t char_len(const char *s, size_t n) {
  unsigned char c = (unsigned char)s[0];
  size_t want = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;

  return want < n ? want : n;
}

/* Record the invalid hexadecimal digit at s, naming its character. */
int bad_hex_digit(const char *s, size_t n) {
  char msg[64];

  snprintf(msg, sizeof msg, "invalid hexadecimal digit: \"%.*s\"",
           (int)char_len(s, n), s);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

/* Parse the hex form: the digits after the "\x". */
int parse_hex(const char *s, size_t len, uint8_t *out, size_t *outlen) {
  size_t p = 2, n = 0;

  while (p < len) {
    if (s[p] == ' ' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r') {
      p++;
      continue;
    }
    int hi = hex_value((unsigned char)s[p]);
    if (hi < 0) {
      return bad_hex_digit(s + p, len - p);
    }
    p++;
    if (p >= len) {
      return ty_internal::set_error(
          TY_ERR_INVALID, "invalid hexadecimal data: odd number of digits");
    }
    int lo = hex_value((unsigned char)s[p]);
    if (lo < 0) {
      return bad_hex_digit(s + p, len - p);
    }
    p++;
    out[n++] = (uint8_t)(hi << 4 | lo);
  }
  *outlen = n;
  return 0;
}

/* The 8 bytes at p as one number, the first the most significant. */
uint64_t load_be(const uint8_t *p, size_t n) {
  uint64_t v = 0;

  for (size_t i = 0; i < n; i++) {
    v = (v << 8) | p[i];
  }
  return v << (8 * (8 - n));
}

/* The bytes of a, n of them at most 8, as an unsigned number. */
template <typename U> int to_unsigned(const uint8_t *a, size_t n, U *r) {
  U v = 0;

  if (n > sizeof(U)) {
    return ty_err_range();
  }
  for (size_t i = 0; i < n; i++) {
    v = (U)((v << 8) | a[i]);
  }
  *r = v;
  return 0;
}

/* The n bytes of v, the most significant first. */
template <typename U> void from_unsigned(U v, uint8_t *out) {
  for (size_t i = 0; i < sizeof(U); i++) {
    out[sizeof(U) - 1 - i] = (uint8_t)(v >> (8 * i));
  }
}

} // namespace

/* Record a negative SUBSTRING length. */
int ty_bytea_err_substr_length(void) {
  return ty_internal::set_error(TY_ERR_INVALID,
                                "negative substring length not allowed");
}

/* Record an index outside 0..limit-1, with PostgreSQL's text. */
int ty_bytea_err_index(long long index, long long limit) {
  char msg[96];

  snprintf(msg, sizeof msg, "index %lld out of valid range, 0..%lld", index,
           limit - 1);
  return ty_internal::set_error(TY_ERR_RANGE, msg);
}

/*
 * Parse the text of a bytea.
 * 1. If it begins "\x", read hexadecimal pairs, skipping space between.
 * 2. Else read the escaped form: a byte, or a backslash and three octal
 *    digits (the first 0 to 3), or two backslashes; any other backslash
 *    is invalid.
 */
int ty_bytea_parse(const char *s, size_t len, uint8_t *out, size_t *outlen) {
  size_t p = 0, n = 0;

  if (ty_bytea_is_hex_text(s, len)) {
    return parse_hex(s, len, out, outlen);
  }
  while (p < len) {
    if (s[p] != '\\') {
      out[n++] = (uint8_t)s[p++];
    } else if (p + 3 < len && s[p + 1] >= '0' && s[p + 1] <= '3' &&
               s[p + 2] >= '0' && s[p + 2] <= '7' && s[p + 3] >= '0' &&
               s[p + 3] <= '7') {
      out[n++] = (uint8_t)(((s[p + 1] - '0') << 6) | ((s[p + 2] - '0') << 3) |
                           (s[p + 3] - '0'));
      p += 4;
    } else if (p + 1 < len && s[p + 1] == '\\') {
      out[n++] = '\\';
      p += 2;
    } else {
      return ty_internal::set_error(TY_ERR_INVALID,
                                    "invalid input syntax for type bytea");
    }
  }
  *outlen = n;
  return 0;
}

/* Write "\x" and the bytes as lowercase hexadecimal digits, a pair a byte. */
void ty_bytea_format_hex(const uint8_t *data, size_t n, char *out) {
  *out++ = '\\';
  *out++ = 'x';
  for (size_t i = 0; i < n; i++) {
    memcpy(out, &hex_table[2 * (size_t)data[i]], 2);
    out += 2;
  }
}

/* The length of the escaped form of the bytes. */
size_t ty_bytea_escape_size(const uint8_t *data, size_t n) {
  size_t size = 0;

  for (size_t i = 0; i < n; i++) {
    size += data[i] == '\\' ? 2 : (data[i] < 0x20 || data[i] > 0x7e) ? 4 : 1;
  }
  return size;
}

/* Write the escaped form of the bytes. */
void ty_bytea_format_escape(const uint8_t *data, size_t n, char *out) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = data[i];

    if (c == '\\') {
      *out++ = '\\';
      *out++ = '\\';
    } else if (c < 0x20 || c > 0x7e) {
      *out++ = '\\';
      *out++ = (char)('0' + ((c >> 6) & 3));
      *out++ = (char)('0' + ((c >> 3) & 7));
      *out++ = (char)('0' + (c & 7));
    } else {
      *out++ = (char)c;
    }
  }
}

/* Compare the bytes; of two that agree as far as the shorter goes, the shorter
 * is less. */
int ty_bytea_cmp(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
  size_t n = na < nb ? na : nb;
  int c = n > 0 ? memcmp(a, b, n) : 0;

  if (c == 0 && na != nb) {
    return na < nb ? -1 : 1;
  }
  return c;
}

/*
 * The hash of bytes: each 8 bytes, the last cut short, mixed into the seed,
 * and then the length, so that bytes ending in zeros differ from shorter.
 */
uint64_t ty_bytea_hash(const uint8_t *data, size_t n, uint64_t seed) {
  uint64_t h =
      ty_internal::mix64(seed + 0x9e3779b97f4a7c15ULL, 0x6a09e667f3bcc909ULL);

  for (size_t i = 0; i < n; i += 8) {
    size_t take = n - i < 8 ? n - i : 8;
    h = ty_internal::mix64(h, load_be(data + i, take));
  }
  return ty_internal::mix64(h, (uint64_t)n);
}

/* Copy a, then b, to out. */
void ty_bytea_cat(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                  uint8_t *out) {
  if (na > 0) {
    memcpy(out, a, na);
  }
  if (nb > 0) {
    memcpy(out + na, b, nb);
  }
}

/* Copy a to out, last byte first: 8 bytes at a time, then the rest. */
void ty_bytea_reverse(const uint8_t *a, size_t n, uint8_t *out) {
  size_t i = 0;

  for (; i + 8 <= n; i += 8) {
    uint64_t w;

    memcpy(&w, a + i, 8);
    w = __builtin_bswap64(w);
    memcpy(out + n - i - 8, &w, 8);
  }
  for (; i < n; i++) {
    out[n - 1 - i] = a[i];
  }
}

namespace {

/*
 * The three parts of an overlay: the bytes of a before start, then b, then
 * the bytes of a from start + len on. pre and from_suffix say where a is cut.
 */
int overlay_cuts(size_t na, int32_t start, int32_t len, size_t *pre,
                 size_t *suffix_from) {
  int32_t sum;

  if (start <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "negative substring length not allowed");
  }
  if (__builtin_add_overflow(start, len, &sum)) {
    return ty_err_range();
  }
  size_t before = (size_t)(start - 1);
  size_t from = (size_t)((sum > 1 ? sum : 1) - 1);

  *pre = before < na ? before : na;
  *suffix_from = from < na ? from : na;
  return 0;
}

} // namespace

/* The length of the overlay. */
int ty_bytea_overlay_size(size_t na, size_t nb, int32_t start, int32_t len,
                          size_t *size) {
  size_t pre, from;

  if (overlay_cuts(na, start, len, &pre, &from) != 0) {
    return -1;
  }
  *size = pre + nb + (na - from);
  return 0;
}

/* Write the overlay: a before start, b, a from start + len. */
int ty_bytea_overlay(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                     int32_t start, int32_t len, uint8_t *out) {
  size_t pre, from;

  if (overlay_cuts(na, start, len, &pre, &from) != 0) {
    return -1;
  }
  ty_bytea_cat(a, pre, b, nb, out);
  if (na > from) {
    memcpy(out + pre + nb, a + from, na - from);
  }
  return 0;
}

/* Find b in a by trying each place; an empty b is at 1. */
int64_t ty_bytea_position(const uint8_t *a, size_t na, const uint8_t *b,
                          size_t nb) {
  if (nb == 0) {
    return 1;
  }
  for (size_t p = 0; p + nb <= na; p++) {
    if (a[p] == b[0] && memcmp(a + p, b, nb) == 0) {
      return (int64_t)p + 1;
    }
  }
  return 0;
}

/* Add up the set bits of the bytes: 8 bytes at a time, then the rest. */
int64_t ty_bytea_bit_count(const uint8_t *a, size_t n) {
  int64_t count = 0;
  size_t i = 0;

  for (; i + 8 <= n; i += 8) {
    uint64_t w;

    memcpy(&w, a + i, 8);
    count += __builtin_popcountll(w);
  }
  for (; i < n; i++) {
    count += __builtin_popcount(a[i]);
  }
  return count;
}

/* Set the byte at index to the low 8 bits of value. */
int ty_bytea_set_byte(uint8_t *a, size_t n, int32_t index, int32_t value) {
  if (index < 0 || (size_t)index >= n) {
    return ty_bytea_err_index(index, (long long)n);
  }
  a[index] = (uint8_t)value;
  return 0;
}

/* Set the bit at index; the index is checked before the bit. */
int ty_bytea_set_bit(uint8_t *a, size_t n, int64_t index, int32_t bit) {
  if (index < 0 || (uint64_t)index >= (uint64_t)n * 8) {
    return ty_bytea_err_index(index, (long long)n * 8);
  }
  if (bit != 0 && bit != 1) {
    return ty_internal::set_error(TY_ERR_INVALID, "new bit must be 0 or 1");
  }
  uint8_t mask = (uint8_t)(1 << (index % 8));
  a[index / 8] =
      bit ? (uint8_t)(a[index / 8] | mask) : (uint8_t)(a[index / 8] & ~mask);
  return 0;
}

/* The int16 of at most 2 bytes. */
int ty_bytea_to_int16(const uint8_t *a, size_t n, int16_t *r) {
  uint16_t v;

  if (to_unsigned<uint16_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int16_t)v;
  return 0;
}

/* The int32 of at most 4 bytes. */
int ty_bytea_to_int32(const uint8_t *a, size_t n, int32_t *r) {
  uint32_t v;

  if (to_unsigned<uint32_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int32_t)v;
  return 0;
}

/* The int64 of at most 8 bytes. */
int ty_bytea_to_int64(const uint8_t *a, size_t n, int64_t *r) {
  uint64_t v;

  if (to_unsigned<uint64_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int64_t)v;
  return 0;
}

/* The 2 bytes of an int16. */
void ty_int16_to_bytea(int16_t v, uint8_t out[2]) {
  from_unsigned<uint16_t>((uint16_t)v, out);
}

/* The 4 bytes of an int32. */
void ty_int32_to_bytea(int32_t v, uint8_t out[4]) {
  from_unsigned<uint32_t>((uint32_t)v, out);
}

/* The 8 bytes of an int64. */
void ty_int64_to_bytea(int64_t v, uint8_t out[8]) {
  from_unsigned<uint64_t>((uint64_t)v, out);
}
