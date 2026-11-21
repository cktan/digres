/*-------------------------------------------------------------------------
 *
 * hashfn.c
 *		Generic hashing functions, and hash functions for use in
 *dynahash.c hashtables
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/common/hashfn.c
 *
 * NOTES
 *	  It is expected that every bit of a hash function's 32-bit result is
 *	  as random as every other; failure to ensure this is likely to lead
 *	  to poor performance of hash tables.  In most cases a hash
 *	  function should use hash_bytes() or its variant hash_bytes_uint32(),
 *	  or the wrappers hash_any() and hash_uint32 defined in hashfn.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "ty_hash.h"

/*
 * The hash of a byte string and of a 32-bit value is ty's: ty holds
 * PostgreSQL's algorithm, Bob Jenkins's lookup3, which moved there from this
 * file, with its notice (ty/src/hash.cc, ty/DESIGN.md). The values are
 * unchanged. These four are what the rest of digres calls.
 */

/* The standard 32-bit hash of keylen bytes at k. */
uint32 hash_bytes(const unsigned char *k, int keylen) {
  return ty_hash_bytes_std(k, (size_t)keylen);
}

/* The 64-bit hash of keylen bytes at k, by a seed; 0 is no seed. */
uint64 hash_bytes_extended(const unsigned char *k, int keylen, uint64 seed) {
  return ty_hash_bytes(k, (size_t)keylen, seed);
}

/* The standard 32-bit hash of a 32-bit value. */
uint32 hash_bytes_uint32(uint32 k) { return ty_hash_u32_std(k); }

/* The 64-bit hash of a 32-bit value, by a seed. */
uint64 hash_bytes_uint32_extended(uint32 k, uint64 seed) {
  return ty_hash_u32(k, seed);
}

/*
 * string_hash: hash function for keys that are NUL-terminated strings.
 *
 * NOTE: this is the default hash function if none is specified.
 */
uint32 string_hash(const void *key, Size keysize) {
  /*
   * If the string exceeds keysize-1 bytes, we want to hash only that many,
   * because when it is copied into the hash table it will be truncated at
   * that length.
   */
  Size s_len = strlen((const char *)key);

  s_len = Min(s_len, keysize - 1);
  return hash_bytes((const unsigned char *)key, (int)s_len);
}

/*
 * tag_hash: hash function for fixed-size tag values
 */
uint32 tag_hash(const void *key, Size keysize) {
  return hash_bytes((const unsigned char *)key, (int)keysize);
}

/*
 * uint32_hash: hash function for keys that are uint32 or int32
 *
 * (tag_hash works for this case too, but is slower)
 */
uint32 uint32_hash(const void *key, Size keysize) {
  Assert(keysize == sizeof(uint32));
  return hash_bytes_uint32(*((const uint32 *)key));
}
