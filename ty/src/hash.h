/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * hash.h -- the mix that ty's hash functions share.
 */
#ifndef TY_SRC_HASH_H
#define TY_SRC_HASH_H

#include <stdint.h>

namespace ty_internal {

/* One round of a 64-bit mix, murmur3's finalizer: h folded with w. */
inline uint64_t mix64(uint64_t h, uint64_t w) {
  h ^= w;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

} // namespace ty_internal

#endif /* TY_SRC_HASH_H */
