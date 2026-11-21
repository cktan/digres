/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * uuid.cc -- the operations of uuid.
 */
#include "ty_uuid.h"

#include <errno.h>
#include <sys/random.h>

#include "error.h"
#include "ty_hash.h"

namespace {

/* The value of a hexadecimal digit, or -1 if c is not one. */
int hex_value(char c) {
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

/* Fill n bytes at p with random bytes from the system; false if it fails. */
bool fill_random(uint8_t *p, size_t n) {
  while (n > 0) {
    ssize_t got = getrandom(p, n, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    p += got;
    n -= (size_t)got;
  }
  return true;
}

/* Set the version in the bits of byte 6, and the RFC variant in byte 8. */
void set_version(ty_uuid_t *u, int version) {
  u->data[6] = (uint8_t)((u->data[6] & 0x0f) | (version << 4));
  u->data[8] = (uint8_t)((u->data[8] & 0x3f) | 0x80);
}

} // namespace

/*
 * Parse a uuid.
 * 1. Skip an opening brace; remember it.
 * 2. Read 16 bytes, two digits each; after the second byte of a pair, and
 *    not after the last, skip one hyphen if it is there.
 * 3. Take the closing brace if there was an opening one.
 * 4. Fail unless that is all of the text.
 */
int ty_uuid_parse(const char *s, size_t len, ty_uuid_t *u) {
  ty_uuid_t v;
  size_t p = 0;
  bool braces = false;

  if (len > 0 && s[0] == '{') {
    p++;
    braces = true;
  }
  for (int i = 0; i < 16; i++) {
    if (p + 1 >= len) {
      goto bad;
    }
    int hi = hex_value(s[p]), lo = hex_value(s[p + 1]);
    if (hi < 0 || lo < 0) {
      goto bad;
    }
    v.data[i] = (uint8_t)(hi << 4 | lo);
    p += 2;
    if (p < len && s[p] == '-' && i % 2 == 1 && i < 15) {
      p++;
    }
  }
  if (braces) {
    if (p >= len || s[p] != '}') {
      goto bad;
    }
    p++;
  }
  if (p != len) {
    goto bad;
  }
  *u = v;
  return 0;
bad:
  return ty_internal::set_error(TY_ERR_INVALID, "invalid uuid");
}

/* Write u as 8-4-4-4-12 lowercase hexadecimal digits and a NUL. */
void ty_uuid_format(const ty_uuid_t *u, char out[TY_UUID_TEXT_SIZE]) {
  static const char digits[] = "0123456789abcdef";
  char *p = out;

  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      *p++ = '-';
    }
    *p++ = digits[u->data[i] >> 4];
    *p++ = digits[u->data[i] & 0x0f];
  }
  *p = '\0';
}

/*
 * The uuid one above a: add 1 to the 128-bit number of the bytes.
 * 1. From the last byte, a byte below 255 gets 1 added and the rest stay.
 * 2. A byte of 255 becomes 0, and the carry goes to the byte before.
 * 3. A carry out of the first byte is an overflow.
 */
int ty_uuid_increment(const ty_uuid_t *a, ty_uuid_t *r) {
  ty_uuid_t v = *a;

  for (int i = 15; i >= 0; i--) {
    if (v.data[i] < 255) {
      v.data[i]++;
      *r = v;
      return 0;
    }
    v.data[i] = 0;
  }
  return ty_err_range();
}

/* The uuid one below a: the same, with a borrow. */
int ty_uuid_decrement(const ty_uuid_t *a, ty_uuid_t *r) {
  ty_uuid_t v = *a;

  for (int i = 15; i >= 0; i--) {
    if (v.data[i] > 0) {
      v.data[i]--;
      *r = v;
      return 0;
    }
    v.data[i] = 255;
  }
  return ty_err_range();
}

/* The hash of a uuid: that of its 16 bytes. */
uint64_t ty_uuid_hash(const ty_uuid_t *u, uint64_t seed) {
  return ty_hash_bytes(u->data, 16, seed);
}

/* The version of a uuid of the RFC variant; -1 for another variant. */
int ty_uuid_version(const ty_uuid_t *u) {
  if ((u->data[8] & 0xc0) != 0x80) {
    return -1;
  }
  return u->data[6] >> 4;
}

/*
 * The time of a version 1 or 7 uuid, in microseconds since the Unix epoch.
 * 1. Version 1: 60 bits of 100-ns ticks since 1582-10-15, kept in a
 *    scattered order; put them together, make them microseconds, and move
 *    the epoch to 1970-01-01 (12,219,292,800 seconds later).
 * 2. Version 7: 48 bits of milliseconds since the Unix epoch.
 */
int ty_uuid_unix_us(const ty_uuid_t *u, int64_t *us) {
  const uint8_t *d = u->data;
  int version = ty_uuid_version(u);

  if (version == 1) {
    uint64_t ticks = ((uint64_t)d[0] << 24) + ((uint64_t)d[1] << 16) +
                     ((uint64_t)d[2] << 8) + (uint64_t)d[3] +
                     ((uint64_t)d[4] << 40) + ((uint64_t)d[5] << 32) +
                     (((uint64_t)d[6] & 0xf) << 56) + ((uint64_t)d[7] << 48);

    *us = (int64_t)(ticks / 10) - 12219292800LL * 1000000;
    return 0;
  }
  if (version == 7) {
    uint64_t ms = ((uint64_t)d[0] << 40) + ((uint64_t)d[1] << 32) +
                  ((uint64_t)d[2] << 24) + ((uint64_t)d[3] << 16) +
                  ((uint64_t)d[4] << 8) + (uint64_t)d[5];

    *us = (int64_t)(ms * 1000);
    return 0;
  }
  return 1;
}

/* Make a version 4 uuid: 16 random bytes, then the version and variant. */
int ty_uuid_v4(ty_uuid_t *u) {
  ty_uuid_t v;

  if (!fill_random(v.data, 16)) {
    return ty_internal::set_error(TY_ERR_EXCEPTION,
                                  "could not generate random values");
  }
  set_version(&v, 4);
  *u = v;
  return 0;
}

/*
 * Make a version 7 uuid.
 * 1. Put the 48 bits of the milliseconds in bytes 0 to 5.
 * 2. Put the part of a millisecond, as a fraction of 4,096, in the low 12
 *    bits of bytes 6 and 7: uuids of one millisecond then still order.
 * 3. Fill bytes 8 to 15 with random bytes.
 * 4. Set the version and the variant.
 */
int ty_uuid_v7(uint64_t unix_ms, uint32_t sub_ms_ns, ty_uuid_t *u) {
  ty_uuid_t v;

  if (unix_ms >> 48 != 0 || sub_ms_ns > 999999) {
    return ty_err_range();
  }
  for (int i = 0; i < 6; i++) {
    v.data[i] = (uint8_t)(unix_ms >> (40 - 8 * i));
  }
  uint32_t frac = (uint32_t)(((uint64_t)sub_ms_ns * 4096) / 1000000);
  v.data[6] = (uint8_t)(frac >> 8);
  v.data[7] = (uint8_t)frac;
  if (!fill_random(v.data + 8, 8)) {
    return ty_internal::set_error(TY_ERR_EXCEPTION,
                                  "could not generate random values");
  }
  set_version(&v, 7);
  *u = v;
  return 0;
}
