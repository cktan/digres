/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * tonum.cc -- to_char of a number (ty_tochar.h): the compile and the checks
 * of a format, the digits of each type, and the writing of the digits by
 * the format, as PostgreSQL's NUM_processor does it.
 */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "ty_decimal.h"
#include "ty_tochar.h"

namespace {

/* The keywords. */
enum num_id : uint8_t {
  N_COMMA,
  N_DEC,
  N_0,
  N_9,
  N_B,
  N_C,
  N_D,
  N_E,
  N_FM,
  N_G,
  N_L,
  N_MI,
  N_PL,
  N_PR,
  N_RN,
  N_SG,
  N_SP,
  N_S,
  N_TH,
  N_V,
  N_rn,
  N_th
};

struct keyword_t {
  const char *name;
  uint8_t len;
  uint8_t id;
};

/* PostgreSQL's NUM_keywords, in its order. */
const keyword_t k_keywords[] = {
    {",", 1, N_COMMA}, {".", 1, N_DEC}, {"0", 1, N_0},   {"9", 1, N_9},
    {"B", 1, N_B},     {"C", 1, N_C},   {"D", 1, N_D},   {"EEEE", 4, N_E},
    {"FM", 2, N_FM},   {"G", 1, N_G},   {"L", 1, N_L},   {"MI", 2, N_MI},
    {"PL", 2, N_PL},   {"PR", 2, N_PR}, {"RN", 2, N_RN}, {"SG", 2, N_SG},
    {"SP", 2, N_SP},   {"S", 1, N_S},   {"TH", 2, N_TH}, {"V", 1, N_V},
    {"b", 1, N_B},     {"c", 1, N_C},   {"d", 1, N_D},   {"eeee", 4, N_E},
    {"fm", 2, N_FM},   {"g", 1, N_G},   {"l", 1, N_L},   {"mi", 2, N_MI},
    {"pl", 2, N_PL},   {"pr", 2, N_PR}, {"rn", 2, N_rn}, {"sg", 2, N_SG},
    {"sp", 2, N_SP},   {"s", 1, N_S},   {"th", 2, N_th}, {"v", 1, N_V},
};
constexpr int k_nkeywords = sizeof k_keywords / sizeof k_keywords[0];

/* The first keyword of each first byte, or -1. */
struct first_t {
  int8_t at[128];
  first_t() {
    for (int c = 0; c < 128; c++) {
      at[c] = -1;
    }
    for (int i = k_nkeywords - 1; i >= 0; i--) {
      at[(unsigned char)k_keywords[i].name[0]] = (int8_t)i;
    }
  }
};
const first_t k_first;

/* The flags of a format, as PostgreSQL's NUM_F_*. */
enum : uint32_t {
  F_DECIMAL = 1 << 1,
  F_LDECIMAL = 1 << 2,
  F_ZERO = 1 << 3,
  F_BLANK = 1 << 4,
  F_FILLMODE = 1 << 5,
  F_LSIGN = 1 << 6,
  F_BRACKET = 1 << 7,
  F_MINUS = 1 << 8,
  F_PLUS = 1 << 9,
  F_ROMAN = 1 << 10,
  F_MULTI = 1 << 11,
  F_PLUS_POST = 1 << 12,
  F_MINUS_POST = 1 << 13,
  F_EEEE = 1 << 14
};
enum { LSIGN_PRE = -1, LSIGN_NONE = 0, LSIGN_POST = 1 };

/* The description of a format, as PostgreSQL's NUMDesc. */
struct desc_t {
  int pre, post, lsign, pre_lsign_num, multi, zero_start, zero_end;
  uint32_t flag;
  bool need_locale;
};

/* A node: a keyword, or a character of up to 4 bytes. */
struct node_t {
  uint8_t is_key;
  uint8_t id;
  uint8_t len;
  char ch[4];
};

} // namespace

struct ty_num_format_t {
  size_t len;
  desc_t desc;
  size_t n;
  node_t node[1];
};

namespace {

/* The bytes of the UTF-8 character that begins with byte b. */
inline size_t char_len(unsigned char b) {
  return b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : 4;
}

/* The keyword at p, or NULL. */
const keyword_t *find_keyword(const char *p, const char *end) {
  unsigned char c = (unsigned char)*p;

  if (c <= ' ' || c >= '~' || k_first.at[c] < 0) {
    return nullptr;
  }
  for (int i = k_first.at[c]; i < k_nkeywords && k_keywords[i].name[0] == *p;
       i++) {
    const keyword_t *k = &k_keywords[i];

    if ((size_t)(end - p) >= k->len && 0 == memcmp(p, k->name, k->len)) {
      return k;
    }
  }
  return nullptr;
}

/* A syntax error of a format. */
int bad(const char *msg) { return ty_internal::set_error(TY_ERR_INVALID, msg); }

/*
 * Add a keyword to the description, and check it against those before it,
 * as PostgreSQL's NUMDesc_prepare: each check, and its message, in its
 * order.
 */
int describe(desc_t *num, uint8_t id) {
  if ((num->flag & F_EEEE) && N_E != id) {
    return bad("\"EEEE\" must be the last pattern used");
  }
  switch (id) {
  case N_9:
    if (num->flag & F_BRACKET) {
      return bad("\"9\" must be ahead of \"PR\"");
    }
    if (num->flag & F_MULTI) {
      ++num->multi;
      break;
    }
    if (num->flag & F_DECIMAL) {
      ++num->post;
    } else {
      ++num->pre;
    }
    break;
  case N_0:
    if (num->flag & F_BRACKET) {
      return bad("\"0\" must be ahead of \"PR\"");
    }
    if (!(num->flag & F_ZERO) && !(num->flag & F_DECIMAL)) {
      num->flag |= F_ZERO;
      num->zero_start = num->pre + 1;
    }
    if (!(num->flag & F_DECIMAL)) {
      ++num->pre;
    } else {
      ++num->post;
    }
    num->zero_end = num->pre + num->post;
    break;
  case N_B:
    if (0 == num->pre && 0 == num->post && !(num->flag & F_ZERO)) {
      num->flag |= F_BLANK;
    }
    break;
  case N_D:
  case N_DEC:
    if (N_D == id) {
      num->flag |= F_LDECIMAL;
      num->need_locale = true;
    }
    if (num->flag & F_DECIMAL) {
      return bad("multiple decimal points");
    }
    if (num->flag & F_MULTI) {
      return bad("cannot use \"V\" and decimal point together");
    }
    num->flag |= F_DECIMAL;
    break;
  case N_FM:
    num->flag |= F_FILLMODE;
    break;
  case N_S:
    if (num->flag & F_LSIGN) {
      return bad("cannot use \"S\" twice");
    }
    if (num->flag & (F_PLUS | F_MINUS | F_BRACKET)) {
      return bad("cannot use \"S\" and \"PL\"/\"MI\"/\"SG\"/\"PR\" together");
    }
    if (!(num->flag & F_DECIMAL)) {
      num->lsign = LSIGN_PRE;
      num->pre_lsign_num = num->pre;
      num->need_locale = true;
      num->flag |= F_LSIGN;
    } else if (LSIGN_NONE == num->lsign) {
      num->lsign = LSIGN_POST;
      num->need_locale = true;
      num->flag |= F_LSIGN;
    }
    break;
  case N_MI:
    if (num->flag & F_LSIGN) {
      return bad("cannot use \"S\" and \"MI\" together");
    }
    num->flag |= F_MINUS;
    if (num->flag & F_DECIMAL) {
      num->flag |= F_MINUS_POST;
    }
    break;
  case N_PL:
    if (num->flag & F_LSIGN) {
      return bad("cannot use \"S\" and \"PL\" together");
    }
    num->flag |= F_PLUS;
    if (num->flag & F_DECIMAL) {
      num->flag |= F_PLUS_POST;
    }
    break;
  case N_SG:
    if (num->flag & F_LSIGN) {
      return bad("cannot use \"S\" and \"SG\" together");
    }
    num->flag |= F_MINUS | F_PLUS;
    break;
  case N_PR:
    if (num->flag & (F_LSIGN | F_PLUS | F_MINUS)) {
      return bad("cannot use \"PR\" and \"S\"/\"PL\"/\"MI\"/\"SG\" together");
    }
    num->flag |= F_BRACKET;
    break;
  case N_rn:
  case N_RN:
    if (num->flag & F_ROMAN) {
      return bad("cannot use \"RN\" twice");
    }
    num->flag |= F_ROMAN;
    break;
  case N_L:
  case N_G:
    num->need_locale = true;
    break;
  case N_V:
    if (num->flag & F_DECIMAL) {
      return bad("cannot use \"V\" and decimal point together");
    }
    num->flag |= F_MULTI;
    break;
  case N_E:
    if (num->flag & F_EEEE) {
      return bad("cannot use \"EEEE\" twice");
    }
    if (num->flag & (F_BLANK | F_FILLMODE | F_LSIGN | F_BRACKET | F_MINUS |
                     F_PLUS | F_ROMAN | F_MULTI)) {
      return bad("\"EEEE\" is incompatible with other formats");
    }
    num->flag |= F_EEEE;
    break;
  }
  if ((num->flag & F_ROMAN) && 0 != (num->flag & ~(F_ROMAN | F_FILLMODE))) {
    return bad("\"RN\" is incompatible with other formats");
  }
  return 0;
}

/* Add a node of the character of n bytes at p. */
inline void add_char(ty_num_format_t *f, const char *p, size_t n) {
  node_t *d = &f->node[f->n++];

  d->is_key = 0;
  d->len = (uint8_t)n;
  memcpy(d->ch, p, n);
}

} // namespace

/*
 * Compile a number format: as a date format's (ty_dch_compile), with no
 * prefix or suffix, and each keyword added to the description and checked.
 */
int ty_num_compile(const char *fmt, size_t len, ty_num_format_t **out) {
  ty_num_format_t *f = (ty_num_format_t *)ty_alloc(
      sizeof(ty_num_format_t) + (len ? len - 1 : 0) * sizeof(node_t));

  if (nullptr == f) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  const char *p = fmt, *end = fmt + len;

  memset(&f->desc, 0, sizeof f->desc);
  f->len = len;
  f->n = 0;
  while (p < end) {
    const keyword_t *k = find_keyword(p, end);

    if (nullptr != k) {
      node_t *d = &f->node[f->n++];

      d->is_key = 1;
      d->id = k->id;
      d->len = k->len;
      p += k->len;
      if (0 != describe(&f->desc, k->id)) {
        ty_free(f);
        return -1;
      }
      continue;
    }
    if ('"' == *p) {
      for (p++; p < end;) {
        if ('"' == *p) {
          p++;
          break;
        }
        if ('\\' == *p && p + 1 < end) {
          p++;
        }
        size_t n = char_len((unsigned char)*p);

        n = n > (size_t)(end - p) ? (size_t)(end - p) : n;
        add_char(f, p, n);
        p += n;
      }
      continue;
    }
    if ('\\' == *p && p + 1 < end && '"' == p[1]) {
      p++;
    }
    size_t n = char_len((unsigned char)*p);

    n = n > (size_t)(end - p) ? (size_t)(end - p) : n;
    add_char(f, p, n);
    p += n;
  }
  *out = f;
  return 0;
}

/* The parts of the format that make a number's digits. */
void ty_num_describe(const ty_num_format_t *f, ty_num_desc_t *d) {
  d->pre = f->desc.pre;
  d->post = f->desc.post;
  d->multi = f->desc.multi;
  d->v = 0 != (f->desc.flag & F_MULTI);
  d->roman = 0 != (f->desc.flag & F_ROMAN);
  d->eeee = 0 != (f->desc.flag & F_EEEE);
}

namespace {

/* PostgreSQL's NUM_MAX_ITEM_SIZ: the bytes of output a byte of format holds. */
constexpr size_t k_item = 8;

/* The state of a write, as PostgreSQL's NUMProc for to_char. */
struct proc_t {
  desc_t num;
  int sign;
  bool sign_wrote;
  int num_count, num_in, num_curr, out_pre_spaces;
  const char *number, *number_p, *last_relevant;
  char *out;
  const char *negative_sign, *positive_sign, *decimal, *thousands_sep,
      *currency;
};

/* The bytes of s, cut to at most max at a character's start. */
size_t clip(const char *s, size_t len, size_t max) {
  if (len <= max) {
    return len;
  }
  size_t n = 0;

  while (n < len) {
    size_t c = char_len((unsigned char)s[n]);

    if (n + c > max) {
      break;
    }
    n += c;
  }
  return n;
}

/* The characters of the len bytes at s. */
size_t chars(const char *s, size_t len) {
  size_t n = 0;

  for (size_t i = 0; i < len; i += char_len((unsigned char)s[i])) {
    n++;
  }
  return n;
}

/* Write a locale's symbol, cut to 8 bytes, a byte at a time: it is short. */
void put_symbol(proc_t *np, const char *s) {
  size_t len = clip(s, strlen(s), k_item);

  for (size_t i = 0; i < len; i++) {
    *np->out++ = s[i];
  }
}

/* The last digit after the point that is not 0, or the point; NULL if none. */
const char *last_relevant_decnum(const char *num) {
  const char *p = strchr(num, '.'), *result;

  if (nullptr == p) {
    return nullptr;
  }
  for (result = p; *++p;) {
    if ('0' != *p) {
      result = p;
    }
  }
  return result;
}

/* PostgreSQL's IS_PREDEC_SPACE: the 0 before the point that "9.9" blanks. */
inline bool predec_space(const proc_t *np) {
  return !(np->num.flag & F_ZERO) && np->number == np->number_p &&
         '0' == *np->number && 0 != np->num.post;
}

/*
 * A digit, a 0 or a point of the format, as PostgreSQL's
 * NUM_numpart_to_char.
 * 1. Write the sign before the first digit written: the locale's for S,
 *    < for PR, a space for + unless FM, and -.
 * 2. Write a space, a 0, the point, or the number's digit.
 * 3. After the last digit, write > for PR, or the locale's sign for an S
 *    after the number.
 */
void numpart(proc_t *np, uint8_t id) {
  desc_t *num = &np->num;

  if (num->flag & F_ROMAN) {
    return;
  }
  np->num_in = 0;

  // 1. Write the sign before the first digit written.
  if (!np->sign_wrote &&
      (np->num_curr >= np->out_pre_spaces ||
       ((num->flag & F_ZERO) && num->zero_start == np->num_curr)) &&
      (!predec_space(np) || (np->last_relevant && '.' == *np->last_relevant))) {
    if (num->flag & F_LSIGN) {
      if (LSIGN_PRE == num->lsign) {
        put_symbol(np, '-' == np->sign ? np->negative_sign : np->positive_sign);
        np->sign_wrote = true;
      }
    } else if (num->flag & F_BRACKET) {
      *np->out++ = '+' == np->sign ? ' ' : '<';
      np->sign_wrote = true;
    } else if ('+' == np->sign) {
      if (!(num->flag & F_FILLMODE)) {
        *np->out++ = ' ';
      }
      np->sign_wrote = true;
    } else if ('-' == np->sign) {
      *np->out++ = '-';
      np->sign_wrote = true;
    }
  }

  // 2. Write a space, a 0, the point, or the number's digit.
  if (N_9 == id || N_0 == id || N_D == id || N_DEC == id) {
    if (np->num_curr < np->out_pre_spaces &&
        (num->zero_start > np->num_curr || !(num->flag & F_ZERO))) {
      if (!(num->flag & F_FILLMODE)) {
        *np->out++ = ' ';
      }
    } else if ((num->flag & F_ZERO) && np->num_curr < np->out_pre_spaces &&
               num->zero_start <= np->num_curr) {
      *np->out++ = '0';
      np->num_in = 1;
    } else {
      if ('.' == *np->number_p) {
        if (!np->last_relevant || '.' != *np->last_relevant) {
          put_symbol(np, np->decimal);
        } else if ((num->flag & F_FILLMODE) && np->last_relevant &&
                   '.' == *np->last_relevant) {
          /* FM9.9 writes "n." */
          put_symbol(np, np->decimal);
        }
      } else {
        if (np->last_relevant && np->number_p > np->last_relevant &&
            N_0 != id) {
          /* a 0 at the end that FM drops */
        } else if (predec_space(np)) {
          /* "0.1" by 9.9 is " .1"; "0" by FM9.9 is "0." */
          if (!(num->flag & F_FILLMODE)) {
            *np->out++ = ' ';
          } else if (np->last_relevant && '.' == *np->last_relevant) {
            *np->out++ = '0';
          }
        } else {
          *np->out++ = *np->number_p;
          np->num_in = 1;
        }
      }
      if (*np->number_p) {
        ++np->number_p;
      }
    }

    // 3. After the last digit, write > for PR, or a sign after.
    int end = np->num_count + (np->out_pre_spaces ? 1 : 0) +
              ((num->flag & F_DECIMAL) ? 1 : 0);

    if (np->last_relevant && np->last_relevant == np->number_p) {
      end = np->num_curr;
    }
    if (np->num_curr + 1 == end) {
      if (np->sign_wrote && (num->flag & F_BRACKET)) {
        *np->out++ = '+' == np->sign ? ' ' : '>';
      } else if ((num->flag & F_LSIGN) && LSIGN_POST == num->lsign) {
        put_symbol(np, '-' == np->sign ? np->negative_sign : np->positive_sign);
      }
    }
  }
  ++np->num_curr;
}

/*
 * The suffix TH or th of the number: PostgreSQL's get_th, which refuses a
 * number that does not end in a digit.
 */
int put_th(proc_t *np, bool upper) {
  size_t len = strlen(np->number);
  char last = len ? np->number[len - 1] : 0;

  if (last < '0' || last > '9') {
    char msg[128];

    snprintf(msg, sizeof msg, "\"%s\" is not a number", np->number);
    return ty_internal::set_error(TY_ERR_INVALID, msg);
  }
  if (len > 1 && '1' == np->number[len - 2]) {
    last = 0;
  }
  const char *s = '1' == last   ? "st"
                  : '2' == last ? "nd"
                  : '3' == last ? "rd"
                                : "th";

  *np->out++ = upper ? (char)(s[0] - 'a' + 'A') : s[0];
  *np->out++ = upper ? (char)(s[1] - 'a' + 'A') : s[1];
  return 0;
}

/* The locale's symbols, with PostgreSQL's defaults, as NUM_prepare_locale. */
void prepare_locale(proc_t *np, const ty_num_locale_t *loc) {
  ty_num_symbols_t sy = {nullptr, nullptr, nullptr, nullptr, nullptr};

  if (!np->num.need_locale) {
    np->negative_sign = "-";
    np->positive_sign = "+";
    np->decimal = ".";
    np->thousands_sep = ",";
    np->currency = " ";
    return;
  }
  if (nullptr != loc) {
    loc->symbols(loc->ctx, &sy);
  }
  np->negative_sign =
      sy.negative_sign && *sy.negative_sign ? sy.negative_sign : "-";
  np->positive_sign =
      sy.positive_sign && *sy.positive_sign ? sy.positive_sign : "+";
  np->decimal = sy.decimal_point && *sy.decimal_point ? sy.decimal_point : ".";
  if (!(np->num.flag & F_LDECIMAL)) {
    np->decimal = ".";
  }
  /* a thousands separator not the point: some locales give "" with "," */
  np->thousands_sep = sy.thousands_sep && *sy.thousands_sep ? sy.thousands_sep
                      : 0 != strcmp(np->decimal, ",")       ? ","
                                                            : ".";
  np->currency =
      sy.currency_symbol && *sy.currency_symbol ? sy.currency_symbol : " ";
}

/*
 * Write the digits by the format, as PostgreSQL's NUM_processor for to_char.
 * 1. EEEE writes the digits as they are.
 * 2. Set the sign: MI, PL and SG write it themselves; FM drops PR's for a
 *    number not negative; an S with no digit before it goes after.
 * 3. FM with a point drops the zeros at the end, but not those the format's
 *    0s keep.
 * 4. Write each node.
 */
int process(const ty_num_format_t *f, const char *number, int out_pre_spaces,
            int sign, int pre, int post, const ty_num_locale_t *loc, char *out,
            char **end) {
  proc_t np;

  memset(&np, 0, sizeof np);
  np.num = f->desc;
  np.num.pre = pre;
  np.num.post = post;
  np.number = number;
  np.out = out;
  if (np.num.zero_start) {
    --np.num.zero_start;
  }

  // 1. EEEE writes the digits as they are.
  if (np.num.flag & F_EEEE) {
    size_t len = strlen(number);

    memcpy(out, number, len);
    *end = out + len;
    return 0;
  }

  // 2. Set the sign.
  np.sign = sign;
  if (np.num.flag & (F_PLUS | F_MINUS)) {
    np.sign_wrote = !((np.num.flag & F_PLUS) && !(np.num.flag & F_MINUS));
  } else {
    if ('-' != np.sign && (np.num.flag & F_FILLMODE)) {
      np.num.flag &= ~F_BRACKET;
    }
    np.sign_wrote = '+' == np.sign && (np.num.flag & F_FILLMODE) &&
                    !(np.num.flag & F_LSIGN);
    if (LSIGN_PRE == np.num.lsign && np.num.pre == np.num.pre_lsign_num) {
      np.num.lsign = LSIGN_POST;
    }
  }
  np.num_count = np.num.post + np.num.pre - 1;
  np.out_pre_spaces = out_pre_spaces;

  // 3. FM with a point drops the zeros at the end.
  if ((np.num.flag & F_FILLMODE) && (np.num.flag & F_DECIMAL)) {
    np.last_relevant = last_relevant_decnum(number);
    if (np.last_relevant && np.num.zero_end > np.out_pre_spaces) {
      int last_zero_pos = (int)strlen(number) - 1;

      if (last_zero_pos > np.num.zero_end - np.out_pre_spaces) {
        last_zero_pos = np.num.zero_end - np.out_pre_spaces;
      }
      if (np.last_relevant < number + last_zero_pos) {
        np.last_relevant = number + last_zero_pos;
      }
    }
  }
  if (!np.sign_wrote && 0 == np.out_pre_spaces) {
    ++np.num_count;
  }
  prepare_locale(&np, loc);
  np.number_p = number;

  // 4. Write each node.
  for (size_t k = 0; k < f->n; k++) {
    const node_t *n = &f->node[k];

    if (!n->is_key) {
      /* a byte at a time: a character is 1 to 4 bytes */
      for (int i = 0; i < n->len; i++) {
        *np.out++ = n->ch[i];
      }
      continue;
    }
    switch (n->id) {
    case N_9:
    case N_0:
    case N_DEC:
    case N_D:
      numpart(&np, n->id);
      break;
    case N_COMMA:
      if (np.num_in) {
        *np.out++ = ',';
      } else if (!(np.num.flag & F_FILLMODE)) {
        *np.out++ = ' ';
      }
      break;
    case N_G: {
      size_t len = clip(np.thousands_sep, strlen(np.thousands_sep), k_item);

      if (np.num_in) {
        memcpy(np.out, np.thousands_sep, len);
        np.out += len;
      } else if (!(np.num.flag & F_FILLMODE)) {
        /* a space a character of the separator */
        size_t c = chars(np.thousands_sep, len);

        memset(np.out, ' ', c);
        np.out += c;
      }
      break;
    }
    case N_L:
      put_symbol(&np, np.currency);
      break;
    case N_RN:
    case N_rn: {
      size_t len = strlen(np.number_p);
      size_t pad = (np.num.flag & F_FILLMODE) || len >= 15 ? 0 : 15 - len;

      memset(np.out, ' ', pad);
      np.out += pad;
      for (size_t i = 0; i < len; i++) {
        char c = np.number_p[i];

        *np.out++ =
            N_rn == n->id && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
      }
      break;
    }
    case N_th:
    case N_TH:
      if ((np.num.flag & (F_ROMAN | F_DECIMAL)) || '#' == *np.number ||
          '-' == np.sign) {
        break;
      }
      if (0 != put_th(&np, N_TH == n->id)) {
        return -1;
      }
      break;
    case N_MI:
      if ('-' == np.sign) {
        *np.out++ = '-';
      } else if (!(np.num.flag & F_FILLMODE)) {
        *np.out++ = ' ';
      }
      break;
    case N_PL:
      if ('+' == np.sign) {
        *np.out++ = '+';
      } else if (!(np.num.flag & F_FILLMODE)) {
        *np.out++ = ' ';
      }
      break;
    case N_SG:
      *np.out++ = (char)np.sign;
      break;
    default:
      /* B, C, EEEE, FM, PR, S, SP and V write nothing here */
      break;
    }
  }
  *end = np.out;
  return 0;
}

/*
 * Write the digits by the format into a new text, of 8 bytes a byte of
 * format, as PostgreSQL allots; an empty or huge format is an empty text.
 */
int to_text(const ty_num_format_t *f, const char *number, int out_pre_spaces,
            int sign, int pre, int post, const ty_num_locale_t *loc,
            ty_varlena_t **out) {
  char *base, *end;

  if (0 == f->len || f->len >= (size_t)(INT32_MAX - 4) / k_item) {
    return ty_vl_new_copy("", 0, out);
  }
  if (0 != ty_vl_new(f->len * k_item + strlen(number), out, &base)) {
    return -1;
  }
  if (0 !=
      process(f, number, out_pre_spaces, sign, pre, post, loc, base, &end)) {
    ty_free(*out);
    *out = nullptr;
    return -1;
  }
  /*
   * A format of more digits than the number writes the number's NUL, and
   * PostgreSQL's result ends there, at its strlen
   */
  ty_vl_set_len(*out, strnlen(base, (size_t)(end - base)));
  return 0;
}

/* A number too wide for the format: pre + post + 1 #, the point at pre. */
void hashes(char *buf, int pre, int post) {
  memset(buf, '#', (size_t)(pre + post + 1));
  buf[pre + post + 1] = '\0';
  buf[pre] = '.';
}

/* An integer's text, with its sign, written here: printf costs more. */
int int_text(int64_t v, char *buf) {
  char tmp[24];
  uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
  int n = 0, pos = 0;

  do {
    tmp[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  if (v < 0) {
    buf[pos++] = '-';
  }
  while (n) {
    buf[pos++] = tmp[--n];
  }
  buf[pos] = '\0';
  return pos;
}

/*
 * The digits of an integer, as int4_to_char and int8_to_char make them
 * after V: its text, a point and post zeros, and the spaces before it or #
 * if it does not fit pre.
 */
int int_to_char(int64_t v, const ty_num_format_t *f, const ty_num_locale_t *loc,
                ty_varlena_t **out, int pre, int post) {
  char txt[24];
  const char *digits = txt;
  int len = int_text(v, txt), out_pre_spaces = 0, sign = '+';

  if ('-' == *digits) {
    sign = '-';
    digits++;
    len--;
  }
  /* the digits on the stack, unless the format is long */
  char stack[128];
  size_t need = (size_t)(len + post + pre + 3);
  char *num = need <= sizeof stack ? stack : (char *)ty_alloc(need);

  if (nullptr == num) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  memcpy(num, digits, (size_t)len);
  if (post) {
    num[len] = '.';
    memset(num + len + 1, '0', (size_t)post);
    num[len + post + 1] = '\0';
  } else {
    num[len] = '\0';
  }
  if (len < pre) {
    out_pre_spaces = pre - len;
  } else if (len > pre) {
    hashes(num, pre, post);
  }
  int rc = to_text(f, num, out_pre_spaces, sign, pre, post, loc, out);

  if (num != stack) {
    ty_free(num);
  }
  return rc;
}

/*
 * An int8 in scientific notation, as digres's numeric_out_sci makes it:
 * the exponent from its digits, and the significand rounded to scale places
 * as a decimal, then "e" and the exponent's sign and 2 digits at least.
 */
void int64_sci(int64_t v, int scale, char *buf, size_t size) {
  char dig[24];
  uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
  int nd = snprintf(dig, sizeof dig, "%llu", (unsigned long long)u);
  int exponent = 0 == v ? 0 : nd - 1;
  ty_dec128_t sig = {(ty_int128_t)v, exponent}, r = sig;
  char txt[96];
  int pos = 0;

  if (scale < 0) {
    scale = 0;
  }
  if (scale < sig.scale) {
    ty_dec128_round(&sig, scale, &r);
  }
  /* the decimal's text: every place of its scale */
  {
    ty_int128_t x = r.value;
    unsigned __int128 ux =
        x < 0 ? 0 - (unsigned __int128)x : (unsigned __int128)x;
    char d[48];
    int n = 0;

    do {
      d[n++] = (char)('0' + (int)(ux % 10));
      ux /= 10;
    } while (ux);
    if (x < 0) {
      txt[pos++] = '-';
    }
    if (n <= r.scale) {
      txt[pos++] = '0';
      if (r.scale > 0) {
        txt[pos++] = '.';
      }
      for (int i = n; i < r.scale; i++) {
        txt[pos++] = '0';
      }
      for (int i = n - 1; i >= 0; i--) {
        txt[pos++] = d[i];
      }
    } else {
      for (int i = n - 1; i >= r.scale; i--) {
        txt[pos++] = d[i];
      }
      if (r.scale > 0) {
        txt[pos++] = '.';
        for (int i = r.scale - 1; i >= 0; i--) {
          txt[pos++] = d[i];
        }
      }
    }
  }
  if (scale >= sig.scale) {
    if (0 == sig.scale && scale > 0) {
      txt[pos++] = '.';
    }
    for (int i = sig.scale; i < scale; i++) {
      txt[pos++] = '0';
    }
  }
  txt[pos] = '\0';
  snprintf(buf, size, "%se%+03d", txt, exponent);
}

/* A float in scientific notation, the sign of a positive one a space. */
char *float_sci(double v, int post) {
  int n = snprintf(nullptr, 0, "%+.*e", post, v);
  char *s = (char *)ty_alloc((size_t)n + 1);

  if (nullptr != s) {
    snprintf(s, (size_t)n + 1, "%+.*e", post, v);
    if ('+' == *s) {
      *s = ' ';
    }
  }
  return s;
}

/*
 * printf's %.*f as PostgreSQL's snprintf writes it, into buf (NULL to
 * count): NaN is "NaN" and an infinity "Infinity", where libc writes "nan"
 * and "inf".
 */
int pg_fixed(char *buf, size_t size, int prec, double v) {
  const char *s = isnan(v)   ? "NaN"
                  : isinf(v) ? (v < 0 ? "-Infinity" : "Infinity")
                             : nullptr;

  if (nullptr != s) {
    return snprintf(buf, size, "%s", s);
  }
  return snprintf(buf, size, "%.*f", prec, v);
}

/*
 * A float's to_char that is not EEEE nor RN, as float8_to_char: the digits
 * before the point first, to fit the precision, then the digits at post.
 */
int float_digits(double val, int maxdig, const ty_num_format_t *f,
                 const ty_num_locale_t *loc, ty_varlena_t **out, int pre,
                 int post) {
  /* a double has at most 309 digits before the point */
  char stack[512];
  int n = pg_fixed(stack, sizeof stack, 0, fabs(val));

  if (n >= maxdig) {
    post = 0;
  } else if (n + post > maxdig) {
    post = maxdig - n;
  }
  size_t need = (size_t)n + 2 + (size_t)post + (size_t)pre + 3;
  char *org = need <= sizeof stack ? stack : (char *)ty_alloc(need);

  if (nullptr == org) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  pg_fixed(org, need, post, val);
  char *num = org;
  int sign = '+', out_pre_spaces = 0;

  if ('-' == *num) {
    sign = '-';
    num++;
  }
  const char *p = strchr(num, '.');
  int pre_len = p ? (int)(p - num) : (int)strlen(num);

  if (pre_len < pre) {
    out_pre_spaces = pre - pre_len;
  } else if (pre_len > pre) {
    num = org;
    hashes(num, pre, post);
  }
  int rc = to_text(f, num, out_pre_spaces, sign, pre, post, loc, out);

  if (org != stack) {
    ty_free(org);
  }
  return rc;
}

/* A float's EEEE of NaN or an infinity: a space and #, the point at pre+1. */
int float_sci_hashes(const ty_num_format_t *f, const ty_num_locale_t *loc,
                     ty_varlena_t **out) {
  int pre = f->desc.pre, post = f->desc.post;
  char *num = (char *)ty_alloc((size_t)(pre + post + 7));

  if (nullptr == num) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  memset(num, '#', (size_t)(pre + post + 6));
  num[pre + post + 6] = '\0';
  num[0] = ' ';
  num[pre + 1] = '.';
  int rc = to_text(f, num, 0, 0, pre, post, loc, out);

  ty_free(num);
  return rc;
}

/* A text's to_char from digits made here. */
int digits_to_char(const ty_num_format_t *f, const char *num,
                   const ty_num_locale_t *loc, ty_varlena_t **out) {
  return to_text(f, num, 0, 0, f->desc.pre, f->desc.post, loc, out);
}

/* PostgreSQL's dtoi4 and dtoi8 of 10^multi, for V: a power past the range
 * is the integer's error. */
int pow10_int(int multi, int64_t lo, const char *msg, int64_t *out) {
  double m = rint(pow(10.0, (double)multi));

  if (isnan(m) || !(m >= (double)lo && m < -(double)lo)) {
    return ty_internal::set_error(TY_ERR_RANGE, msg);
  }
  *out = (int64_t)m;
  return 0;
}

} // namespace

/* The Roman numeral of n: 15 # out of 1 to 3999. */
const char *ty_num_roman(int n, char buf[16]) {
  static const char *const rm1[] = {"I",  "II",  "III",  "IV", "V",
                                    "VI", "VII", "VIII", "IX"};
  static const char *const rm10[] = {"X",  "XX",  "XXX",  "XL", "L",
                                     "LX", "LXX", "LXXX", "XC"};
  static const char *const rm100[] = {"C",  "CC",  "CCC",  "CD", "D",
                                      "DC", "DCC", "DCCC", "CM"};
  char digits[8];

  buf[0] = '\0';
  if (n > 3999 || n < 1) {
    memset(buf, '#', 15);
    buf[15] = '\0';
    return buf;
  }
  int len = snprintf(digits, sizeof digits, "%d", n);

  for (const char *p = digits; *p; p++, len--) {
    int d = *p - '1';

    if (d < 0) {
      continue;
    }
    if (4 == len) {
      while (d-- >= 0) {
        strcat(buf, "M");
      }
    } else {
      strcat(buf, 3 == len ? rm100[d] : 2 == len ? rm10[d] : rm1[d]);
    }
  }
  return buf;
}

/* to_char by digits a host made. */
int ty_num_to_char_digits(const ty_num_format_t *f, const char *numstr,
                          int out_pre_spaces, int sign, int pre, int post,
                          const ty_num_locale_t *loc, ty_varlena_t **out) {
  return to_text(f, numstr, out_pre_spaces, sign, pre, post, loc, out);
}

/* to_char of an int4: RN, EEEE through a double, or the digits after V. */
int ty_int32_to_char(int32_t v, const ty_num_format_t *f,
                     const ty_num_locale_t *loc, ty_varlena_t **out) {
  const desc_t *d = &f->desc;

  if (0 == f->len || f->len >= (size_t)(INT32_MAX - 4) / k_item) {
    return ty_vl_new_copy("", 0, out);
  }
  if (d->flag & F_ROMAN) {
    char buf[16];

    return digits_to_char(f, ty_num_roman(v, buf), loc, out);
  }
  if (d->flag & F_EEEE) {
    /* a double holds an int4 exactly */
    char *s = float_sci((double)v, d->post);

    if (nullptr == s) {
      return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
    }
    int rc = digits_to_char(f, s, loc, out);

    ty_free(s);
    return rc;
  }
  int pre = d->pre;

  if (d->flag & F_MULTI) {
    int64_t m;
    int32_t r;

    if (0 != pow10_int(d->multi, INT32_MIN, "integer out of range", &m) ||
        (__builtin_mul_overflow(v, (int32_t)m, &r) &&
         ty_internal::set_error(TY_ERR_RANGE, "integer out of range"))) {
      return -1;
    }
    v = r;
    pre += d->multi;
  }
  return int_to_char(v, f, loc, out, pre, d->post);
}

/* to_char of an int8: RN of its int4, EEEE as a decimal, or the digits. */
int ty_int64_to_char(int64_t v, const ty_num_format_t *f,
                     const ty_num_locale_t *loc, ty_varlena_t **out) {
  const desc_t *d = &f->desc;

  if (0 == f->len || f->len >= (size_t)(INT32_MAX - 4) / k_item) {
    return ty_vl_new_copy("", 0, out);
  }
  if (d->flag & F_ROMAN) {
    char buf[16];

    return digits_to_char(
        f,
        ty_num_roman(v <= INT32_MAX && v >= INT32_MIN ? (int)v : INT32_MAX,
                     buf),
        loc, out);
  }
  if (d->flag & F_EEEE) {
    char sci[128];

    /* the sign of a positive number is a space */
    sci[0] = ' ';
    int64_sci(v, d->post, sci + 1, sizeof sci - 1);
    return digits_to_char(f, '-' == sci[1] ? sci + 1 : sci, loc, out);
  }
  int pre = d->pre;

  if (d->flag & F_MULTI) {
    int64_t m, r;

    if (0 != pow10_int(d->multi, INT64_MIN, "bigint out of range", &m) ||
        (__builtin_mul_overflow(v, m, &r) &&
         ty_internal::set_error(TY_ERR_RANGE, "bigint out of range"))) {
      return -1;
    }
    v = r;
    pre += d->multi;
  }
  return int_to_char(v, f, loc, out, pre, d->post);
}

/* to_char of a float8: RN of its rounded int4, EEEE by printf, or digits. */
int ty_fp64_to_char(double v, const ty_num_format_t *f,
                    const ty_num_locale_t *loc, ty_varlena_t **out) {
  const desc_t *d = &f->desc;

  if (0 == f->len || f->len >= (size_t)(INT32_MAX - 4) / k_item) {
    return ty_vl_new_copy("", 0, out);
  }
  if (d->flag & F_ROMAN) {
    char buf[16];
    double r = rint(v);
    /* past an int4, PostgreSQL takes INT32_MAX, which RN cannot write */
    int n = !isnan(r) && r >= (double)INT32_MIN && r < -(double)INT32_MIN
                ? (int)r
                : INT32_MAX;

    return digits_to_char(f, ty_num_roman(n, buf), loc, out);
  }
  if (d->flag & F_EEEE) {
    if (isnan(v) || isinf(v)) {
      return float_sci_hashes(f, loc, out);
    }
    char *s = float_sci(v, d->post);

    if (nullptr == s) {
      return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
    }
    int rc = digits_to_char(f, s, loc, out);

    ty_free(s);
    return rc;
  }
  double val = v;
  int pre = d->pre;

  if (d->flag & F_MULTI) {
    val = v * pow(10.0, (double)d->multi);
    pre += d->multi;
  }
  return float_digits(val, DBL_DIG, f, loc, out, pre, d->post);
}

/* to_char of a float4: as a float8's, in float arithmetic, to FLT_DIG. */
int ty_fp32_to_char(float v, const ty_num_format_t *f,
                    const ty_num_locale_t *loc, ty_varlena_t **out) {
  const desc_t *d = &f->desc;

  if (0 == f->len || f->len >= (size_t)(INT32_MAX - 4) / k_item) {
    return ty_vl_new_copy("", 0, out);
  }
  if (d->flag & F_ROMAN) {
    char buf[16];
    float r = (float)rint(v);
    int n = !isnan(r) && r >= (float)INT32_MIN && r < -((float)INT32_MIN)
                ? (int)r
                : INT32_MAX;

    return digits_to_char(f, ty_num_roman(n, buf), loc, out);
  }
  if (d->flag & F_EEEE) {
    if (isnan(v) || isinf(v)) {
      return float_sci_hashes(f, loc, out);
    }
    char *s = float_sci(v, d->post);

    if (nullptr == s) {
      return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
    }
    int rc = digits_to_char(f, s, loc, out);

    ty_free(s);
    return rc;
  }
  float val = v;
  int pre = d->pre;

  if (d->flag & F_MULTI) {
    float m = (float)pow(10.0, (double)d->multi);

    val = v * m;
    pre += d->multi;
  }
  return float_digits(val, FLT_DIG, f, loc, out, pre, d->post);
}

namespace {

/* The state of a read, as PostgreSQL's NUMProc for to_number. */
struct reader_t {
  desc_t num;
  const char *in, *end; /* the text */
  const char *p;        /* the next byte of it */
  char *number;         /* the sign, then the digits */
  char *number_p;
  int read_pre, read_post;
  bool read_dec;
  const char *negative_sign, *positive_sign, *decimal, *thousands_sep,
      *currency;
};

/* PostgreSQL's OVERLOAD_TEST: the text is read. */
inline bool at_end(const reader_t *r) { return r->p >= r->end; }

/* PostgreSQL's AMOUNT_TEST: n bytes of the text remain. */
inline bool remains(const reader_t *r, size_t n) {
  return (size_t)(r->end - r->p) >= n;
}

/* Whether the text at the reader begins with the bytes of s. */
inline bool reads(const reader_t *r, const char *s, size_t n) {
  return 0 != n && remains(r, n) && 0 == memcmp(r->p, s, n);
}

/* Skip up to n characters that are not a number's: digits, . , + -. */
void eat_non_data(reader_t *r, int n) {
  while (n-- > 0 && !at_end(r) && nullptr == strchr("0123456789.,+-", *r->p)) {
    size_t c = char_len((unsigned char)*r->p);

    r->p += c <= (size_t)(r->end - r->p) ? c : (size_t)(r->end - r->p);
  }
}

/* A Roman numeral's value, or 0 for a letter that is not one. */
inline int roman_val(char c) {
  return 'I' == c   ? 1
         : 'V' == c ? 5
         : 'X' == c ? 10
         : 'L' == c ? 50
         : 'C' == c ? 100
         : 'D' == c ? 500
         : 'M' == c ? 1000
                    : 0;
}

/* PostgreSQL's IS_VALID_SUB_COMB: IV, IX, XL, XC, CD and CM. */
inline bool valid_sub(char a, char b) {
  return ('I' == a && ('V' == b || 'X' == b)) ||
         ('X' == a && ('L' == b || 'C' == b)) ||
         ('C' == a && ('D' == b || 'M' == b));
}

/*
 * Read a Roman numeral of standard form, as PostgreSQL's roman_to_int:
 * 1 to 3999, or -1.
 * 1. Skip the spaces before it, and take up to 15 numerals, in any case.
 * 2. No numeral after a subtraction may be as large as what it subtracted.
 * 3. V, L and D come once, and before no larger numeral.
 * 4. A subtraction is IV, IX, XL, XC, CD or CM, and not after a repeat.
 * 5. A numeral repeats at most 3 times.
 */
int roman_to_int(reader_t *r) {
  char chars[15];
  int values[15], len, result = 0, repeat = 1, v = 0, l = 0, d = 0,
                       last_sub = 0;
  bool subtracted = false;

  // 1. Skip the spaces, and take up to 15 numerals.
  while (!at_end(r) && isspace((unsigned char)*r->p)) {
    r->p++;
  }
  for (len = 0; len < 15 && !at_end(r); len++) {
    char c = *r->p >= 'a' && *r->p <= 'z' ? (char)(*r->p - 'a' + 'A') : *r->p;
    int value = roman_val(c);

    if (0 == value) {
      break;
    }
    chars[len] = c;
    values[len] = value;
    r->p++;
  }
  if (0 == len) {
    return -1;
  }
  for (int i = 0; i < len; i++) {
    char c = chars[i];
    int value = values[i];

    // 2. Nothing as large as a subtracted numeral after it.
    if (subtracted && value >= last_sub) {
      return -1;
    }

    // 3. V, L and D once, and before no larger numeral.
    if ((v && value >= 5) || (l && value >= 50) || (d && value >= 500)) {
      return -1;
    }
    v += 'V' == c;
    l += 'L' == c;
    d += 'D' == c;
    if (i == len - 1) {
      result += value;
      continue;
    }
    char next = chars[i + 1];
    int next_value = values[i + 1];

    if (value < next_value) {
      // 4. A subtraction, and not after a repeat.
      if (!valid_sub(c, next) || repeat > 1) {
        return -1;
      }
      if ((v && next_value >= 5) || (l && next_value >= 50) ||
          (d && next_value >= 500)) {
        return -1;
      }
      v += 'V' == next;
      l += 'L' == next;
      d += 'D' == next;
      i++;
      repeat = 1;
      subtracted = true;
      last_sub = value;
      result += next_value - value;
    } else {
      // 5. A numeral repeats at most 3 times.
      if (c == next) {
        if (++repeat > 3) {
          return -1;
        }
      } else {
        repeat = 1;
      }
      result += value;
    }
  }
  return result;
}

/*
 * A digit, a 0 or a point of the format, read as PostgreSQL's
 * NUM_numpart_from_char.
 * 1. Skip one space.
 * 2. Before the first digit, read a sign: the locale's for S before the
 *    number, else -, + or PR's <.
 * 3. Read a digit, up to the format's places after the point, or the point.
 * 4. After a digit, read the locale's sign for S when no digit follows, or
 *    MI's or PL's sign that FM left unplaced.
 */
void numpart_read(reader_t *r, uint8_t id) {
  desc_t *num = &r->num;
  bool isread = false;

  // 1. Skip one space.
  if (at_end(r)) {
    return;
  }
  if (' ' == *r->p) {
    r->p++;
  }
  if (at_end(r)) {
    return;
  }

  // 2. Before the first digit, read a sign.
  if (' ' == *r->number && (N_0 == id || N_9 == id) &&
      0 == r->read_pre + r->read_post) {
    if ((num->flag & F_LSIGN) && LSIGN_PRE == num->lsign) {
      size_t n;

      if (reads(r, r->negative_sign, n = strlen(r->negative_sign))) {
        r->p += n;
        *r->number = '-';
      } else if (reads(r, r->positive_sign, n = strlen(r->positive_sign))) {
        r->p += n;
        *r->number = '+';
      }
    } else if ('-' == *r->p || ((num->flag & F_BRACKET) && '<' == *r->p)) {
      *r->number = '-';
      r->p++;
    } else if ('+' == *r->p) {
      *r->number = '+';
      r->p++;
    }
  }
  if (at_end(r)) {
    return;
  }

  // 3. Read a digit, or the point.
  if (isdigit((unsigned char)*r->p)) {
    if (r->read_dec && r->read_post == num->post) {
      return;
    }
    *r->number_p++ = *r->p;
    if (r->read_dec) {
      r->read_post++;
    } else {
      r->read_pre++;
    }
    isread = true;
  } else if ((num->flag & F_DECIMAL) && !r->read_dec) {
    /* decimal is "." without D */
    size_t n = strlen(r->decimal);

    if (reads(r, r->decimal, n)) {
      r->p += n - 1;
      *r->number_p++ = '.';
      r->read_dec = true;
      isread = true;
    }
  }
  if (at_end(r)) {
    return;
  }

  // 4. After a digit, a sign behind the number.
  if (' ' == *r->number && r->read_pre + r->read_post > 0) {
    if ((num->flag & F_LSIGN) && isread && r->p + 1 < r->end &&
        !isdigit((unsigned char)r->p[1])) {
      const char *tmp = r->p++;
      size_t n;

      /* each - 1: the caller steps past the last byte */
      if (reads(r, r->negative_sign, n = strlen(r->negative_sign))) {
        r->p += n - 1;
        *r->number = '-';
      } else if (reads(r, r->positive_sign, n = strlen(r->positive_sign))) {
        r->p += n - 1;
        *r->number = '+';
      }
      if (' ' == *r->number) {
        r->p = tmp;
      }
    } else if (!isread && !(num->flag & F_LSIGN) &&
               (num->flag & (F_PLUS | F_MINUS))) {
      /* the caller steps past it */
      if ('-' == *r->p || '+' == *r->p) {
        *r->number = *r->p;
      }
    }
  }
}

} // namespace

/*
 * to_number's read, as PostgreSQL's NUM_processor for input.
 * 1. EEEE cannot be read.
 * 2. Each node: a digit, a 0 or a point reads its part; the comma and G
 *    skip the separator if it is there; L skips the symbol; RN reads a Roman
 *    numeral; TH skips two letters; MI, PL and SG read a sign; a character
 *    of the format skips one of the text.
 * 3. End the digits, without a point at the end; the scale is the places
 *    read after the point.
 */
int ty_num_from_char(const ty_num_format_t *f, const char *in, size_t len,
                     const ty_num_locale_t *loc, char *out, int *precision,
                     int *scale) {
  reader_t r;
  proc_t np;

  memset(&r, 0, sizeof r);
  r.num = f->desc;
  if (r.num.zero_start) {
    --r.num.zero_start;
  }

  // 1. EEEE cannot be read.
  if (r.num.flag & F_EEEE) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "\"EEEE\" not supported for input");
  }
  memset(&np, 0, sizeof np);
  np.num = r.num;
  prepare_locale(&np, loc);
  r.negative_sign = np.negative_sign;
  r.positive_sign = np.positive_sign;
  r.decimal = np.decimal;
  r.thousands_sep = np.thousands_sep;
  r.currency = np.currency;
  r.in = in;
  r.end = in + len;
  r.p = in;
  r.number = out;
  out[0] = ' ';
  out[1] = '\0';
  r.number_p = out + 1;

  // 2. Each node.
  for (size_t k = 0; k < f->n && !at_end(&r); k++) {
    const node_t *n = &f->node[k];

    if (!n->is_key) {
      size_t c = char_len((unsigned char)*r.p);

      r.p += c <= (size_t)(r.end - r.p) ? c : (size_t)(r.end - r.p);
      continue;
    }
    switch (n->id) {
    case N_9:
    case N_0:
    case N_DEC:
    case N_D:
      numpart_read(&r, n->id);
      r.p++;
      break;
    case N_COMMA:
      if ((r.num.flag & F_FILLMODE) || ',' != *r.p) {
        break;
      }
      r.p++;
      break;
    case N_G: {
      size_t plen = strlen(r.thousands_sep);

      if (r.num.flag & F_FILLMODE) {
        break;
      }
      if (reads(&r, r.thousands_sep, plen)) {
        r.p += plen;
      }
      break;
    }
    case N_L:
      eat_non_data(&r, (int)chars(r.currency, strlen(r.currency)));
      break;
    case N_RN:
    case N_rn: {
      int value = roman_to_int(&r);

      if (value < 0) {
        return ty_internal::set_error(TY_ERR_INVALID, "invalid Roman numeral");
      }
      int numlen = snprintf(r.number_p, 12, "%d", value);

      r.number_p += numlen;
      r.num.pre = numlen;
      r.num.post = 0;
      break;
    }
    case N_th:
    case N_TH:
      if ((r.num.flag & (F_ROMAN | F_DECIMAL)) || '#' == *r.number) {
        break;
      }
      eat_non_data(&r, 2);
      break;
    case N_MI:
    case N_PL:
    case N_SG: {
      bool minus = '-' == *r.p && N_PL != n->id;
      bool plus = '+' == *r.p && N_MI != n->id;

      if (minus || plus) {
        *r.number = *r.p;
        r.p++;
      } else {
        eat_non_data(&r, 1);
      }
      break;
    }
    default:
      break;
    }
  }

  // 3. End the digits, without a point at the end.
  if ('.' == r.number_p[-1]) {
    r.number_p[-1] = '\0';
  } else {
    *r.number_p = '\0';
  }
  *scale = r.read_post;
  *precision = r.num.pre + r.num.multi + r.read_post;
  return 0;
}
