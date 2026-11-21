/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * mac.cc -- the operations of macaddr and macaddr8.
 */
#include "ty_mac.h"

#include <stdio.h>

#include "error.h"
#include "ty_hash.h"

namespace {

/* True for the bytes C's isspace() takes in the "C" locale. */
bool is_space(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* The value of a hexadecimal digit, or -1. */
int hex_digit(unsigned char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

/* A macaddr8 pair of hex digits at p: the byte, or -1. */
int hex_pair(const unsigned char *p) {
  int hi = hex_digit(p[0]), lo = hex_digit(p[1]);

  return (hi < 0 || lo < 0) ? -1 : hi * 16 + lo;
}

} // namespace

/*
 * Read a macaddr. Seven formats are tried in turn with scanf, as the text may
 * hold only one; "%1s" at the end matches only if text follows, so a count
 * of 7 is junk after the address.
 */
int ty_macaddr_parse(const char *s, ty_macaddr_t *r) {
  static const char *const formats[] = {
      "%x:%x:%x:%x:%x:%x%1s",    "%x-%x-%x-%x-%x-%x%1s",
      "%2x%2x%2x:%2x%2x%2x%1s",  "%2x%2x%2x-%2x%2x%2x%1s",
      "%2x%2x.%2x%2x.%2x%2x%1s", "%2x%2x-%2x%2x-%2x%2x%1s",
      "%2x%2x%2x%2x%2x%2x%1s"};
  int v[6];
  char junk[2];
  int count = 0;

  for (const char *fmt : formats) {
    count = sscanf(s, fmt, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], junk);
    if (count == 6) {
      break;
    }
  }
  if (count != 6) {
    return ty_err_invalid("invalid macaddr text");
  }
  for (int i = 0; i < 6; i++) {
    if (v[i] < 0 || v[i] > 255) {
      return ty_err_range();
    }
  }
  for (int i = 0; i < 6; i++) {
    r->b[i] = (uint8_t)v[i];
  }
  return 0;
}

/*
 * Read a macaddr8.
 * 1. Skip the space at the start.
 * 2. Read pairs of hex digits; between pairs one of ':', '-' or '.', the same
 *    one every time, or none.
 * 3. After the 6th or 8th pair, space may follow, and then the text must end.
 * 4. Six pairs become eight: FF:FE in the middle. Any other count is invalid.
 */
int ty_macaddr8_parse(const char *s, ty_macaddr8_t *r) {
  const unsigned char *p = (const unsigned char *)s;
  uint8_t v[8] = {};
  int count = 0;
  unsigned char spacer = 0;

  while (*p && is_space(*p)) {
    p++;
  }
  while (*p && *(p + 1)) {
    int byte;

    count++;
    if (count > 8) {
      goto bad;
    }
    byte = hex_pair(p);
    if (byte < 0) {
      goto bad;
    }
    v[count - 1] = (uint8_t)byte;
    p += 2;
    if (*p == ':' || *p == '-' || *p == '.') {
      if (spacer == 0) {
        spacer = *p;
      } else if (spacer != *p) {
        goto bad;
      }
      p++;
    }
    if (count == 6 || count == 8) {
      if (is_space(*p)) {
        while (*++p && is_space(*p)) {
        }
        if (*p) {
          goto bad;
        }
      }
    }
  }
  if (count == 6) {
    v[7] = v[5];
    v[6] = v[4];
    v[5] = v[3];
    v[3] = 0xFF;
    v[4] = 0xFE;
  } else if (count != 8) {
    goto bad;
  }
  memcpy(r->b, v, 8);
  return 0;
bad:
  return ty_err_invalid("invalid macaddr8 text");
}

/* Write a macaddr. */
void ty_macaddr_format(const ty_macaddr_t *a, char out[TY_MACADDR_TEXT_SIZE]) {
  snprintf(out, TY_MACADDR_TEXT_SIZE, "%02x:%02x:%02x:%02x:%02x:%02x", a->b[0],
           a->b[1], a->b[2], a->b[3], a->b[4], a->b[5]);
}

/* Write a macaddr8. */
void ty_macaddr8_format(const ty_macaddr8_t *a,
                        char out[TY_MACADDR8_TEXT_SIZE]) {
  snprintf(out, TY_MACADDR8_TEXT_SIZE,
           "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x", a->b[0], a->b[1], a->b[2],
           a->b[3], a->b[4], a->b[5], a->b[6], a->b[7]);
}

/* The bytes inverted. */
ty_macaddr_t ty_macaddr_not(const ty_macaddr_t *a) {
  ty_macaddr_t r;

  for (int i = 0; i < 6; i++) {
    r.b[i] = (uint8_t)~a->b[i];
  }
  return r;
}

/* The bytes AND-ed. */
ty_macaddr_t ty_macaddr_and(const ty_macaddr_t *a, const ty_macaddr_t *b) {
  ty_macaddr_t r;

  for (int i = 0; i < 6; i++) {
    r.b[i] = a->b[i] & b->b[i];
  }
  return r;
}

/* The bytes OR-ed. */
ty_macaddr_t ty_macaddr_or(const ty_macaddr_t *a, const ty_macaddr_t *b) {
  ty_macaddr_t r;

  for (int i = 0; i < 6; i++) {
    r.b[i] = a->b[i] | b->b[i];
  }
  return r;
}

/* The bytes inverted. */
ty_macaddr8_t ty_macaddr8_not(const ty_macaddr8_t *a) {
  ty_macaddr8_t r;

  for (int i = 0; i < 8; i++) {
    r.b[i] = (uint8_t)~a->b[i];
  }
  return r;
}

/* The bytes AND-ed. */
ty_macaddr8_t ty_macaddr8_and(const ty_macaddr8_t *a, const ty_macaddr8_t *b) {
  ty_macaddr8_t r;

  for (int i = 0; i < 8; i++) {
    r.b[i] = a->b[i] & b->b[i];
  }
  return r;
}

/* The bytes OR-ed. */
ty_macaddr8_t ty_macaddr8_or(const ty_macaddr8_t *a, const ty_macaddr8_t *b) {
  ty_macaddr8_t r;

  for (int i = 0; i < 8; i++) {
    r.b[i] = a->b[i] | b->b[i];
  }
  return r;
}

/* The first 3 bytes, then zeros. */
ty_macaddr_t ty_macaddr_trunc(const ty_macaddr_t *a) {
  ty_macaddr_t r = {};

  memcpy(r.b, a->b, 3);
  return r;
}

/* The first 3 bytes, then zeros. */
ty_macaddr8_t ty_macaddr8_trunc(const ty_macaddr8_t *a) {
  ty_macaddr8_t r = {};

  memcpy(r.b, a->b, 3);
  return r;
}

/* The same bytes with bit 0x02 of the first set. */
ty_macaddr8_t ty_macaddr8_set7bit(const ty_macaddr8_t *a) {
  ty_macaddr8_t r = *a;

  r.b[0] |= 0x02;
  return r;
}

/* The 3 bytes, FF, FE, and the last 3. */
ty_macaddr8_t ty_macaddr_to_macaddr8(const ty_macaddr_t *a) {
  ty_macaddr8_t r;

  r.b[0] = a->b[0];
  r.b[1] = a->b[1];
  r.b[2] = a->b[2];
  r.b[3] = 0xFF;
  r.b[4] = 0xFE;
  r.b[5] = a->b[3];
  r.b[6] = a->b[4];
  r.b[7] = a->b[5];
  return r;
}

/* The first 3 and the last 3 bytes, if the middle is FF:FE. */
int ty_macaddr8_to_macaddr(const ty_macaddr8_t *a, ty_macaddr_t *r) {
  if (a->b[3] != 0xFF || a->b[4] != 0xFE) {
    return ty_internal::set_error(
        TY_ERR_RANGE, "macaddr8 data out of range to convert to macaddr");
  }
  r->b[0] = a->b[0];
  r->b[1] = a->b[1];
  r->b[2] = a->b[2];
  r->b[3] = a->b[5];
  r->b[4] = a->b[6];
  r->b[5] = a->b[7];
  return 0;
}

/* The hash of the 6 bytes. */
uint64_t ty_macaddr_hash(const ty_macaddr_t *a, uint64_t seed) {
  return ty_hash_bytes(a->b, 6, seed);
}

/* The hash of the 8 bytes. */
uint64_t ty_macaddr8_hash(const ty_macaddr8_t *a, uint64_t seed) {
  return ty_hash_bytes(a->b, 8, seed);
}
