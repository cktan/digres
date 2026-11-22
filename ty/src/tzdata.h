/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * tzdata.h -- the time zones of ty as data: the pack that src/tzdata.cc,
 * made by tools/gen_tzdata.py, holds. The format of the pack is described at
 * the top of that script; ../TZDATA.md says how it is made. A host may give
 * ty another pack (ty_tz_use_data in ty_timezone.h), as one it mapped from a
 * file.
 */
#ifndef TY_TZDATA_H
#define TY_TZDATA_H

namespace ty_internal {

extern const unsigned char ty_tzpack[]; /* the pack of this build */
extern const unsigned ty_tzpack_size;

} // namespace ty_internal

#endif /* TY_TZDATA_H */
