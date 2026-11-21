/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_mac.h -- the operations of macaddr (6 bytes) and macaddr8 (8 bytes).
 * Written for ty from the forms and cases of PostgreSQL's mac.c and mac8.c.
 *
 * An address is its bytes, the first the most significant; two addresses
 * order as their bytes do. A call that can fail returns 0, or -1 with the
 * thread's error set (TY_ERR_INVALID for text that is no address, TY_ERR_RANGE
 * for one with an octet that does not fit a byte); one that cannot returns its
 * result.
 */
#ifndef TY_MAC_H
#define TY_MAC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"

typedef struct ty_macaddr_t {
    uint8_t b[6];
} ty_macaddr_t;

typedef struct ty_macaddr8_t {
    uint8_t b[8];
} ty_macaddr8_t;

/* The size of the buffer ty_macaddrN_format needs: with the NUL. */
#define TY_MACADDR_TEXT_SIZE 18
#define TY_MACADDR8_TEXT_SIZE 24

/*
 * Read an address from the NUL-terminated s. A macaddr is six octets of
 * hexadecimal, in any of the forms 08:00:2b:01:02:03, 08-00-2b-01-02-03,
 * 08002b:010203, 08002b-010203, 0800.2b01.0203, 0800-2b01-0203, or
 * 08002b010203; it is read as scanf's %x reads it. A macaddr8 is six or eight
 * pairs of hex digits, with one of ':', '-' or '.' between pairs, or none; a
 * six-byte form becomes eight by FF:FE in the middle.
 */
int ty_macaddr_parse(const char* s, ty_macaddr_t* r);
int ty_macaddr8_parse(const char* s, ty_macaddr8_t* r);

/* Write the address as lowercase hex pairs with ':', and a NUL. */
void ty_macaddr_format(const ty_macaddr_t* a, char out[TY_MACADDR_TEXT_SIZE]);
void ty_macaddr8_format(const ty_macaddr8_t* a,
                        char out[TY_MACADDR8_TEXT_SIZE]);

/* -1, 0 or 1; and the six predicates. */
static inline int ty_macaddr_cmp(const ty_macaddr_t* a, const ty_macaddr_t* b) {
    int c = memcmp(a->b, b->b, 6);
    return (c > 0) - (c < 0);
}
static inline int ty_macaddr8_cmp(const ty_macaddr8_t* a,
                                  const ty_macaddr8_t* b) {
    int c = memcmp(a->b, b->b, 8);
    return (c > 0) - (c < 0);
}

/* The bytes inverted, and the bytes of two AND-ed and OR-ed. */
ty_macaddr_t ty_macaddr_not(const ty_macaddr_t* a);
ty_macaddr_t ty_macaddr_and(const ty_macaddr_t* a, const ty_macaddr_t* b);
ty_macaddr_t ty_macaddr_or(const ty_macaddr_t* a, const ty_macaddr_t* b);
ty_macaddr8_t ty_macaddr8_not(const ty_macaddr8_t* a);
ty_macaddr8_t ty_macaddr8_and(const ty_macaddr8_t* a, const ty_macaddr8_t* b);
ty_macaddr8_t ty_macaddr8_or(const ty_macaddr8_t* a, const ty_macaddr8_t* b);

/* The address with all but the vendor's first 3 bytes zero. */
ty_macaddr_t ty_macaddr_trunc(const ty_macaddr_t* a);
ty_macaddr8_t ty_macaddr8_trunc(const ty_macaddr8_t* a);

/* A macaddr8 with its 7th bit set: the modified EUI-64 of an IPv6 address. */
ty_macaddr8_t ty_macaddr8_set7bit(const ty_macaddr8_t* a);

/* A macaddr as a macaddr8: FF:FE between the third byte and the fourth. */
ty_macaddr8_t ty_macaddr_to_macaddr8(const ty_macaddr_t* a);

/*
 * A macaddr8 as a macaddr: only if its 4th and 5th bytes are FF:FE;
 * otherwise TY_ERR_RANGE ("macaddr8 data out of range to convert to macaddr").
 */
int ty_macaddr8_to_macaddr(const ty_macaddr8_t* a, ty_macaddr_t* r);

/* A 64-bit hash of the bytes by a seed (PostgreSQL's; the low half, seed 0). */
uint64_t ty_macaddr_hash(const ty_macaddr_t* a, uint64_t seed);
uint64_t ty_macaddr8_hash(const ty_macaddr8_t* a, uint64_t seed);

#endif /* TY_MAC_H */
