/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_unicode.h -- Unicode's normal forms, the escapes of unistr(), and the
 * properties of a character. The tables are PostgreSQL's, so the version of
 * Unicode is too.
 *
 * A text is a varlena of valid UTF-8 (ty_text.h), and an array of characters
 * is of code points, with a 0 at its end. A call that can fail returns 0, or
 * -1 with the thread's error set.
 */
#ifndef TY_UNICODE_H
#define TY_UNICODE_H

#include <stddef.h>
#include <stdint.h>

#include "ty.h"
#include "ty_varlena.h"

/* The normal forms, with PostgreSQL's values. */
#define TY_NORM_NFC 0
#define TY_NORM_NFD 1
#define TY_NORM_NFKC 2
#define TY_NORM_NFKD 3

/*
 * The normal form that name names, in any case, in *form. Any other name is
 * TY_ERR_INVALID ("invalid normalization form").
 */
int ty_norm_form(const char *name, int *form);

/*
 * The characters of chars in the normal form, in a new array of the thread's
 * alloc function; NULL with the thread's error set if it has no memory, or
 * would be too large (TY_ERR_RANGE).
 */
uint32_t *ty_norm_chars(int form, const uint32_t *chars);

/* s in the normal form, as a new text: normalize(s, form). */
int ty_text_normalize(const ty_varlena_t *s, int form, ty_varlena_t **out);

/* Whether s is in the normal form, 1 or 0 in *r: s IS form NORMALIZED. */
int ty_text_is_normalized(const ty_varlena_t *s, int form, int *r);

/* Whether every character of s is assigned: unicode_assigned(s). */
int ty_text_is_assigned(const ty_varlena_t *s);

/* Why ty_text_unistr refused a text. */
#define TY_UNISTR_ESCAPE 1 /* not an escape */
#define TY_UNISTR_PAIR 2   /* half a pair of surrogates */
#define TY_UNISTR_POINT 3  /* not a code point: 0, or past 10FFFF */

/*
 * s with its escapes replaced, as a new text: unistr(s). An escape is
 * \XXXX, \uXXXX, \+XXXXXX or \UXXXXXXXX, in hexadecimal, or a double backslash;
 * a pair of surrogates is two escapes. A wrong one is TY_ERR_INVALID with
 * PostgreSQL's message and *why; *why is 0 for any other error.
 */
int ty_text_unistr(const ty_varlena_t *s, ty_varlena_t **out, int *why);

/* The general categories, with ICU's values. */
#define TY_U_UNASSIGNED 0             /* Cn */
#define TY_U_UPPERCASE_LETTER 1       /* Lu */
#define TY_U_LOWERCASE_LETTER 2       /* Ll */
#define TY_U_TITLECASE_LETTER 3       /* Lt */
#define TY_U_MODIFIER_LETTER 4        /* Lm */
#define TY_U_OTHER_LETTER 5           /* Lo */
#define TY_U_NONSPACING_MARK 6        /* Mn */
#define TY_U_ENCLOSING_MARK 7         /* Me */
#define TY_U_SPACING_MARK 8           /* Mc */
#define TY_U_DECIMAL_NUMBER 9         /* Nd */
#define TY_U_LETTER_NUMBER 10         /* Nl */
#define TY_U_OTHER_NUMBER 11          /* No */
#define TY_U_SPACE_SEPARATOR 12       /* Zs */
#define TY_U_LINE_SEPARATOR 13        /* Zl */
#define TY_U_PARAGRAPH_SEPARATOR 14   /* Zp */
#define TY_U_CONTROL 15               /* Cc */
#define TY_U_FORMAT 16                /* Cf */
#define TY_U_PRIVATE_USE 17           /* Co */
#define TY_U_SURROGATE 18             /* Cs */
#define TY_U_DASH_PUNCTUATION 19      /* Pd */
#define TY_U_OPEN_PUNCTUATION 20      /* Ps */
#define TY_U_CLOSE_PUNCTUATION 21     /* Pe */
#define TY_U_CONNECTOR_PUNCTUATION 22 /* Pc */
#define TY_U_OTHER_PUNCTUATION 23     /* Po */
#define TY_U_MATH_SYMBOL 24           /* Sm */
#define TY_U_CURRENCY_SYMBOL 25       /* Sc */
#define TY_U_MODIFIER_SYMBOL 26       /* Sk */
#define TY_U_OTHER_SYMBOL 27          /* So */
#define TY_U_INITIAL_PUNCTUATION 28   /* Pi */
#define TY_U_FINAL_PUNCTUATION 29     /* Pf */

/* The general category of the code point c, at most 10FFFF. */
int ty_u_category(uint32_t c);

/* Whether c has the property. */
bool ty_u_cased(uint32_t c);
bool ty_u_case_ignorable(uint32_t c);

/*
 * Whether c is of the class, by Unicode's Compatibility Properties (UTS 18);
 * posix is its "POSIX Compatible" form.
 */
bool ty_u_isdigit(uint32_t c, bool posix);
bool ty_u_isalpha(uint32_t c);
bool ty_u_isalnum(uint32_t c, bool posix);
bool ty_u_isupper(uint32_t c);
bool ty_u_islower(uint32_t c);
bool ty_u_isgraph(uint32_t c);
bool ty_u_isprint(uint32_t c);
bool ty_u_ispunct(uint32_t c, bool posix);
bool ty_u_isspace(uint32_t c);

#endif /* TY_UNICODE_H */
