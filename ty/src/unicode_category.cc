/*
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * unicode_category.cc -- the general category and the properties of a
 * character (ty_unicode.h).
 *
 * Derived from PostgreSQL's src/common/unicode_category.c, with its table,
 * src/unicode/unicode_category_table.h.
 */
#include "ty_unicode.h"

namespace {

typedef uint8_t uint8;
typedef uint32_t uint32;

#include "unicode/unicode_category_table.h"

/* A mask of a category, so that a set of them is one test. */
#define MASK(X) ((uint32_t)1 << (X))

#define P_MASK                                                                 \
  (MASK(TY_U_CONNECTOR_PUNCTUATION) | MASK(TY_U_DASH_PUNCTUATION) |            \
   MASK(TY_U_OPEN_PUNCTUATION) | MASK(TY_U_CLOSE_PUNCTUATION) |                \
   MASK(TY_U_INITIAL_PUNCTUATION) | MASK(TY_U_FINAL_PUNCTUATION) |             \
   MASK(TY_U_OTHER_PUNCTUATION))
#define S_MASK                                                                 \
  (MASK(TY_U_MATH_SYMBOL) | MASK(TY_U_CURRENCY_SYMBOL) |                       \
   MASK(TY_U_MODIFIER_SYMBOL) | MASK(TY_U_OTHER_SYMBOL))

/* Whether code is in one of the table's size ranges: a binary search. */
template <size_t size>
bool range_search(const pg_unicode_range (&tbl)[size], uint32_t code) {
  int min = 0;
  int max = (int)size - 1;

  while (max >= min) {
    int mid = (min + max) / 2;

    if (code > tbl[mid].last) {
      min = mid + 1;
    } else if (code < tbl[mid].first) {
      max = mid - 1;
    } else {
      return true;
    }
  }
  return false;
}

/* Whether code has the property: by the table of ASCII, or of its ranges. */
template <size_t size>
bool has_property(const pg_unicode_range (&tbl)[size], int property,
                  uint32_t code) {
  if (code < 0x80) {
    return (unicode_opt_ascii[code].properties & property) != 0;
  }
  return range_search(tbl, code);
}

/* A lower case letter. */
bool is_lowercase(uint32_t c) {
  return has_property(unicode_lowercase, PG_U_PROP_LOWERCASE, c);
}

/* An upper case letter. */
bool is_uppercase(uint32_t c) {
  return has_property(unicode_uppercase, PG_U_PROP_UPPERCASE, c);
}

} // namespace

/* The general category: a binary search of the ranges, but for ASCII. */
int ty_u_category(uint32_t code) {
  int min = 0;
  int max = (int)(sizeof unicode_categories / sizeof unicode_categories[0]) - 1;

  if (code < 0x80) {
    return unicode_opt_ascii[code].category;
  }
  while (max >= min) {
    int mid = (min + max) / 2;

    if (code > unicode_categories[mid].last) {
      min = mid + 1;
    } else if (code < unicode_categories[mid].first) {
      max = mid - 1;
    } else {
      return unicode_categories[mid].category;
    }
  }
  return TY_U_UNASSIGNED;
}

/* Whether c has case: a title case letter, or a lower or an upper case one. */
bool ty_u_cased(uint32_t c) {
  if (c < 0x80) {
    return (unicode_opt_ascii[c].properties & PG_U_PROP_CASED) != 0;
  }
  return TY_U_TITLECASE_LETTER == ty_u_category(c) || is_lowercase(c) ||
         is_uppercase(c);
}

/* Whether c is passed over when the case of a word is found. */
bool ty_u_case_ignorable(uint32_t c) {
  return has_property(unicode_case_ignorable, PG_U_PROP_CASE_IGNORABLE, c);
}

/* A digit: 0 to 9, or a decimal number of any script. */
bool ty_u_isdigit(uint32_t c, bool posix) {
  if (posix) {
    return '0' <= c && c <= '9';
  }
  return TY_U_DECIMAL_NUMBER == ty_u_category(c);
}

/* Alphabetic. */
bool ty_u_isalpha(uint32_t c) {
  return has_property(unicode_alphabetic, PG_U_PROP_ALPHABETIC, c);
}

/* Alphabetic, or a digit. */
bool ty_u_isalnum(uint32_t c, bool posix) {
  return ty_u_isalpha(c) || ty_u_isdigit(c, posix);
}

/* An upper case letter. */
bool ty_u_isupper(uint32_t c) { return is_uppercase(c); }

/* A lower case letter. */
bool ty_u_islower(uint32_t c) { return is_lowercase(c); }

/* Seen when printed: assigned, and not space, a control or a surrogate. */
bool ty_u_isgraph(uint32_t c) {
  uint32_t mask = MASK(ty_u_category(c));

  return 0 == (mask & (MASK(TY_U_CONTROL) | MASK(TY_U_SURROGATE) |
                       MASK(TY_U_UNASSIGNED))) &&
         !ty_u_isspace(c);
}

/* Seen when printed, or a blank, but no control. */
bool ty_u_isprint(uint32_t c) {
  int category = ty_u_category(c);

  if (TY_U_CONTROL == category) {
    return false;
  }
  return ty_u_isgraph(c) || TY_U_SPACE_SEPARATOR == category;
}

/* Punctuation; with posix a symbol too, and nothing alphabetic. */
bool ty_u_ispunct(uint32_t c, bool posix) {
  if (posix) {
    return !ty_u_isalpha(c) &&
           (MASK(ty_u_category(c)) & (P_MASK | S_MASK)) != 0;
  }
  return (MASK(ty_u_category(c)) & P_MASK) != 0;
}

/* White space. */
bool ty_u_isspace(uint32_t c) {
  return has_property(unicode_white_space, PG_U_PROP_WHITE_SPACE, c);
}
