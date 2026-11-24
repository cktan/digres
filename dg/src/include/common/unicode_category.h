/*-------------------------------------------------------------------------
 *
 * unicode_category.h
 *	  Routines for determining the category of Unicode characters.
 *
 * These definitions can be used by both frontend and backend code.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/common/unicode_category.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UNICODE_CATEGORY_H
#define UNICODE_CATEGORY_H

#include "mb/pg_wchar.h"
#include "ty_unicode.h"

/*
 * The properties and the classes of a character, which are ty's
 * (ty_unicode.h).
 */
static inline bool pg_u_prop_cased(pg_wchar code) { return ty_u_cased(code); }

static inline bool pg_u_prop_case_ignorable(pg_wchar code) {
  return ty_u_case_ignorable(code);
}

static inline bool pg_u_isdigit(pg_wchar code, bool posix) {
  return ty_u_isdigit(code, posix);
}

static inline bool pg_u_isalpha(pg_wchar code) { return ty_u_isalpha(code); }

static inline bool pg_u_isalnum(pg_wchar code, bool posix) {
  return ty_u_isalnum(code, posix);
}

static inline bool pg_u_isupper(pg_wchar code) { return ty_u_isupper(code); }

static inline bool pg_u_islower(pg_wchar code) { return ty_u_islower(code); }

static inline bool pg_u_isgraph(pg_wchar code) { return ty_u_isgraph(code); }

static inline bool pg_u_isprint(pg_wchar code) { return ty_u_isprint(code); }

static inline bool pg_u_ispunct(pg_wchar code, bool posix) {
  return ty_u_ispunct(code, posix);
}

static inline bool pg_u_isspace(pg_wchar code) { return ty_u_isspace(code); }

#endif /* UNICODE_CATEGORY_H */
