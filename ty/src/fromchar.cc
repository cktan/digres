/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * fromchar.cc -- the reverse parse of a date format (ty_tochar.h): the text
 * read field by field, as PostgreSQL's DCH_from_char, then the fields made
 * into a date and time, as its do_to_timestamp.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dch.h"
#include "error.h"
#include "ty_datetime.h"
#include "ty_tochar.h"

using namespace ty_internal::dch;

namespace {

/* The fields read, as PostgreSQL's TmFromChar. */
struct tmfc_t {
  int mode;
  int hh, pm, mi, ss, ssss, d, dd, ddd, mm, ms, year, bc, ww, w, cc, j, us;
  int yysz;  /* the digits of the year: YY or YYYY */
  int clock; /* 12 for the 12-hour clock */
  int tzsign, tzh, tzm, ff;
  bool has_tz;
  int gmtoffset;      /* a fixed abbreviation's offset east */
  const ty_tz_t *tzp; /* a dynamic abbreviation's zone */
};

/* The state of a parse. */
struct parse_t {
  const ty_dch_format_t *f;
  const ty_dch_parse_host_t *host;
  ty_dch_error_t *err;
  char abbrev[64]; /* a dynamic abbreviation, set only with tmfc_t's tzp */
};

const char *const k_ampm[] = {"am", "pm", "AM", "PM", nullptr};
const char *const k_ampm_long[] = {"a.m.", "p.m.", "A.M.", "P.M.", nullptr};
const char *const k_adbc[] = {"ad", "bc", "AD", "BC", nullptr};
const char *const k_adbc_long[] = {"a.d.", "b.c.", "A.D.", "B.C.", nullptr};
const char *const k_months_full[] = {
    "January", "February",  "March",   "April",    "May",      "June", "July",
    "August",  "September", "October", "November", "December", nullptr};
const char *const k_months[] = {"Jan", "Feb", "Mar",  "Apr", "May",
                                "Jun", "Jul", "Aug",  "Sep", "Oct",
                                "Nov", "Dec", nullptr};
const char *const k_days[] = {"Sunday",   "Monday", "Tuesday",  "Wednesday",
                              "Thursday", "Friday", "Saturday", nullptr};
const char *const k_days_short[] = {"Sun", "Mon", "Tue", "Wed",
                                    "Thu", "Fri", "Sat", nullptr};
const char *const k_rm_lower[] = {"xii", "xi", "x",    "ix", "viii",
                                  "vii", "vi", "v",    "iv", "iii",
                                  "ii",  "i",  nullptr};

/* The bytes of the UTF-8 character at s, 1 at the end. */
inline int mblen_at(const char *s) {
  unsigned char b = (unsigned char)*s;

  return b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : 4;
}

/* PostgreSQL's scanner_isspace: the spaces of SQL. */
inline bool scanner_isspace(char c) {
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c || '\f' == c ||
         '\v' == c;
}

/* Whether the character at s is a separator: printable, not a letter or a
 * digit. */
inline bool is_separator(const char *s) {
  char c = *s;

  return c > 0x20 && c < 0x7f && !(c >= 'A' && c <= 'Z') &&
         !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9');
}

/* The name of a node's keyword, as the errors give it. */
inline const char *name_of(const node_t *n) { return k_keywords[n->kw].name; }

/*
 * Fail with code, ty's message, and the detail and hint, each a printf
 * format or NULL; returns -1.
 */
int fail(parse_t *p, int code, const char *detail, const char *hint,
         const char *fmt, ...) {
  char msg[512];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  p->err->code = code;
  p->err->dterr = 0;
  snprintf(p->err->detail, sizeof p->err->detail, "%s", detail ? detail : "");
  snprintf(p->err->hint, sizeof p->err->hint, "%s", hint ? hint : "");
  return ty_internal::set_error(
      TY_DCH_E_FORMAT == code ? TY_ERR_INVALID : TY_ERR_RANGE, msg);
}

/* DateTimeParseError's error, of dterr; the host writes its message. */
int fail_dterr(parse_t *p, int dterr) {
  p->err->code = TY_DCH_E_DTERR;
  p->err->dterr = dterr;
  p->err->detail[0] = '\0';
  p->err->hint[0] = '\0';
  return ty_internal::set_error(TY_ERR_RANGE,
                                "date/time field value out of range");
}

/* Set the date convention, which a format may not mix. */
int set_mode(parse_t *p, tmfc_t *t, int mode) {
  if (M_NONE != mode) {
    if (M_NONE == t->mode) {
      t->mode = mode;
    } else if (t->mode != mode) {
      return fail(p, TY_DCH_E_FORMAT, nullptr,
                  "Do not mix Gregorian and ISO week date conventions "
                  "in a formatting template.",
                  "invalid combination of date conventions");
    }
  }
  return 0;
}

/* Set a field, which a second value may not contradict. */
int set_int(parse_t *p, int *dest, int value, const node_t *n) {
  if (0 != *dest && *dest != value) {
    return fail(p, TY_DCH_E_FORMAT,
                "This value contradicts a previous setting for the same "
                "field type.",
                nullptr,
                "conflicting values for \"%s\" field in formatting string",
                name_of(n));
  }
  *dest = value;
  return 0;
}

/*
 * Whether the node after a numeric field is not a digit, so that the field
 * may take every digit it finds: a TH suffix, the end, a keyword not of
 * digits, or a character not a digit.
 */
bool next_is_separator(const parse_t *p, size_t k) {
  const node_t *n = &p->f->node[k];

  if (n->is_key && (n->suffix & (S_TH | S_th))) {
    return true;
  }
  if (k + 1 >= p->f->n) {
    return true;
  }
  const node_t *next = &p->f->node[k + 1];

  if (next->is_key) {
    return !k_keywords[next->kw].is_digit;
  }
  return !(1 == next->len && isdigit((unsigned char)next->ch[0]));
}

/*
 * Read a number of len characters at *src into *dest (NULL to drop it), as
 * PostgreSQL's from_char_parse_int_len; returns the bytes read, or -1.
 * 1. Skip the spaces before it.
 * 2. With FM, or a separator next, read every digit strtol takes, from
 *    before the spaces.
 * 3. Else read exactly len characters, all of them digits.
 * 4. A number must have been read, and fit an int.
 */
int parse_int_len(parse_t *p, int *dest, const char **src, int len, size_t k) {
  const node_t *n = &p->f->node[k];
  const char *init = *src;
  char copy[13];
  long result;

  // 1. Skip the spaces before it.
  while (**src && isspace((unsigned char)**src)) {
    (*src)++;
  }
  /* the first len characters, as strlcpy copies them; used is all there are */
  int used = (int)strlen(*src);
  int ncopy = used < len ? used : len;

  memcpy(copy, *src, (size_t)ncopy);
  copy[ncopy] = '\0';
  if ((n->suffix & S_FM) || next_is_separator(p, k)) {
    // 2. With FM, or a separator next, read every digit strtol takes.
    char *endptr;

    errno = 0;
    result = strtol(init, &endptr, 10);
    *src = endptr;
  } else {
    // 3. Else read exactly len characters, all of them digits.
    char *last;

    if (used < len) {
      char detail[96];

      snprintf(detail, sizeof detail,
               "Field requires %d characters, but only %d remain.", len, used);
      return fail(p, TY_DCH_E_FORMAT, detail,
                  "If your source string is not fixed-width, try using the "
                  "\"FM\" modifier.",
                  "source string too short for \"%s\" formatting field",
                  name_of(n));
    }
    errno = 0;
    result = strtol(copy, &last, 10);
    used = (int)(last - copy);
    if (used > 0 && used < len) {
      char detail[96];

      snprintf(detail, sizeof detail,
               "Field requires %d characters, but only %d could be parsed.",
               len, used);
      return fail(p, TY_DCH_E_FORMAT, detail,
                  "If your source string is not fixed-width, try using the "
                  "\"FM\" modifier.",
                  "invalid value \"%s\" for \"%s\"", copy, name_of(n));
    }
    *src += used;
  }

  // 4. A number must have been read, and fit an int.
  if (*src == init) {
    return fail(p, TY_DCH_E_FORMAT, "Value must be an integer.", nullptr,
                "invalid value \"%s\" for \"%s\"", copy, name_of(n));
  }
  if (ERANGE == errno || result < INT_MIN || result > INT_MAX) {
    char detail[96];

    snprintf(detail, sizeof detail, "Value must be in the range %d to %d.",
             INT_MIN, INT_MAX);
    return fail(p, TY_DCH_E_RANGE, detail, nullptr,
                "value for \"%s\" in source string is out of range",
                name_of(n));
  }
  if (nullptr != dest && 0 != set_int(p, dest, (int)result, n)) {
    return -1;
  }
  return (int)(*src - init);
}

/* A number as long as its keyword. */
inline int parse_int(parse_t *p, int *dest, const char **src, size_t k) {
  return parse_int_len(p, dest, src, p->f->node[k].len, k);
}

/* The ASCII lower case of c. */
inline unsigned char lower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c - 'A' + 'a') : c;
}

/* The word of array that name begins with, in any case: its place and *len,
 * or -1. */
int seq_search_ascii(const char *name, const char *const *array, size_t *len) {
  *len = 0;
  if (!*name) {
    return -1;
  }
  unsigned char first = lower((unsigned char)*name);

  for (const char *const *a = array; *a; a++) {
    if (lower((unsigned char)**a) != first) {
      continue;
    }
    for (const char *q = *a + 1, *m = name + 1;; q++, m++) {
      if ('\0' == *q) {
        *len = (size_t)(m - name);
        return (int)(a - array);
      }
      if ('\0' == *m || lower((unsigned char)*q) != lower((unsigned char)*m)) {
        break;
      }
    }
  }
  return -1;
}

/*
 * Match a word of array at *src, or with TM the host's localized names of
 * kind: its place in *dest, and *src after it; or the error, with the text
 * up to its first space.
 */
int seq_search(parse_t *p, int *dest, const char **src,
               const char *const *array, int kind, bool localized, size_t k) {
  size_t len = 0;

  if (localized && nullptr != p->host && nullptr != p->host->match_name) {
    int index;

    *dest = 0 == p->host->match_name(p->host->ctx, kind, *src, &index, &len)
                ? index
                : -1;
    if (*dest < 0) {
      len = 0;
    }
  } else {
    *dest = seq_search_ascii(*src, array, &len);
  }
  if (len <= 0) {
    char copy[256];
    size_t i = 0;

    for (; (*src)[i] && !scanner_isspace((*src)[i]) && i + 1 < sizeof copy;
         i++) {
      copy[i] = (*src)[i];
    }
    copy[i] = '\0';
    return fail(p, TY_DCH_E_FORMAT,
                "The given value did not match any of the allowed values for "
                "this field.",
                nullptr, "invalid value \"%s\" for \"%s\"", copy,
                name_of(&p->f->node[k]));
  }
  *src += len;
  return 0;
}

/* Skip a TH suffix's two characters, if the node has one. */
inline void skip_th(const char **s, const node_t *n) {
  if (n->suffix & (S_TH | S_th)) {
    if (**s) {
      *s += mblen_at(*s);
    }
    if (**s) {
      *s += mblen_at(*s);
    }
  }
}

/* PostgreSQL's adjust_partial_year_to_2020: a year of fewer digits than 4. */
int adjust_partial_year(int year) {
  return year < 70     ? year + 2000
         : year < 100  ? year + 1900
         : year < 520  ? year + 2000
         : year < 1000 ? year + 1000
                       : year;
}

/* The sign of a zone's offset, and the hours, as TZH and OF read them. */
int parse_tzh(parse_t *p, tmfc_t *t, const char **s, int extra_skip, size_t k) {
  /* a - before the hours may have been skipped as a separator */
  if ('+' == **s || '-' == **s || ' ' == **s) {
    t->tzsign = '-' == **s ? -1 : +1;
    (*s)++;
  } else {
    t->tzsign = extra_skip > 0 && '-' == *(*s - 1) ? -1 : +1;
  }
  return parse_int_len(p, &t->tzh, s, 2, k) < 0 ? -1 : 0;
}

/*
 * Read the text by the format into t, as PostgreSQL's DCH_from_char.
 * 1. Outside FX, skip the spaces at the start and before each field.
 * 2. A space or a separator of the format takes one of the text, or none
 *    outside FX; in the standard mode it must match.
 * 3. A character of the format takes one of the text, unless spaces were
 *    skipped there; in the standard mode it must match.
 * 4. A keyword reads its field.
 * 5. Outside FX, skip the spaces after each field.
 * 6. In the standard mode, the format must be read whole, and nothing but
 *    spaces may remain of the text.
 */
int from_char(parse_t *p, const char *in, bool std, tmfc_t *t) {
  const ty_dch_format_t *f = p->f;
  const char *s = in;
  bool fx_mode = std;
  int extra_skip = 0, value, len;
  size_t k;

  for (k = 0; k < f->n && '\0' != *s; k++) {
    const node_t *n = &f->node[k];

    // 1. Outside FX, skip the spaces at the start and before each field.
    if (!fx_mode && (!n->is_key || K_FX != n->id) && (n->is_key || 0 == k)) {
      while ('\0' != *s && isspace((unsigned char)*s)) {
        s++;
        extra_skip++;
      }
    }

    // 2. A space or a separator of the format.
    if (!n->is_key && (T_SPACE == n->type || T_SEPARATOR == n->type)) {
      if (std) {
        if (*s == n->ch[0]) {
          s++;
        } else {
          return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                      "unmatched format separator \"%c\"", n->ch[0]);
        }
      } else if (!fx_mode) {
        extra_skip--;
        if (isspace((unsigned char)*s) || is_separator(s)) {
          s++;
          extra_skip++;
        }
      } else {
        s += mblen_at(s);
      }
      continue;
    }

    // 3. A character of the format.
    if (!n->is_key) {
      if (!fx_mode) {
        if (extra_skip > 0) {
          extra_skip--;
        } else {
          s += mblen_at(s);
        }
      } else {
        int chlen = mblen_at(s);

        if (std && T_CHAR == n->type && 0 != strncmp(s, n->ch, (size_t)chlen)) {
          return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                      "unmatched format character \"%.*s\"", (int)n->len,
                      n->ch);
        }
        s += chlen;
      }
      continue;
    }

    // 4. A keyword reads its field.
    if (0 != set_mode(p, t, k_keywords[n->kw].mode)) {
      return -1;
    }
    bool tm = 0 != (n->suffix & S_TM);

    switch (n->id) {
    case K_FX:
      fx_mode = true;
      break;
    case K_A_M:
    case K_P_M:
    case K_a_m:
    case K_p_m:
      if (0 != seq_search(p, &value, &s, k_ampm_long, 0, false, k) ||
          0 != set_int(p, &t->pm, value % 2, n)) {
        return -1;
      }
      t->clock = 12;
      break;
    case K_AM:
    case K_PM:
    case K_am:
    case K_pm:
      if (0 != seq_search(p, &value, &s, k_ampm, 0, false, k) ||
          0 != set_int(p, &t->pm, value % 2, n)) {
        return -1;
      }
      t->clock = 12;
      break;
    case K_HH:
    case K_HH12:
      if (parse_int_len(p, &t->hh, &s, 2, k) < 0) {
        return -1;
      }
      t->clock = 12;
      skip_th(&s, n);
      break;
    case K_HH24:
      if (parse_int_len(p, &t->hh, &s, 2, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_MI:
      if (parse_int(p, &t->mi, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_SS:
      if (parse_int(p, &t->ss, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_MS:
      if ((len = parse_int_len(p, &t->ms, &s, 3, k)) < 0) {
        return -1;
      }
      /* 25 is 0.25 and 250 is 0.25 too; 025 is 0.025 */
      t->ms *= 1 == len ? 100 : 2 == len ? 10 : 1;
      skip_th(&s, n);
      break;
    case K_FF1:
    case K_FF2:
    case K_FF3:
    case K_FF4:
    case K_FF5:
    case K_FF6:
    case K_US:
      if (K_US != n->id) {
        t->ff = n->id - K_FF1 + 1;
      }
      if ((len = parse_int_len(p, &t->us, &s, K_US == n->id ? 6 : t->ff, k)) <
          0) {
        return -1;
      }
      t->us *= 1 == len   ? 100000
               : 2 == len ? 10000
               : 3 == len ? 1000
               : 4 == len ? 100
               : 5 == len ? 10
                          : 1;
      skip_th(&s, n);
      break;
    case K_SSSS:
      if (parse_int(p, &t->ssss, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_tz:
    case K_TZ: {
      int gmtoff = 0;
      const ty_tz_t *tzp = nullptr;
      int tzlen = nullptr != p->host && nullptr != p->host->tz_abbrev
                      ? p->host->tz_abbrev(p->host->ctx, s, &gmtoff, &tzp)
                      : 0;

      if (tzlen > 0) {
        t->has_tz = true;
        t->gmtoffset = gmtoff;
        t->tzp = tzp;
        /* the abbreviation is needed for a dynamic one */
        if (nullptr != tzp) {
          snprintf(p->abbrev, sizeof p->abbrev, "%.*s", tzlen, s);
        }
        t->tzsign = 0; /* drop TZH and TZM before it */
        s += tzlen;
        break;
      }
      if (isalpha((unsigned char)*s)) {
        /* a misspelled abbreviation: OF cannot read it either */
        return fail(p, TY_DCH_E_FORMAT,
                    "Time zone abbreviation is not recognized.", nullptr,
                    "invalid value \"%s\" for \"%s\"", s, name_of(n));
      }
    }
      /* else as OF */
      [[fallthrough]];
    case K_OF:
      /* TZH, or TZH:TZM */
      if (0 != parse_tzh(p, t, &s, extra_skip, k)) {
        return -1;
      }
      if (':' == *s) {
        s++;
        if (parse_int_len(p, &t->tzm, &s, 2, k) < 0) {
          return -1;
        }
      }
      break;
    case K_TZH:
      if (0 != parse_tzh(p, t, &s, extra_skip, k)) {
        return -1;
      }
      break;
    case K_TZM:
      /* + if TZH was not seen */
      if (!t->tzsign) {
        t->tzsign = +1;
      }
      if (parse_int_len(p, &t->tzm, &s, 2, k) < 0) {
        return -1;
      }
      break;
    case K_A_D:
    case K_B_C:
    case K_a_d:
    case K_b_c:
      if (0 != seq_search(p, &value, &s, k_adbc_long, 0, false, k) ||
          0 != set_int(p, &t->bc, value % 2, n)) {
        return -1;
      }
      break;
    case K_AD:
    case K_BC:
    case K_ad:
    case K_bc:
      if (0 != seq_search(p, &value, &s, k_adbc, 0, false, k) ||
          0 != set_int(p, &t->bc, value % 2, n)) {
        return -1;
      }
      break;
    case K_MONTH:
    case K_Month:
    case K_month:
      if (0 != seq_search(p, &value, &s, k_months_full, TY_TOCHAR_FULL_MONTH,
                          tm, k) ||
          0 != set_int(p, &t->mm, value + 1, n)) {
        return -1;
      }
      break;
    case K_MON:
    case K_Mon:
    case K_mon:
      if (0 != seq_search(p, &value, &s, k_months, TY_TOCHAR_ABBREV_MONTH, tm,
                          k) ||
          0 != set_int(p, &t->mm, value + 1, n)) {
        return -1;
      }
      break;
    case K_MM:
      if (parse_int(p, &t->mm, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_DAY:
    case K_Day:
    case K_day:
      if (0 != seq_search(p, &value, &s, k_days, TY_TOCHAR_FULL_DAY, tm, k) ||
          0 != set_int(p, &t->d, value, n)) {
        return -1;
      }
      t->d++;
      break;
    case K_DY:
    case K_Dy:
    case K_dy:
      if (0 != seq_search(p, &value, &s, k_days_short, TY_TOCHAR_ABBREV_DAY, tm,
                          k) ||
          0 != set_int(p, &t->d, value, n)) {
        return -1;
      }
      t->d++;
      break;
    case K_DDD:
      if (parse_int(p, &t->ddd, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_IDDD:
      if (parse_int_len(p, &t->ddd, &s, 3, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_DD:
      if (parse_int(p, &t->dd, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_D:
      if (parse_int(p, &t->d, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_ID:
      if (parse_int_len(p, &t->d, &s, 1, k) < 0) {
        return -1;
      }
      /* Sunday is 1, as Gregorian */
      if (++t->d > 7) {
        t->d = 1;
      }
      skip_th(&s, n);
      break;
    case K_WW:
    case K_IW:
      if (parse_int(p, &t->ww, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_Q:
      /* read, and not used: which day of the quarter is unclear */
      if (parse_int(p, nullptr, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_CC:
      if (parse_int(p, &t->cc, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_Y_YYY: {
      int years, millennia, nch;

      if (sscanf(s, "%d,%03d%n", &millennia, &years, &nch) < 2) {
        return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                    "invalid value \"%s\" for \"%s\"", s, "Y,YYY");
      }
      if (__builtin_mul_overflow(millennia, 1000, &millennia) ||
          __builtin_add_overflow(years, millennia, &years)) {
        return fail(p, TY_DCH_E_FIELD, nullptr, nullptr,
                    "value for \"%s\" in source string is out of range",
                    "Y,YYY");
      }
      if (0 != set_int(p, &t->year, years, n)) {
        return -1;
      }
      t->yysz = 4;
      s += nch;
      skip_th(&s, n);
      break;
    }
    case K_YYYY:
    case K_IYYY:
      if (parse_int(p, &t->year, &s, k) < 0) {
        return -1;
      }
      t->yysz = 4;
      skip_th(&s, n);
      break;
    case K_YYY:
    case K_IYY:
    case K_YY:
    case K_IY:
    case K_Y:
    case K_I:
      if ((len = parse_int(p, &t->year, &s, k)) < 0) {
        return -1;
      }
      if (len < 4) {
        t->year = adjust_partial_year(t->year);
      }
      t->yysz = K_YYY == n->id || K_IYY == n->id ? 3
                : K_YY == n->id || K_IY == n->id ? 2
                                                 : 1;
      skip_th(&s, n);
      break;
    case K_RM:
    case K_rm:
      if (0 != seq_search(p, &value, &s, k_rm_lower, 0, false, k) ||
          0 != set_int(p, &t->mm, 12 - value, n)) {
        return -1;
      }
      break;
    case K_W:
      if (parse_int(p, &t->w, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    case K_J:
      if (parse_int(p, &t->j, &s, k) < 0) {
        return -1;
      }
      skip_th(&s, n);
      break;
    }

    // 5. Outside FX, skip the spaces after each field.
    if (!fx_mode) {
      extra_skip = 0;
      while ('\0' != *s && isspace((unsigned char)*s)) {
        s++;
        extra_skip++;
      }
    }
  }

  // 6. In the standard mode, the format read whole and nothing left.
  if (std) {
    if (k < f->n) {
      return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                  "input string is too short for datetime format");
    }
    while ('\0' != *s && isspace((unsigned char)*s)) {
      s++;
    }
    if ('\0' != *s) {
      return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                  "trailing characters remain in input string after datetime "
                  "format");
    }
  }
  return 0;
}

/* The days before each month, in a common and a leap year. */
const int k_ysum[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366}};

/* The days of each month, in a common and a leap year. */
const int k_day_tab[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                              {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

/* The masks of PostgreSQL's ValidateDate. */
enum { F_YEAR = 1, F_MONTH = 2, F_DAY = 4, F_DATE = 7 };

/* PostgreSQL's ValidateDate of a Julian year: the month and the day. */
bool date_valid(int fmask, const ty_tm_t *tm) {
  if ((fmask & F_MONTH) && (tm->mon < 1 || tm->mon > 12)) {
    return false;
  }
  if ((fmask & F_DAY) && (tm->mday < 1 || tm->mday > 31)) {
    return false;
  }
  if (F_DATE == (fmask & F_DATE) &&
      tm->mday > k_day_tab[ty_isleap(tm->year)][tm->mon - 1]) {
    return false;
  }
  return true;
}

/*
 * Make the fields read into a date and time, as PostgreSQL's
 * do_to_timestamp.
 * 1. The time: SSSS, then SS, MI and HH; the 12-hour clock to the 24.
 * 2. The year: YY in the century CC, or the year, BC as negative; or CC's
 *    first year.
 * 3. J, then ISO week and day, or a week as days; W as days.
 * 4. The day and the month, then the day of the year for those not given.
 * 5. The fraction: MS, then US.
 * 6. Check the date and the time.
 * 7. The zone: TZH and TZM, or TZ's abbreviation at the date read.
 */
int to_timestamp(parse_t *p, tmfc_t *t, ty_dch_parsed_t *out) {
  ty_tm_t *tm = &out->tm;
  int32_t *fsec = &out->fsec;
  int fmask = 0;

  // 1. The time.
  if (t->ssss) {
    int x = t->ssss;

    tm->hour = x / 3600;
    x %= 3600;
    tm->min = x / 60;
    tm->sec = x % 60;
  }
  if (t->ss) {
    tm->sec = t->ss;
  }
  if (t->mi) {
    tm->min = t->mi;
  }
  if (t->hh) {
    tm->hour = t->hh;
  }
  if (12 == t->clock) {
    if (tm->hour < 1 || tm->hour > 12) {
      return fail(p, TY_DCH_E_FORMAT, nullptr,
                  "Use the 24-hour clock, or give an hour between 1 and 12.",
                  "hour \"%d\" is invalid for the 12-hour clock", tm->hour);
    }
    if (t->pm && tm->hour < 12) {
      tm->hour += 12;
    } else if (!t->pm && 12 == tm->hour) {
      tm->hour = 0;
    }
  }

  // 2. The year.
  if (t->year) {
    /* the 21st century is 2001 to 2100; the 6th BC is 600 BC to 501 BC */
    if (t->cc && t->yysz <= 2) {
      if (t->bc) {
        t->cc = -t->cc;
      }
      tm->year = t->year % 100;
      if (tm->year) {
        int tmp;

        if (t->cc >= 0) {
          if (__builtin_mul_overflow(t->cc - 1, 100, &tmp) ||
              __builtin_add_overflow(tm->year, tmp, &tm->year)) {
            return fail_dterr(p, -2);
          }
        } else if (__builtin_mul_overflow(t->cc + 1, 100, &tmp) ||
                   __builtin_sub_overflow(tmp, tm->year, &tmp) ||
                   __builtin_add_overflow(tmp, 1, &tm->year)) {
          return fail_dterr(p, -2);
        }
      } else {
        /* the century's year that ends in 00 */
        tm->year = t->cc * 100 + (t->cc >= 0 ? 0 : 1);
      }
    } else {
      /* a year of 4 digits wins over CC */
      tm->year = t->year;
      if (t->bc) {
        tm->year = -tm->year;
      }
      if (tm->year < 0) {
        tm->year++;
      }
    }
    fmask |= F_YEAR;
  } else if (t->cc) {
    /* the century's first year */
    if (t->bc) {
      t->cc = -t->cc;
    }
    if (t->cc >= 0 ? __builtin_mul_overflow(t->cc - 1, 100, &tm->year) ||
                         __builtin_add_overflow(tm->year, 1, &tm->year)
                   : __builtin_mul_overflow(t->cc, 100, &tm->year) ||
                         __builtin_add_overflow(tm->year, 1, &tm->year)) {
      return fail_dterr(p, -2);
    }
    fmask |= F_YEAR;
  }

  // 3. J, then a week.
  if (t->j) {
    ty_j2date(t->j, &tm->year, &tm->mon, &tm->mday);
    fmask |= F_DATE;
  }
  if (t->ww) {
    if (M_ISOWEEK == t->mode) {
      /* with no day, the week's Monday */
      if (t->d) {
        int jday = ty_isoweek2j(tm->year, t->ww);

        /* the Gregorian week starts on Sunday, 1; the ISO on Monday */
        jday += t->d > 1 ? t->d - 2 : 6;
        ty_j2date(jday, &tm->year, &tm->mon, &tm->mday);
      } else {
        ty_isoweek2date(t->ww, &tm->year, &tm->mon, &tm->mday);
      }
      fmask |= F_DATE;
    } else if (__builtin_sub_overflow(t->ww, 1, &t->ddd) ||
               __builtin_mul_overflow(t->ddd, 7, &t->ddd) ||
               __builtin_add_overflow(t->ddd, 1, &t->ddd)) {
      return fail_dterr(p, -2);
    }
  }
  if (t->w && (__builtin_sub_overflow(t->w, 1, &t->dd) ||
               __builtin_mul_overflow(t->dd, 7, &t->dd) ||
               __builtin_add_overflow(t->dd, 1, &t->dd))) {
    return fail_dterr(p, -2);
  }

  // 4. The day and the month, then the day of the year.
  if (t->dd) {
    tm->mday = t->dd;
    fmask |= F_DAY;
  }
  if (t->mm) {
    tm->mon = t->mm;
    fmask |= F_MONTH;
  }
  if (t->ddd && (tm->mon <= 1 || tm->mday <= 1)) {
    if (!tm->year && !t->bc) {
      return fail(p, TY_DCH_E_FORMAT, nullptr, nullptr,
                  "cannot calculate day of year without year information");
    }
    if (M_ISOWEEK == t->mode) {
      /* the ISO year's day 0 */
      int j0 = ty_isoweek2j(tm->year, 1) - 1;

      ty_j2date(j0 + t->ddd, &tm->year, &tm->mon, &tm->mday);
      fmask |= F_DATE;
    } else {
      const int *y = k_ysum[ty_isleap(tm->year)];
      int i;

      for (i = 1; i <= 12; i++) {
        if (t->ddd <= y[i]) {
          break;
        }
      }
      if (tm->mon <= 1) {
        tm->mon = i;
      }
      if (tm->mday <= 1) {
        tm->mday = t->ddd - y[i - 1];
      }
      fmask |= F_MONTH | F_DAY;
    }
  }

  // 5. The fraction.
  if (t->ms) {
    int tmp;

    if (__builtin_mul_overflow(t->ms, 1000, &tmp) ||
        __builtin_add_overflow(*fsec, tmp, fsec)) {
      return fail_dterr(p, -2);
    }
  }
  if (t->us) {
    *fsec += t->us;
  }
  out->fprec = t->ff;

  // 6. Check the date and the time.
  if ((0 != fmask && !date_valid(fmask, tm)) || tm->hour < 0 ||
      tm->hour >= 24 || tm->min < 0 || tm->min >= 60 || tm->sec < 0 ||
      tm->sec >= 60 || *fsec < 0 || *fsec >= 1000000) {
    return fail_dterr(p, -2);
  }

  // 7. The zone.
  if (t->tzsign) {
    if (t->tzh < 0 || t->tzh > 15 || t->tzm < 0 || t->tzm >= 60) {
      return fail_dterr(p, -5);
    }
    out->has_tz = 1;
    out->gmtoffset = (t->tzh * 60 + t->tzm) * 60;
    /* west of Greenwich */
    if (t->tzsign > 0) {
      out->gmtoffset = -out->gmtoffset;
    }
  } else if (t->has_tz) {
    out->has_tz = 1;
    if (nullptr == t->tzp) {
      out->gmtoffset = -t->gmtoffset;
    } else {
      int isdst;

      /* a dynamic abbreviation: its offset at the time read */
      out->gmtoffset = ty_tz_abbrev_offset(t->tzp, tm, p->abbrev, &isdst);
    }
  }
  return 0;
}

} // namespace

/* What the format holds: a date, a time, a zone (DCH_datetime_type). */
unsigned ty_dch_flags(const ty_dch_format_t *f) {
  unsigned flags = 0;

  for (size_t k = 0; k < f->n; k++) {
    const node_t *n = &f->node[k];

    if (!n->is_key) {
      continue;
    }
    switch (n->id) {
    case K_A_M:
    case K_P_M:
    case K_a_m:
    case K_p_m:
    case K_AM:
    case K_PM:
    case K_am:
    case K_pm:
    case K_HH:
    case K_HH12:
    case K_HH24:
    case K_MI:
    case K_SS:
    case K_MS:
    case K_US:
    case K_FF1:
    case K_FF2:
    case K_FF3:
    case K_FF4:
    case K_FF5:
    case K_FF6:
    case K_SSSS:
      flags |= TY_DCH_TIMED;
      break;
    case K_tz:
    case K_TZ:
    case K_OF:
    case K_TZH:
    case K_TZM:
      flags |= TY_DCH_ZONED;
      break;
    case K_FX:
    case K_IW:
      /* IW is neither, as PostgreSQL has it */
      break;
    default:
      flags |= TY_DCH_DATED;
      break;
    }
  }
  return flags;
}

/*
 * The reverse parse: an empty format reads nothing, and gives January 1 of
 * year 0; else the fields read, made a date and time.
 */
int ty_dch_parse(const ty_dch_format_t *f, const char *in, int std,
                 const ty_dch_parse_host_t *host, ty_dch_parsed_t *out,
                 ty_dch_error_t *err) {
  parse_t p;
  tmfc_t t;

  /* the error is written only on a failure */
  p.f = f;
  p.host = host;
  p.err = err;
  err->code = 0;
  memset(&t, 0, sizeof t);
  memset(out, 0, sizeof *out);
  out->tm.mon = 1;
  out->tm.mday = 1;
  if (0 != f->len && 0 != from_char(&p, in, 0 != std, &t)) {
    return -1;
  }
  return to_timestamp(&p, &t, out);
}

namespace {

/* PostgreSQL's IS_VALID_JULIAN: 4714-11-24 BC to 5874898-06-01. */
inline bool valid_julian(int y, int m) {
  return (y > -4713 || (-4713 == y && m >= 11)) &&
         (y < 5874898 || (5874898 == y && m < 6));
}

} // namespace

/*
 * A timestamp of the fields, as tm2timestamp with a zone.
 * 1. The zone's offset: the text's, or the session's at that local time.
 * 2. The Julian day and the time, in microseconds, with no overflow.
 * 3. Move to UTC by the offset, then check the range.
 * 4. Round to FFn's precision.
 */
int ty_dch_to_timestamp(const ty_dch_parsed_t *p, const ty_tz_t *tz,
                        int64_t *out) {
  const ty_tm_t *tm = &p->tm;
  int isdst;

  // 1. The zone's offset.
  int west = p->has_tz ? p->gmtoffset
                       : ty_tz_offset_for_local(tz, tm, &isdst, nullptr);

  // 2. The Julian day and the time.
  int64_t r;

  if (!valid_julian(tm->year, tm->mon)) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  int64_t date =
      ty_date2j(tm->year, tm->mon, tm->mday) - TY_POSTGRES_EPOCH_JDATE;
  int64_t time =
      (((int64_t)tm->hour * 60 + tm->min) * 60 + tm->sec) * 1000000 + p->fsec;

  if (__builtin_mul_overflow(date, (int64_t)86400000000, &r) ||
      __builtin_add_overflow(r, time, &r)) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }

  // 3. Move to UTC by the offset, then check the range.
  r += (int64_t)west * 1000000;
  if (!ty_timestamp_valid(r)) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }

  // 4. Round to FFn's precision.
  if (p->fprec && 0 != ty_timestamp_adjust_typmod(&r, p->fprec)) {
    return -1;
  }
  *out = r;
  return 0;
}

/* A date of the fields, in the Julian range and a date's. */
int ty_dch_to_date(const ty_dch_parsed_t *p, int32_t *out) {
  const ty_tm_t *tm = &p->tm;

  if (!valid_julian(tm->year, tm->mon)) {
    return ty_internal::set_error(TY_ERR_RANGE, "date out of range");
  }
  int32_t d = ty_date2j(tm->year, tm->mon, tm->mday) - TY_POSTGRES_EPOCH_JDATE;

  if (!ty_date_valid(d)) {
    return ty_internal::set_error(TY_ERR_RANGE, "date out of range");
  }
  *out = d;
  return 0;
}
