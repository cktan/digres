/*
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * ty_hash.h -- the hash of a byte string and of a 32-bit value, which every
 * type's hash in ty is built on. They are PostgreSQL's (src/common/hashfn.c,
 * Bob Jenkins's lookup3), so a hash is PostgreSQL's, bit for bit, and a
 * system that moves to ty keeps its hash partitions and its hash indexes.
 *
 * A standard hash is 32 bits. The extended hash is 64 bits, by a seed; with
 * seed 0 its low 32 bits are the standard hash. None can fail.
 */
#ifndef TY_HASH_H
#define TY_HASH_H

#include <stddef.h>
#include <stdint.h>

/* The standard hash of the n bytes at k, and the extended hash by a seed. */
uint32_t ty_hash_bytes_std(const uint8_t *k, size_t n);
uint64_t ty_hash_bytes(const uint8_t *k, size_t n, uint64_t seed);

/*
 * The standard hash of a 32-bit value, and the extended hash by a seed. The
 * standard hash is that of the 4 bytes of the value in the machine's order.
 */
uint32_t ty_hash_u32_std(uint32_t k);
uint64_t ty_hash_u32(uint32_t k, uint64_t seed);

#endif /* TY_HASH_H */
