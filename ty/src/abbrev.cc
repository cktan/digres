/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * abbrev.cc -- the states and the rules of the abbreviated keys
 * (ty_abbrev.h).
 */
#include "ty_abbrev.h"

/* Count the keys from the start. */
int ty_abbrev_uuid_init(ty_abbrev_uuid_t *st) {
  st->input_count = 0;
  st->estimating = 1;
  return ty_hll_init(&st->card, 10);
}

/*
 * Judge the uuid keys.
 * 1. Wait for 10,000 rows and values: a smaller sort gains little either way.
 *    The values' own distinct count does not matter, as a uuid compare has no
 *    fast path for equal values.
 * 2. Past 100,000 distinct keys the keys pay even for billions of rows: stop
 *    counting.
 * 3. Under one distinct key for each 2,000 values, plus 0.5, abort.
 */
int ty_abbrev_uuid_abort(ty_abbrev_uuid_t *st, int memtupcount, double *card) {
  if (memtupcount < 10000 || st->input_count < 10000 || !st->estimating) {
    return TY_ABBREV_WAIT;
  }
  *card = ty_hll_estimate(&st->card);
  if (*card > 100000.0) {
    st->estimating = 0;
    return TY_ABBREV_ENDS;
  }
  if (*card < st->input_count / 2000.0 + 0.5) {
    return TY_ABBREV_ABORT;
  }
  return TY_ABBREV_GO_ON;
}

/* Start the share at 0.20; free the first estimator if the second fails. */
int ty_abbrev_bytes_init(ty_abbrev_bytes_t *st) {
  st->prop_card = 0.20;
  if (0 != ty_hll_init(&st->abbr, 10)) {
    return -1;
  }
  if (0 != ty_hll_init(&st->full, 10)) {
    ty_hll_free(&st->abbr);
    return -1;
  }
  return 0;
}

/*
 * Judge the keys of bytes.
 * 1. Wait for 100 rows.
 * 2. Estimate the distinct keys and values, each at least 1: rows of NULLs
 *    alone add none.
 * 3. If the distinct keys pass the share of the distinct values, go on: a
 *    tie of keys is then mostly a tie of values too, which memcmp settles.
 *    Past 10,000 rows the share falls to 0.65 of itself for the next call,
 *    made when the rows have doubled, as the compares then cost more than
 *    the keys.
 * 4. Else abort.
 */
int ty_abbrev_bytes_abort(ty_abbrev_bytes_t *st, int memtupcount,
                          double *abbrev_distinct, double *key_distinct,
                          double *prop_card) {
  if (memtupcount < 100) {
    return TY_ABBREV_WAIT;
  }
  double a = ty_hll_estimate(&st->abbr);
  double k = ty_hll_estimate(&st->full);

  *abbrev_distinct = a <= 1.0 ? 1.0 : a;
  *key_distinct = k <= 1.0 ? 1.0 : k;
  *prop_card = st->prop_card;
  if (*abbrev_distinct > *key_distinct * st->prop_card) {
    if (memtupcount > 10000) {
      st->prop_card *= 0.65;
    }
    return TY_ABBREV_GO_ON;
  }
  return TY_ABBREV_ABORT;
}
