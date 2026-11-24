/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_abbrev.h -- the abbreviated keys of a sort: the first 8 bytes of a value
 * as an unsigned integer, so that most compares are of two integers and the
 * value is read only on a tie. A key is in the machine's order, to compare
 * as a uint64_t. The rules end the keys when they do not pay. Written for ty,
 * with PostgreSQL's cases (uuid.c, and varlena.c for the C collation): the
 * same keys, the same hashes into the same estimators (ty_hll.h), and the
 * same rules, so a sort ends its keys at the row PostgreSQL's does.
 *
 * A host keeps the state, and logs what a rule decided, by its numbers.
 */
#ifndef TY_ABBREV_H
#define TY_ABBREV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ty_hash.h"
#include "ty_hll.h"

/* What a rule decided. */
enum {
  TY_ABBREV_WAIT = 0,  /* too few rows to judge: go on, nothing to log */
  TY_ABBREV_GO_ON = 1, /* the keys pay: go on */
  TY_ABBREV_ENDS = 2,  /* uuid: the keys pay at any size; stop counting */
  TY_ABBREV_ABORT = 3  /* the keys do not pay: compare the values */
};

/* 8 bytes in memory order, as an integer that compares as they do. */
static inline uint64_t ty_abbrev_order(uint64_t raw) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(raw);
#else
  return raw;
#endif
}

/* The hash of 8 bytes of a key, into an estimator: its halves joined. */
static inline uint32_t ty_abbrev_hash(uint64_t raw) {
  return ty_hash_u32_std((uint32_t)raw ^ (uint32_t)(raw >> 32));
}

/*
 * uuid: the key is the first 8 of its 16 bytes. The state counts the values,
 * and estimates their distinct keys until there are more than 100,000.
 */
typedef struct ty_abbrev_uuid_t {
  int64_t input_count; /* the values made into keys */
  int estimating;      /* the keys are still counted */
  ty_hll_t card;       /* the distinct keys */
} ty_abbrev_uuid_t;

/* Make the state: 2^10 registers, of the alloc function. */
int ty_abbrev_uuid_init(ty_abbrev_uuid_t *st);

/* The key of the uuid of 16 bytes at data; the state counts it. */
static inline uint64_t ty_abbrev_uuid(ty_abbrev_uuid_t *st,
                                      const uint8_t *data) {
  uint64_t raw;

  memcpy(&raw, data, 8);
  st->input_count++;
  if (st->estimating) {
    ty_hll_add(&st->card, ty_abbrev_hash(raw));
  }
  return ty_abbrev_order(raw);
}

/*
 * The rule, when the sort holds memtupcount rows: WAIT under 10,000 rows or
 * values, or once counting has ended; else the estimate in *card, and ENDS
 * past 100,000 distinct keys, ABORT under one key for each 2,000 values, plus
 * 0.5, or GO_ON.
 */
int ty_abbrev_uuid_abort(ty_abbrev_uuid_t *st, int memtupcount, double *card);

/*
 * Bytes in the C collation (text, varchar, bpchar without its trailing
 * spaces, and bytea): the key is the first 8 bytes, and 0 after a shorter
 * value. The state estimates the distinct keys and the distinct values, and
 * keeps the share of them the keys must reach.
 */
typedef struct ty_abbrev_bytes_t {
  ty_hll_t abbr;    /* the distinct keys */
  ty_hll_t full;    /* the distinct values */
  double prop_card; /* the share of distinct values the keys must reach */
} ty_abbrev_bytes_t;

/* Make the state: 2^10 registers each, and a share of 0.20. */
int ty_abbrev_bytes_init(ty_abbrev_bytes_t *st);

/* The first 8 bytes of the len at p, in memory order, 0 after them. */
static inline uint64_t ty_abbrev_bytes_raw(const char *p, size_t len) {
  uint64_t raw = 0;

  memcpy(&raw, p, len < 8 ? len : 8);
  return raw;
}

/*
 * Count a value of len bytes at p, whose key's 8 bytes are raw, and give the
 * key. A value is hashed by its first 128 bytes, and by its length if it is
 * longer. A host whose keys come from elsewhere (strxfrm) calls this with
 * them.
 */
static inline uint64_t ty_abbrev_bytes_count(ty_abbrev_bytes_t *st,
                                             const char *p, size_t len,
                                             uint64_t raw) {
  uint32_t hash = ty_hash_bytes_std((const uint8_t *)p, len < 128 ? len : 128);

  if (len > 128) {
    hash ^= ty_hash_u32_std((uint32_t)len);
  }
  ty_hll_add(&st->full, hash);
  ty_hll_add(&st->abbr, ty_abbrev_hash(raw));
  return ty_abbrev_order(raw);
}

/* The key of the len bytes at p, in the C collation; the state counts it. */
static inline uint64_t ty_abbrev_bytes(ty_abbrev_bytes_t *st, const char *p,
                                       size_t len) {
  return ty_abbrev_bytes_count(st, p, len, ty_abbrev_bytes_raw(p, len));
}

/*
 * The rule, when the sort holds memtupcount rows: WAIT under 100 rows; else
 * the estimates, at least 1, in *abbrev_distinct and *key_distinct, and the
 * share they were judged by in *prop_card. GO_ON if the distinct keys pass
 * the share of the distinct values, and the share falls to 0.65 of itself
 * past 10,000 rows; else ABORT.
 */
int ty_abbrev_bytes_abort(ty_abbrev_bytes_t *st, int memtupcount,
                          double *abbrev_distinct, double *key_distinct,
                          double *prop_card);

#endif /* TY_ABBREV_H */
