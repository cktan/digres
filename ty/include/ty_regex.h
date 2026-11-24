/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_regex.h -- regular expressions of text, through the host's engine.
 * ty has no engine of its own: a host sets one, process-wide, and ty's
 * callers (sm evaluating ~ in an index expression or a check, say) reach
 * it through ty_text_regex. digres sets PostgreSQL's engine, whose
 * collations, character classes and compiled patterns stay digres's.
 */
#ifndef TY_REGEX_H
#define TY_REGEX_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_varlena.h"

/* The flags of a match. */
enum {
  TY_REGEX_ICASE = 1 /* case-insensitive: ~* rather than ~ */
};

/*
 * The host's engine: whether the slen bytes at s match the plen bytes of
 * the pattern pat, by flags, under the host's collation collation, which
 * ty passes through as it is. 0 and *matched 1 or 0; or -1, with the
 * engine's message written into errmsg, errlen bytes at most, NUL-ended.
 * Neither text need end in NUL. The engine may be called from any thread.
 */
typedef int (*ty_regex_fn)(const char *pat, size_t plen, const char *s,
                           size_t slen, int flags, uint32_t collation,
                           int *matched, char *errmsg, size_t errlen);

/*
 * Set the engine for the whole process; NULL takes it away. A host sets it
 * once, before its threads call ty; setting it while they run is safe, and
 * a call already in the old engine ends there.
 */
void ty_set_regex(ty_regex_fn fn);

/* The engine set, or NULL. */
ty_regex_fn ty_regex(void);

/*
 * ~ and ~*: whether the text t matches the pattern pat, through the engine.
 * 0 and *matched; or -1 with TY_ERR_INVALID: the engine's message, or "no
 * regular expression engine is set" with none.
 */
int ty_text_regex(const ty_varlena_t *t, const ty_varlena_t *pat, int flags,
                  uint32_t collation, int *matched);

#endif /* TY_REGEX_H */
