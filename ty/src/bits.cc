/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * bits.cc -- the operations of bit and bit varying.
 */
#include "ty_bits.h"

#include <stdio.h>
#include <string.h>

#include "error.h"

namespace {

/* The 8 bits starting at bit offset bit of the string of nbytes bytes. */
inline uint8_t get8(const uint8_t *s, size_t nbytes, size_t bit) {
  size_t i = bit >> 3, sh = bit & 7;
  uint8_t v = (uint8_t)(s[i] << sh);

  if (sh != 0 && i + 1 < nbytes) {
    v |= (uint8_t)(s[i + 1] >> (8 - sh));
  }
  return v;
}

/*
 * Copy n bits from bit sbit of src (nsbytes bytes) to bit dbit of dst, which
 * has zeros there: the bits are OR-ed in.
 * 1. If dbit is at a byte, copy whole bytes: with memcpy if sbit is at a byte
 *    too, else each from two source bytes. Then the bits of the last byte.
 * 2. If not, take 8 bits from src at a time and put them in two bytes of dst.
 */
void copy_bits(uint8_t *dst, size_t dbit, const uint8_t *src, size_t nsbytes,
               size_t sbit, size_t n) {
  if ((dbit & 7) == 0) {
    uint8_t *d = dst + (dbit >> 3);
    const uint8_t *s = src + (sbit >> 3);
    size_t whole = n >> 3, sh = sbit & 7;

    if (sh == 0) {
      memcpy(d, s, whole);
    } else {
      for (size_t i = 0; i < whole; i++) {
        d[i] = (uint8_t)((s[i] << sh) | (s[i + 1] >> (8 - sh)));
      }
    }
    if (n & 7) {
      d[whole] |= (uint8_t)(get8(src, nsbytes, sbit + whole * 8) &
                            (0xFF << (8 - (n & 7))));
    }
    return;
  }
  for (size_t k = 0; k < n; k += 8) {
    size_t chunk = n - k < 8 ? n - k : 8;
    uint8_t v = get8(src, nsbytes, sbit + k);
    size_t d = (dbit + k) >> 3, off = (dbit + k) & 7;

    v &= (uint8_t)(0xFF << (8 - chunk));
    dst[d] |= (uint8_t)(v >> off);
    if (off + chunk > 8) {
      dst[d + 1] |= (uint8_t)(v << (8 - off));
    }
  }
}

/* Zero the bits past nbits in the last byte of out. */
void clear_pad(uint8_t *out, int32_t nbits) {
  if (nbits & 7) {
    out[nbits >> 3] &= (uint8_t)(0xFF << (8 - (nbits & 7)));
  }
}

/* How many bytes the UTF-8 character at s has, of the n left; at least 1. */
size_t char_len(const char *s, size_t n) {
  unsigned char c = (unsigned char)s[0];
  size_t want = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;

  return want < n ? want : n;
}

} // namespace

/*
 * Read a bit string.
 * 1. A leading b or B, or none, means binary digits; x or X, hex digits.
 * 2. Binary: a 1 sets the next bit, a 0 leaves it; anything else is invalid.
 * 3. Hex: each digit is four bits, the first digit in the high four.
 */
int ty_bits_parse(const char *s, size_t len, uint8_t *out, int32_t *nbits) {
  bool binary = true;
  size_t i = 0;
  char msg[96];

  if (len > 0 && (s[0] == 'b' || s[0] == 'B')) {
    i = 1;
  } else if (len > 0 && (s[0] == 'x' || s[0] == 'X')) {
    binary = false;
    i = 1;
  }
  size_t digits = len - i;

  if (!binary && digits > (size_t)TY_BITS_MAX_LEN / 4) {
    snprintf(msg, sizeof msg,
             "bit string length exceeds the maximum allowed (%d)",
             TY_BITS_MAX_LEN);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  size_t nb = binary ? digits : digits * 4;

  memset(out, 0, ty_bits_bytes((int32_t)nb));
  if (binary) {
    for (size_t k = 0; k < digits; k++) {
      char c = s[i + k];

      if (c == '1') {
        out[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
      } else if (c != '0') {
        snprintf(msg, sizeof msg, "\"%.*s\" is not a valid binary digit",
                 (int)char_len(s + i + k, len - i - k), s + i + k);
        return ty_internal::set_error(TY_ERR_INVALID, msg);
      }
    }
  } else {
    for (size_t k = 0; k < digits; k++) {
      char c = s[i + k];
      int v = (c >= '0' && c <= '9')   ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                       : -1;

      if (v < 0) {
        snprintf(msg, sizeof msg, "\"%.*s\" is not a valid hexadecimal digit",
                 (int)char_len(s + i + k, len - i - k), s + i + k);
        return ty_internal::set_error(TY_ERR_INVALID, msg);
      }
      out[k >> 1] |= (uint8_t)((k & 1) ? v : v << 4);
    }
  }
  *nbits = (int32_t)nb;
  return 0;
}

namespace {

/* The 8 characters of each byte value, first bit first. */
struct digits_t {
  char c[256][8];

  constexpr digits_t() : c() {
    for (int v = 0; v < 256; v++) {
      for (int k = 0; k < 8; k++) {
        c[v][k] = (v & (0x80 >> k)) ? '1' : '0';
      }
    }
  }
};

constexpr digits_t k_digits;

} // namespace

/* Write the bits as characters: 8 at a time from a table. */
void ty_bits_format(const uint8_t *a, int32_t nbits, char *out) {
  size_t whole = (size_t)nbits >> 3;

  for (size_t i = 0; i < whole; i++) {
    memcpy(out + i * 8, k_digits.c[a[i]], 8);
  }
  for (int32_t k = (int32_t)(whole * 8); k < nbits; k++) {
    out[k] = (a[k >> 3] & (0x80 >> (k & 7))) ? '1' : '0';
  }
  out[nbits] = '\0';
}

/* Equal: the same length and the same bytes. */
int ty_bits_eq(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb) {
  return na == nb && ty_bits_cmp(a, na, b, nb) == 0;
}

/* Concatenate. */
int ty_bits_cat(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb,
                uint8_t *out, int32_t *nout) {
  char msg[96];

  if (na > TY_BITS_MAX_LEN - nb) {
    snprintf(msg, sizeof msg,
             "bit string length exceeds the maximum allowed (%d)",
             TY_BITS_MAX_LEN);
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  /*
   * Of a, whole bytes are copied; the copies OR into a byte that is only part
   * of the result, so that byte, and every one if a ends mid-byte, is zeroed.
   */
  size_t nbytes = ty_bits_bytes(na + nb);

  if (na & 7) {
    memset(out, 0, nbytes);
  } else if (nbytes > 0) {
    out[nbytes - 1] = 0;
  }
  copy_bits(out, 0, a, ty_bits_bytes(na), 0, (size_t)na);
  copy_bits(out, (size_t)na, b, ty_bits_bytes(nb), 0, (size_t)nb);
  *nout = na + nb;
  return 0;
}

/*
 * The part SUBSTRING names.
 * 1. The first bit is at start, but not before 1.
 * 2. The end (excluded) is start + len, held to the length + 1; with no length,
 *    or if start + len overflows, it is the length + 1.
 * 3. A part that begins past the string, or is empty, is empty.
 */
int ty_bits_substr(const uint8_t *a, int32_t na, int32_t start, int32_t len,
                   int has_len, uint8_t *out, int32_t *nout) {
  int32_t s1 = start > 1 ? start : 1, e1, e;

  if (!has_len) {
    e1 = na + 1;
  } else if (len < 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "negative substring length not allowed");
  } else if (__builtin_add_overflow(start, len, &e)) {
    e1 = na + 1;
  } else {
    e1 = e < na + 1 ? e : na + 1;
  }
  if (s1 > na || e1 <= s1) {
    *nout = 0;
    return 0;
  }
  int32_t n = e1 - s1;

  memset(out, 0, ty_bits_bytes(n));
  copy_bits(out, 0, a, ty_bits_bytes(na), (size_t)(s1 - 1), (size_t)n);
  *nout = n;
  return 0;
}

/*
 * OVERLAY: the bits of a before start, then b, then the bits of a from
 * start + len on.
 */
int ty_bits_overlay(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb,
                    int32_t start, int32_t len, uint8_t *out, int32_t *nout) {
  int32_t sum;

  if (start <= 0) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "negative substring length not allowed");
  }
  if (__builtin_add_overflow(start, len, &sum)) {
    return ty_err_range();
  }
  int32_t pre = start - 1 < na ? start - 1 : na;
  int32_t from = (sum > 1 ? sum : 1) - 1;
  int32_t rest = from < na ? na - from : 0;

  if (pre > TY_BITS_MAX_LEN - nb || pre + nb > TY_BITS_MAX_LEN - rest) {
    return ty_err_range();
  }
  memset(out, 0, ty_bits_bytes(pre + nb + rest));
  copy_bits(out, 0, a, ty_bits_bytes(na), 0, (size_t)pre);
  copy_bits(out, (size_t)pre, b, ty_bits_bytes(nb), 0, (size_t)nb);
  if (rest > 0) {
    copy_bits(out, (size_t)(pre + nb), a, ty_bits_bytes(na), (size_t)from,
              (size_t)rest);
  }
  *nout = pre + nb + rest;
  return 0;
}

/* Find b in a: try each place. */
int32_t ty_bits_position(const uint8_t *a, int32_t na, const uint8_t *b,
                         int32_t nb) {
  if (na == 0 || nb > na) {
    return 0;
  }
  if (nb == 0) {
    return 1;
  }
  size_t ba = ty_bits_bytes(na), bb = ty_bits_bytes(nb);

  if (nb <= 8) {
    /* the whole of b is one byte: compare a window of a with it */
    uint8_t mask = (uint8_t)(0xFF << (8 - nb)), pat = b[0] & mask;

    for (int32_t p = 0; p + nb <= na; p++) {
      if (((get8(a, ba, (size_t)p) ^ pat) & mask) == 0) {
        return p + 1;
      }
    }
    return 0;
  }
  for (int32_t p = 0; p + nb <= na; p++) {
    bool match = true;

    for (size_t k = 0; k < bb && match; k++) {
      size_t remain = (size_t)nb - k * 8;
      uint8_t mask = remain >= 8 ? 0xFF : (uint8_t)(0xFF << (8 - remain));

      match = (uint8_t)((get8(a, ba, (size_t)p + k * 8) ^ b[k]) & mask) == 0;
    }
    if (match) {
      return p + 1;
    }
  }
  return 0;
}

/* Add up the set bits (the pad is zero). */
int64_t ty_bits_count(const uint8_t *a, int32_t nbits) {
  int64_t count = 0;
  size_t n = ty_bits_bytes(nbits), i = 0;

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

namespace {

/* The error of a binary operation on strings of two lengths. */
int length_mismatch(const char *what) {
  char msg[96];

  snprintf(msg, sizeof msg, "cannot %s bit strings of different sizes", what);
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

} // namespace

/* AND. */
int ty_bits_and(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb,
                uint8_t *out) {
  if (na != nb) {
    return length_mismatch("AND");
  }
  for (size_t i = 0; i < ty_bits_bytes(na); i++) {
    out[i] = a[i] & b[i];
  }
  return 0;
}

/* OR. */
int ty_bits_or(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb,
               uint8_t *out) {
  if (na != nb) {
    return length_mismatch("OR");
  }
  for (size_t i = 0; i < ty_bits_bytes(na); i++) {
    out[i] = a[i] | b[i];
  }
  return 0;
}

/* XOR. */
int ty_bits_xor(const uint8_t *a, int32_t na, const uint8_t *b, int32_t nb,
                uint8_t *out) {
  if (na != nb) {
    return length_mismatch("XOR");
  }
  for (size_t i = 0; i < ty_bits_bytes(na); i++) {
    out[i] = a[i] ^ b[i];
  }
  return 0;
}

/* NOT, with the pad cleared. */
void ty_bits_not(const uint8_t *a, int32_t nbits, uint8_t *out) {
  for (size_t i = 0; i < ty_bits_bytes(nbits); i++) {
    out[i] = (uint8_t)~a[i];
  }
  clear_pad(out, nbits);
}

/*
 * Shift left: the bits move toward the start, zeros come in at the end. A
 * negative count shifts right. The count is held to TY_BITS_MAX_LEN.
 */
void ty_bits_shl(const uint8_t *a, int32_t nbits, int32_t n, uint8_t *out) {
  if (n < 0) {
    ty_bits_shr(a, nbits, n < -TY_BITS_MAX_LEN ? TY_BITS_MAX_LEN : -n, out);
    return;
  }
  size_t nbytes = ty_bits_bytes(nbits);

  memset(out, 0, nbytes);
  if (n < nbits) {
    copy_bits(out, 0, a, nbytes, (size_t)n, (size_t)(nbits - n));
  }
}

/* Shift right: the bits move toward the end, zeros come in at the start. */
void ty_bits_shr(const uint8_t *a, int32_t nbits, int32_t n, uint8_t *out) {
  if (n < 0) {
    ty_bits_shl(a, nbits, n < -TY_BITS_MAX_LEN ? TY_BITS_MAX_LEN : -n, out);
    return;
  }
  size_t nbytes = ty_bits_bytes(nbits);

  memset(out, 0, nbytes);
  if (n < nbits) {
    copy_bits(out, (size_t)n, a, nbytes, 0, (size_t)(nbits - n));
  }
}

namespace {

/* Record a bit index outside 0..nbits-1. */
int bad_bit_index(int32_t index, int32_t nbits) {
  char msg[96];

  snprintf(msg, sizeof msg, "bit index %d out of valid range (0..%d)", index,
           nbits - 1);
  return ty_internal::set_error(TY_ERR_RANGE, msg);
}

} // namespace

/* The bit at index. */
int ty_bits_get(const uint8_t *a, int32_t nbits, int32_t index, int32_t *r) {
  if (index < 0 || index >= nbits) {
    return bad_bit_index(index, nbits);
  }
  *r = (a[index >> 3] >> (7 - (index & 7))) & 1;
  return 0;
}

/* A copy with the bit at index set. */
int ty_bits_set(const uint8_t *a, int32_t nbits, int32_t index, int32_t bit,
                uint8_t *out) {
  if (index < 0 || index >= nbits) {
    return bad_bit_index(index, nbits);
  }
  if (bit != 0 && bit != 1) {
    return ty_internal::set_error(TY_ERR_INVALID, "new bit must be 0 or 1");
  }
  memcpy(out, a, ty_bits_bytes(nbits));
  uint8_t mask = (uint8_t)(0x80 >> (index & 7));

  out[index >> 3] = bit ? (uint8_t)(out[index >> 3] | mask)
                        : (uint8_t)(out[index >> 3] & ~mask);
  return 0;
}

namespace {

/*
 * An integer of w bits (32 or 64), two's complement, as len bits.
 * 1. Put the number's w bits, high byte first, in a small buffer.
 * 2. If len is at most w, the string is the low len of them.
 * 3. If not, the string is the sign for len - w bits, then all w bits.
 */
template <typename U> void from_integer(U v, int32_t len, uint8_t *out) {
  const int w = (int)(sizeof(U) * 8);
  size_t nbytes = ty_bits_bytes(len);

  if (len <= w) {
    /* the low len bits, moved to the top: the bits under them are zero */
    U top = (U)(v << (w - len));

    /* high byte first: no loop, as nbytes is at most 4 or 8 */
    out[0] = (uint8_t)(top >> (w - 8));
    if (nbytes > 1) {
      out[1] = (uint8_t)(top >> (w - 16));
    }
    if (nbytes > 2) {
      out[2] = (uint8_t)(top >> (w - 24));
      if (nbytes > 3) {
        out[3] = (uint8_t)(top >> (w - 32));
      }
    }
    for (size_t i = 4; i < nbytes; i++) {
      out[i] = (uint8_t)(top >> (w - 8 - 8 * (int)i));
    }
    return;
  }
  size_t ext = (size_t)(len - w);
  uint8_t be[sizeof(U)];

  for (int i = 0; i < w / 8; i++) {
    be[i] = (uint8_t)(v >> (w - 8 - 8 * i));
  }

  /* the sign bits: whole bytes of ones, then the high bits of the next */
  memset(out, 0, nbytes);
  if (v >> (w - 1)) {
    memset(out, 0xFF, ext >> 3);
    if (ext & 7) {
      out[ext >> 3] = (uint8_t)(0xFF << (8 - (ext & 7)));
    }
  }
  copy_bits(out, ext, be, sizeof be, 0, (size_t)w);
}

/* The bits as an unsigned number of type U: bytes, then drop the pad. */
template <typename U> int to_integer(const uint8_t *a, int32_t nbits, U *r) {
  if ((size_t)nbits > sizeof(U) * 8) {
    return ty_err_range();
  }
  size_t nbytes = ty_bits_bytes(nbits);
  U v = 0;

  for (size_t i = 0; i < nbytes; i++) {
    v = (U)((v << 8) | a[i]);
  }
  /* the pad is the low bits of the last byte */
  *r = nbytes == 0 ? 0 : (U)(v >> (nbytes * 8 - (size_t)nbits));
  return 0;
}

} // namespace

/* An int32 as len bits: the lengths the inline version of the header leaves. */
void ty_bits_from_int32_slow(int32_t a, int32_t len, uint8_t *out,
                             int32_t *nout) {
  if (len <= 0 || len > TY_BITS_MAX_LEN) {
    len = 1;
  }
  from_integer<uint32_t>((uint32_t)a, len, out);
  *nout = len;
}

/* An int64 as len bits: the lengths the inline version of the header leaves. */
void ty_bits_from_int64_slow(int64_t a, int32_t len, uint8_t *out,
                             int32_t *nout) {
  if (len <= 0 || len > TY_BITS_MAX_LEN) {
    len = 1;
  }
  from_integer<uint64_t>((uint64_t)a, len, out);
  *nout = len;
}

/* The bits as an int32. */
int ty_bits_to_int32(const uint8_t *a, int32_t nbits, int32_t *r) {
  uint32_t v;

  if (to_integer<uint32_t>(a, nbits, &v) != 0) {
    return -1;
  }
  *r = (int32_t)v;
  return 0;
}

/* The bits as an int64. */
int ty_bits_to_int64(const uint8_t *a, int32_t nbits, int64_t *r) {
  uint64_t v;

  if (to_integer<uint64_t>(a, nbits, &v) != 0) {
    return -1;
  }
  *r = (int64_t)v;
  return 0;
}

/* Cut or pad to len bits. */
void ty_bits_resize(const uint8_t *a, int32_t nbits, int32_t len,
                    uint8_t *out) {
  size_t out_bytes = ty_bits_bytes(len), in_bytes = ty_bits_bytes(nbits);

  memset(out, 0, out_bytes);
  memcpy(out, a, in_bytes < out_bytes ? in_bytes : out_bytes);
  clear_pad(out, len < nbits ? len : nbits);
  if (len > nbits) {
    /* the bits past nbits in the last copied byte are zero already */
  }
}
