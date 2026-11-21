/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_inet.h -- the operations of inet and cidr. The text parse and format
 * are the ISC code PostgreSQL uses, with its notices in the files
 * inet_pton.cc, inet_ntop.cc and inet_cidr_ntop.cc; the rest follows the
 * cases of PostgreSQL's network.c.
 *
 * A value is its family, its mask length and its address, which is 4 bytes
 * for an IPv4 and 16 for an IPv6, then zero bytes up to 16. The layout is
 * what PostgreSQL stores (family 2 is IPv4, 3 is IPv6), so a stored value
 * is a ty_inet_t after its varlena header.
 */
#ifndef TY_INET_H
#define TY_INET_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty.h"

#define TY_INET_V4 2
#define TY_INET_V6 3

typedef struct ty_inet_t {
    uint8_t family;
    uint8_t bits;
    uint8_t addr[16];
} ty_inet_t;

/* The bytes of the address, and its bits. */
static inline int ty_inet_addrsize(const ty_inet_t* a) {
    return a->family == TY_INET_V4 ? 4 : 16;
}
static inline int ty_inet_maxbits(const ty_inet_t* a) {
    return a->family == TY_INET_V4 ? 32 : 128;
}

/* The longest text of a value, with its NUL. */
#define TY_INET_TEXT_MAX sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")

/*
 * Read the text of an inet, or of a cidr (is_cidr), which has no host bits.
 * A syntax error is TY_ERR_INVALID; a cidr with bits to the right of its
 * mask is TY_ERR_RANGE (PostgreSQL's "invalid cidr value").
 */
int ty_inet_parse(const char* s, int is_cidr, ty_inet_t* out);

/*
 * Write the text of a value, in out of TY_INET_TEXT_MAX bytes.
 *   TY_INET_OUT    what inet_out writes: no /bits if the whole address
 *   TY_INET_CIDR   what cidr_out writes: /bits always
 *   TY_INET_HOST   host(): the address alone
 *   TY_INET_SHOW   text(): the address and /bits always
 *   TY_INET_ABBREV abbrev() of an inet
 *   TY_INET_CIDR_ABBREV abbrev() of a cidr: trailing zero bytes dropped
 * A value that cannot be formatted is TY_ERR_INVALID (a family that is
 * none, or a mask too long).
 */
enum { TY_INET_OUT, TY_INET_CIDR, TY_INET_HOST, TY_INET_SHOW, TY_INET_ABBREV, TY_INET_CIDR_ABBREV };
int ty_inet_format(const ty_inet_t* a, int mode, char* out);

/* Whether a cidr value has no bit set to the right of its mask. */
int ty_inet_address_ok(const ty_inet_t* a);

/*
 * Compare: the common bits of the network part, then the mask length, then
 * the whole address; two families by their number.
 */
int ty_inet_cmp(const ty_inet_t* a, const ty_inet_t* b);

/* The first n bits compared: negative, zero or positive. */
int ty_inet_bitncmp(const uint8_t* l, const uint8_t* r, int n);

/* How many of the first n bits are alike in l and r. */
int ty_inet_bitncommon(const uint8_t* l, const uint8_t* r, int n);

/*
 * Network inclusion. Values of two families are never included: 0.
 * sub: a is inside b, not equal; subeq: or equal; sup and supeq the other
 * way; overlap: either holds, or they are equal.
 */
int ty_inet_sub(const ty_inet_t* a, const ty_inet_t* b);
int ty_inet_subeq(const ty_inet_t* a, const ty_inet_t* b);
int ty_inet_sup(const ty_inet_t* a, const ty_inet_t* b);
int ty_inet_supeq(const ty_inet_t* a, const ty_inet_t* b);
int ty_inet_overlap(const ty_inet_t* a, const ty_inet_t* b);

/* The family as the number a user sees: 4, 6, or 0 if it is none. */
static inline int ty_inet_family_number(const ty_inet_t* a) {
    return a->family == TY_INET_V4 ? 4 : a->family == TY_INET_V6 ? 6 : 0;
}

/*
 * The mask length of a result is set to bits and its host part cleared
 * (cidr_set_masklen); bits must be in 0..maxbits.
 */
void ty_inet_set_masklen_cidr(const ty_inet_t* a, int bits, ty_inet_t* out);

/* broadcast, network, netmask and hostmask, as PostgreSQL's functions. */
void ty_inet_broadcast(const ty_inet_t* a, ty_inet_t* out);
void ty_inet_network(const ty_inet_t* a, ty_inet_t* out);
void ty_inet_netmask(const ty_inet_t* a, ty_inet_t* out);
void ty_inet_hostmask(const ty_inet_t* a, ty_inet_t* out);

/*
 * The smallest cidr that holds both; TY_ERR_INVALID if the families are
 * different ("cannot merge addresses from different families").
 */
int ty_inet_merge(const ty_inet_t* a, const ty_inet_t* b, ty_inet_t* out);

/* ~a, a & b and a | b; AND and OR of two families are TY_ERR_INVALID. */
void ty_inet_not(const ty_inet_t* a, ty_inet_t* out);
int ty_inet_and(const ty_inet_t* a, const ty_inet_t* b, ty_inet_t* out);
int ty_inet_or(const ty_inet_t* a, const ty_inet_t* b, ty_inet_t* out);

/* a + n: TY_ERR_RANGE if it passes the address space. */
int ty_inet_add(const ty_inet_t* a, int64_t n, ty_inet_t* out);

/* a - b as an int64: TY_ERR_INVALID for two families, RANGE for too wide. */
int ty_inet_sub_inet(const ty_inet_t* a, const ty_inet_t* b, int64_t* out);

/*
 * The hash of the bytes PostgreSQL hashes: family, bits and the address
 * (addrsize + 2 bytes).
 */
uint32_t ty_inet_hash(const ty_inet_t* a);
uint64_t ty_inet_hash_extended(const ty_inet_t* a, uint64_t seed);

#endif
