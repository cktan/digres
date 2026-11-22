/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_uuid.h -- the operations of uuid. Written for ty; the forms it reads
 * and the bits it sets are those of RFC 9562 and of PostgreSQL's uuid.
 *
 * A uuid is its 16 bytes, most significant first; two uuids order as
 * their bytes do.
 */
#ifndef TY_UUID_H
#define TY_UUID_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"

typedef struct ty_uuid_t {
  uint8_t data[16];
} ty_uuid_t;

/* The size of the buffer ty_uuid_format() needs: 36 characters and a NUL. */
#define TY_UUID_TEXT_SIZE 37

/*
 * Parse the len bytes at s: 32 hexadecimal digits, with a hyphen allowed
 * after each group of four, and the whole in braces if you like. Any case.
 * s need not end in NUL. Sets *u and returns 0; or returns -1 with
 * TY_ERR_INVALID, and *u is not set.
 */
int ty_uuid_parse(const char *s, size_t len, ty_uuid_t *u);

/* Write u as 8-4-4-4-12 lowercase digits and a NUL, to out. */
void ty_uuid_format(const ty_uuid_t *u, char out[TY_UUID_TEXT_SIZE]);

/* Negative, zero or positive as a is below, equal to or above b. */
static inline int ty_uuid_cmp(const ty_uuid_t *a, const ty_uuid_t *b) {
  return memcmp(a->data, b->data, 16);
}

/*
 * The uuid one above a, or one below it, as the 128-bit number of its bytes:
 * sets *r and returns 0. All ones has no successor, and all zeros no
 * predecessor: -1 with TY_ERR_RANGE, and *r is not set.
 */
int ty_uuid_increment(const ty_uuid_t *a, ty_uuid_t *r);
int ty_uuid_decrement(const ty_uuid_t *a, ty_uuid_t *r);

/*
 * A 64-bit hash of u by a seed; the low 32 bits with seed 0 are the
 * standard 32-bit hash. It cannot fail.
 */
uint64_t ty_uuid_hash(const ty_uuid_t *u, uint64_t seed);

/*
 * The version of u, 1 to 8 (0 to 15 as its bits say), or -1 if its variant
 * is not RFC 9562's.
 */
int ty_uuid_version(const ty_uuid_t *u);

/*
 * The time a version 1 or version 7 uuid holds, in microseconds since the
 * Unix epoch: sets *us and returns 0. For another version, or another
 * variant, it returns 1 and sets nothing.
 */
int ty_uuid_unix_us(const ty_uuid_t *u, int64_t *us);

/*
 * Make a version 4 uuid, of random bytes from the system. Returns 0, or -1
 * with TY_ERR_EXCEPTION if the system gives none.
 */
int ty_uuid_v4(ty_uuid_t *u);

/*
 * Make a version 7 uuid: unix_ms, 48 bits of the time in milliseconds
 * since the Unix epoch, then sub_ms_ns, the nanoseconds past that
 * millisecond (0 to 999,999) as a fraction in 12 bits, so uuids made in
 * one millisecond still order; the rest random. Returns 0, or -1 with
 * TY_ERR_EXCEPTION if the system gives no random bytes, or TY_ERR_RANGE
 * if unix_ms does not fit 48 bits or sub_ms_ns is over 999,999.
 */
int ty_uuid_v7(uint64_t unix_ms, uint32_t sub_ms_ns, ty_uuid_t *u);

#endif /* TY_UUID_H */
