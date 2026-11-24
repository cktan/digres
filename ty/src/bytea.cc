/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * bytea.cc -- the operations of bytea.
 */
#include "bytea_raw.h"
#include "ty_bytea.h"

#include <limits.h>

#include <array>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "ty_hash.h"

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

/* The value of each byte as a hexadecimal digit, or -1 if it is not one. */
constexpr std::array<int8_t, 256> make_hex_values() {
  std::array<int8_t, 256> t{};

  for (int i = 0; i < 256; i++) {
    t[i] = (i >= '0' && i <= '9')   ? (int8_t)(i - '0')
           : (i >= 'a' && i <= 'f') ? (int8_t)(i - 'a' + 10)
           : (i >= 'A' && i <= 'F') ? (int8_t)(i - 'A' + 10)
                                    : (int8_t)-1;
  }
  return t;
}

constexpr std::array<int8_t, 256> hex_values = make_hex_values();

/* The value of a hexadecimal digit, or -1 if c is not one. */
inline int hex_value(unsigned char c) { return hex_values[c]; }

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

/* Parse hex pairs from start on, skipping the space between them. */
int parse_hex(const char *s, size_t len, size_t start, uint8_t *out,
              size_t *outlen) {
  size_t p = start, n = 0;

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

const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The value of a base64 digit, or -1 if c is not one. */
int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* Record the invalid base64 symbol at s, naming its character. */
int bad_base64_symbol(const char *s, size_t n) {
  char msg[96];

  snprintf(msg, sizeof msg,
           "invalid symbol \"%.*s\" found while decoding base64 sequence",
           (int)char_len(s, n), s);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

/* Write base64 text; a line feed follows each 76 characters of whole groups. */
size_t encode_base64(const uint8_t *data, size_t n, char *out) {
  char *p = out, *line_end = out + 76;
  uint32_t buf = 0;
  int pos = 2;

  for (size_t i = 0; i < n; i++) {
    buf |= (uint32_t)data[i] << (pos << 3);
    pos--;
    if (pos < 0) {
      *p++ = base64_chars[(buf >> 18) & 0x3f];
      *p++ = base64_chars[(buf >> 12) & 0x3f];
      *p++ = base64_chars[(buf >> 6) & 0x3f];
      *p++ = base64_chars[buf & 0x3f];
      pos = 2;
      buf = 0;
    }
    if (p >= line_end) {
      *p++ = '\n';
      line_end = p + 76;
    }
  }
  if (pos != 2) {
    *p++ = base64_chars[(buf >> 18) & 0x3f];
    *p++ = base64_chars[(buf >> 12) & 0x3f];
    *p++ = (pos == 0) ? base64_chars[(buf >> 6) & 0x3f] : '=';
    *p++ = '=';
  }
  return (size_t)(p - out);
}

/*
 * Read base64 text.
 * 1. Skip a space, tab, newline or return.
 * 2. An "=" ends the data, but only after 2 or 3 digits of a group of 4.
 * 3. Any other character must be a digit; four make three bytes, fewer if
 *    "=" came.
 * 4. A group left unfinished at the end is an error.
 */
int decode_base64(const char *s, size_t n, uint8_t *out, size_t *outlen) {
  uint8_t *p = out;
  uint32_t buf = 0;
  int pos = 0, end = 0;

  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    int b;

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }
    if (c == '=') {
      if (!end) {
        if (pos == 2) {
          end = 1;
        } else if (pos == 3) {
          end = 2;
        } else {
          return ty_internal::set_error(
              TY_ERR_INVALID,
              "unexpected \"=\" while decoding base64 sequence");
        }
      }
      b = 0;
    } else {
      b = (c > 0 && c < 127) ? base64_value(c) : -1;
      if (b < 0) {
        return bad_base64_symbol(s + i, n - i);
      }
    }
    buf = (buf << 6) + (uint32_t)b;
    pos++;
    if (pos == 4) {
      *p++ = (uint8_t)((buf >> 16) & 255);
      if (end == 0 || end > 1) {
        *p++ = (uint8_t)((buf >> 8) & 255);
      }
      if (end == 0 || end > 2) {
        *p++ = (uint8_t)(buf & 255);
      }
      buf = 0;
      pos = 0;
    }
  }
  if (pos != 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "invalid base64 end sequence");
  }
  *outlen = (size_t)(p - out);
  return 0;
}

/* True if the 3 bytes after a backslash at s[i] are an octal, 000 to 377. */
bool is_octal_escape(const char *s, size_t i, size_t n) {
  return i + 3 < n && s[i + 1] >= '0' && s[i + 1] <= '3' && s[i + 2] >= '0' &&
         s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7';
}

/* Read the escaped form into out (n bytes of room); the length it gives. */
int decode_escape(const char *s, size_t n, uint8_t *out, size_t *outlen) {
  size_t i = 0, len = 0;

  while (i < n) {
    if (s[i] != '\\') {
      out[len++] = (uint8_t)s[i++];
    } else if (is_octal_escape(s, i, n)) {
      out[len++] = (uint8_t)(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) |
                             (s[i + 3] - '0'));
      i += 4;
    } else if (i + 1 < n && s[i + 1] == '\\') {
      out[len++] = '\\';
      i += 2;
    } else {
      return ty_internal::set_error(TY_ERR_INVALID,
                                    "invalid input syntax for type bytea");
    }
  }
  *outlen = len;
  return 0;
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
int tyr_bytea_parse(const char *s, size_t len, uint8_t *out, size_t *outlen) {
  size_t p = 0, n = 0;

  if (tyr_bytea_is_hex_text(s, len)) {
    return parse_hex(s, len, 2, out, outlen);
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
void tyr_bytea_format_hex(const uint8_t *data, size_t n, char *out) {
  *out++ = '\\';
  *out++ = 'x';
  for (size_t i = 0; i < n; i++) {
    memcpy(out, &hex_table[2 * (size_t)data[i]], 2);
    out += 2;
  }
}

/* The length of the escaped form of the bytes. */
size_t tyr_bytea_escape_size(const uint8_t *data, size_t n) {
  size_t size = 0;

  for (size_t i = 0; i < n; i++) {
    size += data[i] == '\\' ? 2 : (data[i] < 0x20 || data[i] > 0x7e) ? 4 : 1;
  }
  return size;
}

/* Write the escaped form of the bytes. */
void tyr_bytea_format_escape(const uint8_t *data, size_t n, char *out) {
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

/* The hash of bytes: PostgreSQL's hash of the byte string. */
uint64_t tyr_bytea_hash(const uint8_t *data, size_t n, uint64_t seed) {
  return ty_hash_bytes(data, n, seed);
}

/* Copy a, then b, to out. */
void tyr_bytea_cat(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                   uint8_t *out) {
  if (na > 0) {
    memcpy(out, a, na);
  }
  if (nb > 0) {
    memcpy(out + na, b, nb);
  }
}

/* Copy a to out, last byte first: 8 bytes at a time, then the rest. */
void tyr_bytea_reverse(const uint8_t *a, size_t n, uint8_t *out) {
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
int tyr_bytea_overlay_size(size_t na, size_t nb, int32_t start, int32_t len,
                           size_t *size) {
  size_t pre, from;

  if (overlay_cuts(na, start, len, &pre, &from) != 0) {
    return -1;
  }
  *size = pre + nb + (na - from);
  return 0;
}

/* Write the overlay: a before start, b, a from start + len. */
int tyr_bytea_overlay(const uint8_t *a, size_t na, const uint8_t *b, size_t nb,
                      int32_t start, int32_t len, uint8_t *out) {
  size_t pre, from;

  if (overlay_cuts(na, start, len, &pre, &from) != 0) {
    return -1;
  }
  tyr_bytea_cat(a, pre, b, nb, out);
  if (na > from) {
    memcpy(out + pre + nb, a + from, na - from);
  }
  return 0;
}

/* Find b in a by trying each place; an empty b is at 1. */
int64_t tyr_bytea_position(const uint8_t *a, size_t na, const uint8_t *b,
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

namespace {

/* Whether the byte c is one of the m bytes at set. */
inline bool in_short_set(uint8_t c, const uint8_t *set, size_t m) {
  for (size_t i = 0; i < m; i++) {
    if (set[i] == c) {
      return true;
    }
  }
  return false;
}

} // namespace

/*
 * The part of a that trim leaves.
 * 1. With one byte in the set, compare to it; with a short set, look each
 *    byte up in it; with a longer one, mark its bytes in a table of 256.
 * 2. Step over bytes of the set from the start, if left.
 * 3. Step back over bytes of the set from the end, if right, not past the
 *    start.
 */
void tyr_bytea_trim(const uint8_t *a, size_t n, const uint8_t *set, size_t m,
                    int left, int right, size_t *off, size_t *len) {
  size_t b = 0, e = n;

  if (1 == m) {
    // 2. and 3., with one byte in the set: compare to it.
    uint8_t c = set[0];

    if (left) {
      while (b < e && a[b] == c) {
        b++;
      }
    }
    if (right) {
      while (e > b && a[e - 1] == c) {
        e--;
      }
    }
  } else if (m <= 16) {
    // 2. Step over bytes of the set from the start.
    if (left) {
      while (b < e && in_short_set(a[b], set, m)) {
        b++;
      }
    }

    // 3. Step back over bytes of the set from the end.
    if (right) {
      while (e > b && in_short_set(a[e - 1], set, m)) {
        e--;
      }
    }
  } else {
    // 1. Mark the bytes of a longer set.
    bool in[256] = {};

    for (size_t i = 0; i < m; i++) {
      in[set[i]] = true;
    }
    if (left) {
      while (b < e && in[a[b]]) {
        b++;
      }
    }
    if (right) {
      while (e > b && in[a[e - 1]]) {
        e--;
      }
    }
  }
  *off = b;
  *len = e - b;
}

/* Add up the set bits of the bytes: 8 bytes at a time, then the rest. */
int64_t tyr_bytea_bit_count(const uint8_t *a, size_t n) {
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
int tyr_bytea_set_byte(uint8_t *a, size_t n, int32_t index, int32_t value) {
  if (index < 0 || (size_t)index >= n) {
    return ty_bytea_err_index(index, (long long)n);
  }
  a[index] = (uint8_t)value;
  return 0;
}

/* Set the bit at index; the index is checked before the bit. */
int tyr_bytea_set_bit(uint8_t *a, size_t n, int64_t index, int32_t bit) {
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
int tyr_bytea_to_int16(const uint8_t *a, size_t n, int16_t *r) {
  uint16_t v;

  if (to_unsigned<uint16_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int16_t)v;
  return 0;
}

/* The int32 of at most 4 bytes. */
int tyr_bytea_to_int32(const uint8_t *a, size_t n, int32_t *r) {
  uint32_t v;

  if (to_unsigned<uint32_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int32_t)v;
  return 0;
}

/* The int64 of at most 8 bytes. */
int tyr_bytea_to_int64(const uint8_t *a, size_t n, int64_t *r) {
  uint64_t v;

  if (to_unsigned<uint64_t>(a, n, &v) != 0) {
    return -1;
  }
  *r = (int64_t)v;
  return 0;
}

/* The 2 bytes of an int16. */
void tyr_int16_to_bytea(int16_t v, uint8_t out[2]) {
  from_unsigned<uint16_t>((uint16_t)v, out);
}

/* The 4 bytes of an int32. */
void tyr_int32_to_bytea(int32_t v, uint8_t out[4]) {
  from_unsigned<uint32_t>((uint32_t)v, out);
}

/* The 8 bytes of an int64. */
void tyr_int64_to_bytea(int64_t v, uint8_t out[8]) {
  from_unsigned<uint64_t>((uint64_t)v, out);
}

/* True if name is lit, in any case; lit is lowercase letters and digits. */
static bool is_name(const char *name, const char *lit) {
  for (; *lit; lit++, name++) {
    if ((*name | 0x20) != *lit) {
      return false;
    }
  }
  return *name == '\0';
}

/* The encoding of a name, in any case. */
int tyr_bytea_encoding(const char *name, ty_bytea_enc_t *enc) {
  char msg[128];

  switch (name[0] | 0x20) {
  case 'h':
    if (is_name(name, "hex")) {
      *enc = TY_BYTEA_ENC_HEX;
      return 0;
    }
    break;
  case 'b':
    if (is_name(name, "base64")) {
      *enc = TY_BYTEA_ENC_BASE64;
      return 0;
    }
    break;
  case 'e':
    if (is_name(name, "escape")) {
      *enc = TY_BYTEA_ENC_ESCAPE;
      return 0;
    }
    break;
  }
  snprintf(msg, sizeof msg, "unrecognized encoding: \"%.80s\"", name);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

/* The most characters the encoding of n bytes takes. */
size_t tyr_bytea_encode_size(ty_bytea_enc_t enc, const uint8_t *data,
                             size_t n) {
  switch (enc) {
  case TY_BYTEA_ENC_HEX:
    return n * 2;
  case TY_BYTEA_ENC_BASE64:
    /* 3 bytes become 4 characters, and a line feed follows each 76 */
    return (n + 2) / 3 * 4 + n / (76 * 3 / 4);
  default: {
    size_t size = 0;

    for (size_t i = 0; i < n; i++) {
      size += (data[i] == 0 || data[i] >= 0x80) ? 4 : data[i] == '\\' ? 2 : 1;
    }
    return size;
  }
  }
}

/* Write the encoding of the bytes; the length written. */
size_t tyr_bytea_encode(ty_bytea_enc_t enc, const uint8_t *data, size_t n,
                        char *out) {
  switch (enc) {
  case TY_BYTEA_ENC_HEX:
    for (size_t i = 0; i < n; i++) {
      memcpy(out + 2 * i, &hex_table[2 * (size_t)data[i]], 2);
    }
    return n * 2;
  case TY_BYTEA_ENC_BASE64:
    return encode_base64(data, n, out);
  default: {
    char *p = out;

    for (size_t i = 0; i < n; i++) {
      uint8_t c = data[i];

      if (c == 0 || c >= 0x80) {
        *p++ = '\\';
        *p++ = (char)('0' + (c >> 6));
        *p++ = (char)('0' + ((c >> 3) & 7));
        *p++ = (char)('0' + (c & 7));
      } else if (c == '\\') {
        *p++ = '\\';
        *p++ = '\\';
      } else {
        *p++ = (char)c;
      }
    }
    return (size_t)(p - out);
  }
  }
}

/* The most bytes the n characters decode to; escape is scanned for faults. */
int tyr_bytea_decode_size(ty_bytea_enc_t enc, const char *s, size_t n,
                          size_t *size) {
  switch (enc) {
  case TY_BYTEA_ENC_HEX:
    *size = n / 2;
    return 0;
  case TY_BYTEA_ENC_BASE64:
    *size = n * 3 / 4;
    return 0;
  default: {
    size_t i = 0, len = 0;

    while (i < n) {
      if (s[i] != '\\') {
        i++;
      } else if (is_octal_escape(s, i, n)) {
        i += 4;
      } else if (i + 1 < n && s[i + 1] == '\\') {
        i += 2;
      } else {
        return ty_internal::set_error(TY_ERR_INVALID,
                                      "invalid input syntax for type bytea");
      }
      len++;
    }
    *size = len;
    return 0;
  }
  }
}

/* Decode the text. */
int tyr_bytea_decode(ty_bytea_enc_t enc, const char *s, size_t n, uint8_t *out,
                     size_t *outlen) {
  switch (enc) {
  case TY_BYTEA_ENC_HEX:
    return parse_hex(s, n, 0, out, outlen);
  case TY_BYTEA_ENC_BASE64:
    return decode_base64(s, n, out, outlen);
  default:
    return decode_escape(s, n, out, outlen);
  }
}

/* ---- the operations on a varlena: each reads its inputs' bytes, calls the
 * function above, and puts a result in memory from the thread's alloc ---- */

namespace {

/* The place and length of the bytes of v. */
struct bytes_t {
  const uint8_t *p;
  size_t n;
  explicit bytes_t(const ty_varlena_t *v)
      : p((const uint8_t *)ty_vl_data(v)), n(ty_vl_len(v)) {}
};

} // namespace

int ty_bytea_parse(const char *s, size_t len, ty_varlena_t **out) {
  ty_varlena_t *r;
  char *d;
  size_t n;

  /* the most bytes the text can hold: a hex pair, or a character */
  size_t room = ty_bytea_is_hex_text(s, len) ? (len - 2) / 2 : len;

  if (ty_vl_new(room, &r, &d) != 0) {
    return -1;
  }
  if (tyr_bytea_parse(s, len, (uint8_t *)d, &n) != 0) {
    ty_free(r);
    return -1;
  }
  ty_vl_set_len(r, n);
  *out = r;
  return 0;
}

size_t ty_bytea_hex_size(const ty_varlena_t *a) {
  return tyr_bytea_hex_size(ty_vl_len(a));
}

void ty_bytea_format_hex(const ty_varlena_t *a, char *out) {
  bytes_t x(a);

  tyr_bytea_format_hex(x.p, x.n, out);
}

size_t ty_bytea_escape_size(const ty_varlena_t *a) {
  bytes_t x(a);

  return tyr_bytea_escape_size(x.p, x.n);
}

void ty_bytea_format_escape(const ty_varlena_t *a, char *out) {
  bytes_t x(a);

  tyr_bytea_format_escape(x.p, x.n, out);
}

int ty_bytea_encoding(const char *name, ty_bytea_enc_t *enc) {
  return tyr_bytea_encoding(name, enc);
}

int ty_bytea_encode(ty_bytea_enc_t enc, const ty_varlena_t *data,
                    ty_varlena_t **out) {
  bytes_t x(data);
  ty_varlena_t *r;
  char *d;
  size_t n = tyr_bytea_encode_size(enc, x.p, x.n);

  if (ty_vl_new(n, &r, &d) != 0) {
    return -1;
  }
  ty_vl_set_len(r, tyr_bytea_encode(enc, x.p, x.n, d));
  *out = r;
  return 0;
}

int ty_bytea_decode(ty_bytea_enc_t enc, const ty_varlena_t *text,
                    ty_varlena_t **out) {
  const char *s = ty_vl_data(text);
  size_t n = ty_vl_len(text), size, len;
  ty_varlena_t *r;
  char *d;

  if (tyr_bytea_decode_size(enc, s, n, &size) != 0 ||
      ty_vl_new(size, &r, &d) != 0) {
    return -1;
  }
  if (tyr_bytea_decode(enc, s, n, (uint8_t *)d, &len) != 0) {
    ty_free(r);
    return -1;
  }
  ty_vl_set_len(r, len);
  *out = r;
  return 0;
}

uint64_t ty_bytea_hash(const ty_varlena_t *a, uint64_t seed) {
  bytes_t x(a);

  return tyr_bytea_hash(x.p, x.n, seed);
}

int ty_bytea_reverse(const ty_varlena_t *a, ty_varlena_t **out) {
  bytes_t x(a);
  char *d;

  if (ty_vl_new(x.n, out, &d) != 0) {
    return -1;
  }
  tyr_bytea_reverse(x.p, x.n, (uint8_t *)d);
  return 0;
}

int ty_bytea_overlay(const ty_varlena_t *a, const ty_varlena_t *b,
                     int32_t start, int32_t len, ty_varlena_t **out) {
  bytes_t x(a), y(b);
  size_t size;
  char *d;

  if (tyr_bytea_overlay_size(x.n, y.n, start, len, &size) != 0 ||
      ty_vl_new(size, out, &d) != 0) {
    return -1;
  }
  /* the sizes are checked, so this cannot fail */
  tyr_bytea_overlay(x.p, x.n, y.p, y.n, start, len, (uint8_t *)d);
  return 0;
}

int64_t ty_bytea_position(const ty_varlena_t *a, const ty_varlena_t *b) {
  bytes_t x(a), y(b);

  return tyr_bytea_position(x.p, x.n, y.p, y.n);
}

int64_t ty_bytea_bit_count(const ty_varlena_t *a) {
  bytes_t x(a);

  return tyr_bytea_bit_count(x.p, x.n);
}

void ty_bytea_trim_range(const ty_varlena_t *a, const ty_varlena_t *set,
                         int left, int right, size_t *off, size_t *len) {
  bytes_t x(a), y(set);

  tyr_bytea_trim(x.p, x.n, y.p, y.n, left, right, off, len);
}

/* A copy of a, for the functions that change a byte or a bit of it. */
static int copy_of(const ty_varlena_t *a, ty_varlena_t **r, uint8_t **d) {
  char *p;

  if (ty_vl_new_copy(ty_vl_data(a), ty_vl_len(a), r) != 0) {
    return -1;
  }
  p = (char *)ty_vl_data(*r);
  *d = (uint8_t *)p;
  return 0;
}

int ty_bytea_set_byte(const ty_varlena_t *a, int32_t index, int32_t value,
                      ty_varlena_t **out) {
  ty_varlena_t *r;
  uint8_t *d;

  if (copy_of(a, &r, &d) != 0) {
    return -1;
  }
  if (tyr_bytea_set_byte(d, ty_vl_len(r), index, value) != 0) {
    ty_free(r);
    return -1;
  }
  *out = r;
  return 0;
}

int ty_bytea_set_bit(const ty_varlena_t *a, int64_t index, int32_t bit,
                     ty_varlena_t **out) {
  ty_varlena_t *r;
  uint8_t *d;

  if (copy_of(a, &r, &d) != 0) {
    return -1;
  }
  if (tyr_bytea_set_bit(d, ty_vl_len(r), index, bit) != 0) {
    ty_free(r);
    return -1;
  }
  *out = r;
  return 0;
}

int ty_bytea_to_int16(const ty_varlena_t *a, int16_t *r) {
  bytes_t x(a);

  return tyr_bytea_to_int16(x.p, x.n, r);
}

int ty_bytea_to_int32(const ty_varlena_t *a, int32_t *r) {
  bytes_t x(a);

  return tyr_bytea_to_int32(x.p, x.n, r);
}

int ty_bytea_to_int64(const ty_varlena_t *a, int64_t *r) {
  bytes_t x(a);

  return tyr_bytea_to_int64(x.p, x.n, r);
}

int ty_int16_to_bytea(int16_t v, ty_varlena_t **out) {
  uint8_t b[2];

  tyr_int16_to_bytea(v, b);
  return ty_vl_new_copy((const char *)b, sizeof b, out);
}

int ty_int32_to_bytea(int32_t v, ty_varlena_t **out) {
  uint8_t b[4];

  tyr_int32_to_bytea(v, b);
  return ty_vl_new_copy((const char *)b, sizeof b, out);
}

int ty_int64_to_bytea(int64_t v, ty_varlena_t **out) {
  uint8_t b[8];

  tyr_int64_to_bytea(v, b);
  return ty_vl_new_copy((const char *)b, sizeof b, out);
}
