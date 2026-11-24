/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * tochar.cc -- to_char of timestamp, timestamptz and interval (ty_tochar.h):
 * the compile of a format, and the writing of each keyword's field.
 */
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "ty_datetime.h"
#include "ty_tochar.h"

namespace {

/* The keywords. A lowercase one that writes as its uppercase one has its id. */
enum dch_id : uint8_t {
  K_A_D,
  K_A_M,
  K_AD,
  K_AM,
  K_B_C,
  K_BC,
  K_CC,
  K_DAY,
  K_DDD,
  K_DD,
  K_DY,
  K_Day,
  K_Dy,
  K_D,
  K_FF1,
  K_FF2,
  K_FF3,
  K_FF4,
  K_FF5,
  K_FF6,
  K_FX,
  K_HH24,
  K_HH12,
  K_HH,
  K_IDDD,
  K_ID,
  K_IW,
  K_IYYY,
  K_IYY,
  K_IY,
  K_I,
  K_J,
  K_MI,
  K_MM,
  K_MONTH,
  K_MON,
  K_MS,
  K_Month,
  K_Mon,
  K_OF,
  K_P_M,
  K_PM,
  K_Q,
  K_RM,
  K_SSSS,
  K_SS,
  K_TZH,
  K_TZM,
  K_TZ,
  K_US,
  K_WW,
  K_W,
  K_Y_YYY,
  K_YYYY,
  K_YYY,
  K_YY,
  K_Y,
  K_a_d,
  K_a_m,
  K_ad,
  K_am,
  K_b_c,
  K_bc,
  K_day,
  K_dy,
  K_month,
  K_mon,
  K_p_m,
  K_pm,
  K_rm,
  K_tz
};

/* A keyword: its name, and the id it writes as. */
struct keyword_t {
  const char *name;
  uint8_t len;
  uint8_t id;
};

/*
 * PostgreSQL's DCH_keywords, in its order: those that begin alike go longest
 * first, as the first match wins.
 */
const keyword_t k_keywords[] = {
    {"A.D.", 4, K_A_D},    {"A.M.", 4, K_A_M},    {"AD", 2, K_AD},
    {"AM", 2, K_AM},       {"B.C.", 4, K_B_C},    {"BC", 2, K_BC},
    {"CC", 2, K_CC},       {"DAY", 3, K_DAY},     {"DDD", 3, K_DDD},
    {"DD", 2, K_DD},       {"DY", 2, K_DY},       {"Day", 3, K_Day},
    {"Dy", 2, K_Dy},       {"D", 1, K_D},         {"FF1", 3, K_FF1},
    {"FF2", 3, K_FF2},     {"FF3", 3, K_FF3},     {"FF4", 3, K_FF4},
    {"FF5", 3, K_FF5},     {"FF6", 3, K_FF6},     {"FX", 2, K_FX},
    {"HH24", 4, K_HH24},   {"HH12", 4, K_HH12},   {"HH", 2, K_HH},
    {"IDDD", 4, K_IDDD},   {"ID", 2, K_ID},       {"IW", 2, K_IW},
    {"IYYY", 4, K_IYYY},   {"IYY", 3, K_IYY},     {"IY", 2, K_IY},
    {"I", 1, K_I},         {"J", 1, K_J},         {"MI", 2, K_MI},
    {"MM", 2, K_MM},       {"MONTH", 5, K_MONTH}, {"MON", 3, K_MON},
    {"MS", 2, K_MS},       {"Month", 5, K_Month}, {"Mon", 3, K_Mon},
    {"OF", 2, K_OF},       {"P.M.", 4, K_P_M},    {"PM", 2, K_PM},
    {"Q", 1, K_Q},         {"RM", 2, K_RM},       {"SSSSS", 5, K_SSSS},
    {"SSSS", 4, K_SSSS},   {"SS", 2, K_SS},       {"TZH", 3, K_TZH},
    {"TZM", 3, K_TZM},     {"TZ", 2, K_TZ},       {"US", 2, K_US},
    {"WW", 2, K_WW},       {"W", 1, K_W},         {"Y,YYY", 5, K_Y_YYY},
    {"YYYY", 4, K_YYYY},   {"YYY", 3, K_YYY},     {"YY", 2, K_YY},
    {"Y", 1, K_Y},         {"a.d.", 4, K_a_d},    {"a.m.", 4, K_a_m},
    {"ad", 2, K_ad},       {"am", 2, K_am},       {"b.c.", 4, K_b_c},
    {"bc", 2, K_bc},       {"cc", 2, K_CC},       {"day", 3, K_day},
    {"ddd", 3, K_DDD},     {"dd", 2, K_DD},       {"dy", 2, K_dy},
    {"d", 1, K_D},         {"ff1", 3, K_FF1},     {"ff2", 3, K_FF2},
    {"ff3", 3, K_FF3},     {"ff4", 3, K_FF4},     {"ff5", 3, K_FF5},
    {"ff6", 3, K_FF6},     {"fx", 2, K_FX},       {"hh24", 4, K_HH24},
    {"hh12", 4, K_HH12},   {"hh", 2, K_HH},       {"iddd", 4, K_IDDD},
    {"id", 2, K_ID},       {"iw", 2, K_IW},       {"iyyy", 4, K_IYYY},
    {"iyy", 3, K_IYY},     {"iy", 2, K_IY},       {"i", 1, K_I},
    {"j", 1, K_J},         {"mi", 2, K_MI},       {"mm", 2, K_MM},
    {"month", 5, K_month}, {"mon", 3, K_mon},     {"ms", 2, K_MS},
    {"of", 2, K_OF},       {"p.m.", 4, K_p_m},    {"pm", 2, K_pm},
    {"q", 1, K_Q},         {"rm", 2, K_rm},       {"sssss", 5, K_SSSS},
    {"ssss", 4, K_SSSS},   {"ss", 2, K_SS},       {"tzh", 3, K_TZH},
    {"tzm", 3, K_TZM},     {"tz", 2, K_tz},       {"us", 2, K_US},
    {"ww", 2, K_WW},       {"w", 1, K_W},         {"y,yyy", 5, K_Y_YYY},
    {"yyyy", 4, K_YYYY},   {"yyy", 3, K_YYY},     {"yy", 2, K_YY},
    {"y", 1, K_Y},
};
constexpr int k_nkeywords = sizeof k_keywords / sizeof k_keywords[0];

/* The first keyword of each first byte, or -1: made once, from the table. */
struct first_t {
  int16_t at[128];
  first_t() {
    for (int c = 0; c < 128; c++) {
      at[c] = -1;
    }
    for (int i = k_nkeywords - 1; i >= 0; i--) {
      at[(unsigned char)k_keywords[i].name[0]] = (int16_t)i;
    }
  }
};
const first_t k_first;

/* The prefixes and the suffixes. */
enum : uint8_t {
  S_FM = 0x01,
  S_TH = 0x02,
  S_th = 0x04,
  S_SP = 0x08,
  S_TM = 0x10
};

/* A node: a keyword with its prefix and suffix, or a character. */
struct node_t {
  uint8_t is_key; /* a keyword, else a character */
  uint8_t id;     /* the keyword's id */
  uint8_t len;    /* the keyword's length, or the character's bytes */
  uint8_t suffix; /* the prefix and suffix bits */
  char ch[4];     /* the character */
};

} // namespace

struct ty_dch_format_t {
  size_t len; /* the bytes of the format */
  size_t n;   /* the nodes */
  node_t node[1];
};

namespace {

/* The bytes of the UTF-8 character that begins with byte b: 1 to 4. */
inline size_t char_len(unsigned char b) {
  return b < 0x80 ? 1 : (b & 0xe0) == 0xc0 ? 2 : (b & 0xf0) == 0xe0 ? 3 : 4;
}

/* Whether the end - p bytes at p begin with the len at s. */
inline bool starts(const char *p, const char *end, const char *s, size_t len) {
  return (size_t)(end - p) >= len && 0 == memcmp(p, s, len);
}

/* The keyword at p, or NULL: the first of those its first byte begins. */
const keyword_t *find_keyword(const char *p, const char *end) {
  unsigned char c = (unsigned char)*p;

  if (c <= ' ' || c >= '~' || k_first.at[c] < 0) {
    return nullptr;
  }
  for (int i = k_first.at[c]; i < k_nkeywords && k_keywords[i].name[0] == *p;
       i++) {
    if (starts(p, end, k_keywords[i].name, k_keywords[i].len)) {
      return &k_keywords[i];
    }
  }
  return nullptr;
}

/* A prefix at p: FM or TM, either case as PostgreSQL's table has them. */
uint8_t find_prefix(const char *p, const char *end) {
  if (starts(p, end, "FM", 2) || starts(p, end, "fm", 2)) {
    return S_FM;
  }
  if (starts(p, end, "TM", 2) || starts(p, end, "tm", 2)) {
    return S_TM;
  }
  return 0;
}

/* A suffix at p: TH, th or SP. */
uint8_t find_suffix(const char *p, const char *end) {
  if (starts(p, end, "TH", 2)) {
    return S_TH;
  }
  if (starts(p, end, "th", 2)) {
    return S_th;
  }
  if (starts(p, end, "SP", 2)) {
    return S_SP;
  }
  return 0;
}

/* Add a node of the character of n bytes at p. */
inline void add_char(ty_dch_format_t *f, const char *p, size_t n) {
  node_t *d = &f->node[f->n++];

  d->is_key = 0;
  d->len = (uint8_t)n;
  memcpy(d->ch, p, n);
}

} // namespace

/*
 * Compile a format.
 * 1. A prefix FM or TM is taken first; it is lost if no keyword follows it.
 * 2. A keyword, then a suffix after it.
 * 3. Else a "quoted" string: each character of it, \ quoting the next.
 * 4. Else a character; \ before " drops the \.
 */
int ty_dch_compile(const char *fmt, size_t len, ty_dch_format_t **out) {
  ty_dch_format_t *f = (ty_dch_format_t *)ty_alloc(
      sizeof(ty_dch_format_t) + (len ? len - 1 : 0) * sizeof(node_t));

  if (nullptr == f) {
    return ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  const char *p = fmt, *end = fmt + len;

  f->len = len;
  f->n = 0;
  while (p < end) {
    uint8_t prefix = find_prefix(p, end);
    const keyword_t *k;

    if (prefix) {
      p += 2;
      if (p >= end) {
        break;
      }
    }
    if (nullptr != (k = find_keyword(p, end))) {
      node_t *d = &f->node[f->n++];

      p += k->len;
      d->is_key = 1;
      d->id = k->id;
      d->len = k->len;
      d->suffix = prefix;
      if (p < end) {
        uint8_t s = find_suffix(p, end);

        if (s) {
          d->suffix |= s;
          p += 2;
        }
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

        if (n > (size_t)(end - p)) {
          n = (size_t)(end - p);
        }
        add_char(f, p, n);
        p += n;
      }
      continue;
    }
    if ('\\' == *p && p + 1 < end && '"' == p[1]) {
      p++;
    }
    size_t n = char_len((unsigned char)*p);

    if (n > (size_t)(end - p)) {
      n = (size_t)(end - p);
    }
    add_char(f, p, n);
    p += n;
  }
  *out = f;
  return 0;
}

namespace {

/* The fields of a value, as PostgreSQL's TmToChar. */
struct fields_t {
  int64_t hour;
  int sec, min, mday, mon, year, wday, yday;
  long gmtoff;     /* east of Greenwich */
  int32_t fsec;    /* microseconds */
  const char *tzn; /* the zone's abbreviation, or NULL */
};

const char *const k_months_full[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};
const char *const k_months_abbrev[] = {"Jan", "Feb", "Mar", "Apr",
                                       "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec"};
const char *const k_days_full[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday"};
const char *const k_days_abbrev[] = {"Sun", "Mon", "Tue", "Wed",
                                     "Thu", "Fri", "Sat"};
const char *const k_rm_upper[] = {"XII", "XI", "X",  "IX",  "VIII", "VII",
                                  "VI",  "V",  "IV", "III", "II",   "I"};
const char *const k_rm_lower[] = {"xii", "xi", "x",  "ix",  "viii", "vii",
                                  "vi",  "v",  "iv", "iii", "ii",   "i"};

/* PostgreSQL's DCH_MAX_ITEM_SIZ: the bytes of output a byte of format holds. */
constexpr size_t k_item = 12;

/* The output: a text of room for the longest a format can write. */
struct out_t {
  char *base; /* after the header */
  char *p;
};

/* Write v as printf's %0*lld: zero-padded to width, the sign counted. */
void put_int(out_t *o, int64_t v, int width) {
  char buf[24];
  int n = 0;
  uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;

  do {
    buf[n++] = (char)('0' + u % 10);
    u /= 10;
  } while (u);
  int digits = n + (v < 0);

  if (v < 0) {
    *o->p++ = '-';
  }
  for (int i = digits; i < width; i++) {
    *o->p++ = '0';
  }
  while (n) {
    *o->p++ = buf[--n];
  }
}

/* Write the len bytes at s, as printf's %-*s with width w (0 for none). */
void put_str(out_t *o, const char *s, size_t len, size_t w) {
  memcpy(o->p, s, len);
  o->p += len;
  for (size_t i = len; i < w; i++) {
    *o->p++ = ' ';
  }
}

/* Write s cased: 0 as it is, 1 upper, 2 lower; ASCII alone. */
void put_cased(out_t *o, const char *s, int c, size_t w) {
  size_t len = strlen(s);

  for (size_t i = 0; i < len; i++) {
    char ch = s[i];

    if (1 == c && ch >= 'a' && ch <= 'z') {
      ch = (char)(ch - 'a' + 'A');
    } else if (2 == c && ch >= 'A' && ch <= 'Z') {
      ch = (char)(ch - 'A' + 'a');
    }
    o->p[i] = ch;
  }
  o->p += len;
  for (size_t i = len; i < w; i++) {
    *o->p++ = ' ';
  }
}

/*
 * The suffix TH or th after the number just written from start: TH for the
 * teens (a 1 before the last digit), else ST, ND, RD by the last digit.
 */
void put_th(out_t *o, const char *start, int upper) {
  size_t len = (size_t)(o->p - start);
  char last = start[len - 1];
  const char *s;

  if (len > 1 && '1' == start[len - 2]) {
    last = 0;
  }
  s = '1' == last ? "st" : '2' == last ? "nd" : '3' == last ? "rd" : "th";
  o->p[0] = upper ? (char)(s[0] - 'a' + 'A') : s[0];
  o->p[1] = upper ? (char)(s[1] - 'a' + 'A') : s[1];
  o->p += 2;
}

/* A number field: v zero-padded to width (0 with FM), then TH if asked. */
inline void put_num(out_t *o, const node_t *n, int64_t v, int width) {
  char *start = o->p;

  put_int(o, v, (n->suffix & S_FM) ? 0 : width);
  if (n->suffix & (S_TH | S_th)) {
    put_th(o, start, n->suffix & S_TH);
  }
}

/* PostgreSQL's ADJUST_YEAR: no year 0, so 1 BC is year 1 of a date. */
inline int adjust_year(int year, bool is_interval) {
  return is_interval ? year : year <= 0 ? -(year - 1) : year;
}

/* The error of a calendar field in an interval. */
int interval_error() {
  return ty_internal::set_error(
      TY_ERR_INVALID, "invalid format specification for an interval value");
}

/*
 * A name with TM: the host's, in its case, at most (keyword + 2) * 12
 * bytes; or, with no host, the C locale's English, ASCII-cased.
 */
int put_tm_name(out_t *o, const node_t *n, const ty_tochar_locale_t *loc,
                int kind, int i, int c) {
  const char *name;
  size_t len;

  if (nullptr == loc) {
    const char *const *t = TY_TOCHAR_FULL_MONTH == kind     ? k_months_full
                           : TY_TOCHAR_ABBREV_MONTH == kind ? k_months_abbrev
                           : TY_TOCHAR_FULL_DAY == kind     ? k_days_full
                                                            : k_days_abbrev;
    const char *s = t[i];

    /* initcap of an English name is the name */
    put_cased(o, s, TY_TOCHAR_UPPER == c ? 1 : TY_TOCHAR_LOWER == c ? 2 : 0, 0);
    return 0;
  }
  if (0 != loc->name(loc->ctx, kind, i, c, &name, &len)) {
    return -1;
  }
  if (len > (size_t)(n->len + 2) * k_item) {
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "localized string format value too long");
  }
  put_str(o, name, len, 0);
  return 0;
}

/*
 * A name: with TM the host's (put_tm_name), else the English one, cased, and
 * padded to 9 unless FM when pad is set.
 */
int put_name(out_t *o, const node_t *n, const ty_tochar_locale_t *loc, int kind,
             int i, int c, bool pad) {
  if (n->suffix & S_TM) {
    return put_tm_name(o, n, loc, kind, i, c);
  }
  const char *const *t = TY_TOCHAR_FULL_MONTH == kind     ? k_months_full
                         : TY_TOCHAR_ABBREV_MONTH == kind ? k_months_abbrev
                         : TY_TOCHAR_FULL_DAY == kind     ? k_days_full
                                                          : k_days_abbrev;

  put_cased(o, t[i],
            TY_TOCHAR_UPPER == c   ? 1
            : TY_TOCHAR_LOWER == c ? 2
                                   : 0,
            pad && !(n->suffix & S_FM) ? 9 : 0);
  return 0;
}

/* A zone's abbreviation for TZ (as it is) or tz (lower), at most 24 bytes. */
int put_tzn(out_t *o, const fields_t *t, bool lower) {
  if (nullptr == t->tzn) {
    return 0;
  }
  size_t len = strlen(t->tzn);

  if (len > 2 * k_item) {
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "time zone format value too long");
  }
  put_cased(o, t->tzn, lower ? 2 : 0, 0);
  return 0;
}

/*
 * Write each node: a character as it is; a keyword's field, by the cases of
 * PostgreSQL's DCH_to_char. A field of a calendar date in an interval is an
 * error.
 */
int write_fields(const ty_dch_format_t *f, const fields_t *t, bool is_interval,
                 const ty_tochar_locale_t *loc, out_t *o) {
  for (size_t k = 0; k < f->n; k++) {
    const node_t *n = &f->node[k];

    if (!n->is_key) {
      memcpy(o->p, n->ch, n->len);
      o->p += n->len;
      continue;
    }
    bool pm = t->hour % 24 >= 12;
    int64_t i;
    int y;

    switch (n->id) {
    case K_A_M:
    case K_P_M:
      put_str(o, pm ? "P.M." : "A.M.", 4, 0);
      break;
    case K_AM:
    case K_PM:
      put_str(o, pm ? "PM" : "AM", 2, 0);
      break;
    case K_a_m:
    case K_p_m:
      put_str(o, pm ? "p.m." : "a.m.", 4, 0);
      break;
    case K_am:
    case K_pm:
      put_str(o, pm ? "pm" : "am", 2, 0);
      break;
    case K_HH:
    case K_HH12:
      /* the 12-hour clock, even for an interval */
      put_num(o, n, 0 == t->hour % 12 ? 12 : t->hour % 12,
              t->hour >= 0 ? 2 : 3);
      break;
    case K_HH24:
      put_num(o, n, t->hour, t->hour >= 0 ? 2 : 3);
      break;
    case K_MI:
      put_num(o, n, t->min, t->min >= 0 ? 2 : 3);
      break;
    case K_SS:
      put_num(o, n, t->sec, t->sec >= 0 ? 2 : 3);
      break;
    case K_FF1:
    case K_FF2:
    case K_FF3:
    case K_MS:
    case K_FF4:
    case K_FF5:
    case K_FF6:
    case K_US: {
      /* the digits of a fraction: FM does not cut them */
      static const int div[] = {100000, 10000, 1000, 100, 10, 1};
      int d = K_MS == n->id ? 3 : K_US == n->id ? 6 : n->id - K_FF1 + 1;
      char *start = o->p;

      put_int(o, t->fsec / div[d - 1], d);
      if (n->suffix & (S_TH | S_th)) {
        put_th(o, start, n->suffix & S_TH);
      }
      break;
    }
    case K_SSSS: {
      char *start = o->p;

      put_int(o, t->hour * 3600 + t->min * 60 + t->sec, 0);
      if (n->suffix & (S_TH | S_th)) {
        put_th(o, start, n->suffix & S_TH);
      }
      break;
    }
    case K_tz:
    case K_TZ:
      if (is_interval) {
        return interval_error();
      }
      if (0 != put_tzn(o, t, K_tz == n->id)) {
        return -1;
      }
      break;
    case K_TZH:
      if (is_interval) {
        return interval_error();
      }
      *o->p++ = t->gmtoff >= 0 ? '+' : '-';
      put_int(o, abs((int)t->gmtoff) / 3600, 2);
      break;
    case K_TZM:
      if (is_interval) {
        return interval_error();
      }
      put_int(o, (abs((int)t->gmtoff) % 3600) / 60, 2);
      break;
    case K_OF:
      if (is_interval) {
        return interval_error();
      }
      *o->p++ = t->gmtoff >= 0 ? '+' : '-';
      put_int(o, abs((int)t->gmtoff) / 3600, (n->suffix & S_FM) ? 0 : 2);
      if (0 != abs((int)t->gmtoff) % 3600) {
        *o->p++ = ':';
        put_int(o, (abs((int)t->gmtoff) % 3600) / 60, 2);
      }
      break;
    case K_A_D:
    case K_B_C:
    case K_AD:
    case K_BC:
    case K_a_d:
    case K_b_c:
    case K_ad:
    case K_bc: {
      if (is_interval) {
        return interval_error();
      }
      bool bc = t->year <= 0;
      bool dots =
          K_A_D == n->id || K_B_C == n->id || K_a_d == n->id || K_b_c == n->id;
      bool lower = n->id >= K_a_d;
      const char *s =
          dots ? (lower ? (bc ? "b.c." : "a.d.") : (bc ? "B.C." : "A.D."))
               : (lower ? (bc ? "bc" : "ad") : (bc ? "BC" : "AD"));

      put_str(o, s, strlen(s), 0);
      break;
    }
    case K_MONTH:
    case K_Month:
    case K_month:
    case K_MON:
    case K_Mon:
    case K_mon: {
      if (is_interval) {
        return interval_error();
      }
      if (0 == t->mon) {
        break;
      }
      bool full = K_MONTH == n->id || K_Month == n->id || K_month == n->id;
      int c = K_MONTH == n->id || K_MON == n->id   ? TY_TOCHAR_UPPER
              : K_Month == n->id || K_Mon == n->id ? TY_TOCHAR_INITCAP
                                                   : TY_TOCHAR_LOWER;

      if (0 != put_name(o, n, loc,
                        full ? TY_TOCHAR_FULL_MONTH : TY_TOCHAR_ABBREV_MONTH,
                        t->mon - 1, c, full)) {
        return -1;
      }
      break;
    }
    case K_DAY:
    case K_Day:
    case K_day:
    case K_DY:
    case K_Dy:
    case K_dy: {
      if (is_interval) {
        return interval_error();
      }
      bool full = K_DAY == n->id || K_Day == n->id || K_day == n->id;
      int c = K_DAY == n->id || K_DY == n->id   ? TY_TOCHAR_UPPER
              : K_Day == n->id || K_Dy == n->id ? TY_TOCHAR_INITCAP
                                                : TY_TOCHAR_LOWER;

      if (0 != put_name(o, n, loc,
                        full ? TY_TOCHAR_FULL_DAY : TY_TOCHAR_ABBREV_DAY,
                        t->wday, c, full)) {
        return -1;
      }
      break;
    }
    case K_MM:
      put_num(o, n, t->mon, t->mon >= 0 ? 2 : 3);
      break;
    case K_DDD:
      put_num(o, n, t->yday, 3);
      break;
    case K_IDDD:
      put_num(o, n, ty_date2isoyearday(t->year, t->mon, t->mday), 3);
      break;
    case K_DD:
      put_num(o, n, t->mday, 2);
      break;
    case K_D:
      if (is_interval) {
        return interval_error();
      }
      put_num(o, n, t->wday + 1, 0);
      break;
    case K_ID:
      if (is_interval) {
        return interval_error();
      }
      put_num(o, n, 0 == t->wday ? 7 : t->wday, 0);
      break;
    case K_WW:
      put_num(o, n, (t->yday - 1) / 7 + 1, 2);
      break;
    case K_IW:
      put_num(o, n, ty_date2isoweek(t->year, t->mon, t->mday), 2);
      break;
    case K_Q:
      if (0 == t->mon) {
        break;
      }
      put_num(o, n, (t->mon - 1) / 3 + 1, 0);
      break;
    case K_CC:
      /* century 20 is 1901 to 2000; 6 BC is 600 BC to 501 BC */
      i = is_interval   ? t->year / 100
          : t->year > 0 ? (t->year - 1) / 100 + 1
                        : t->year / 100 - 1;
      put_num(o, n, i, i <= 99 && i >= -99 ? (i >= 0 ? 2 : 3) : 0);
      break;
    case K_Y_YYY: {
      char *start = o->p;

      y = adjust_year(t->year, is_interval);
      put_int(o, y / 1000, 0);
      *o->p++ = ',';
      put_int(o, y - (y / 1000) * 1000, 3);
      if (n->suffix & (S_TH | S_th)) {
        put_th(o, start, n->suffix & S_TH);
      }
      break;
    }
    case K_YYYY:
    case K_IYYY:
    case K_YYY:
    case K_IYY:
    case K_YY:
    case K_IY:
    case K_Y:
    case K_I: {
      bool iso =
          K_IYYY == n->id || K_IYY == n->id || K_IY == n->id || K_I == n->id;
      int digits = K_YYYY == n->id || K_IYYY == n->id ? 4
                   : K_YYY == n->id || K_IYY == n->id ? 3
                   : K_YY == n->id || K_IY == n->id   ? 2
                                                      : 1;
      int width = adjust_year(t->year, is_interval) >= 0 ? digits : digits + 1;
      int mod = 4 == digits ? 0 : 3 == digits ? 1000 : 2 == digits ? 100 : 10;

      y = adjust_year(iso ? ty_date2isoyear(t->year, t->mon, t->mday) : t->year,
                      is_interval);
      if (0 != mod) {
        y %= mod;
      }
      /* Y and I are %1d: FM changes nothing */
      put_num(o, n, y, 1 == digits ? 1 : width);
      break;
    }
    case K_RM:
    case K_rm: {
      /* an interval of whole years has month 0 */
      if (0 == t->mon && 0 == t->year) {
        break;
      }
      const char *const *m = K_RM == n->id ? k_rm_upper : k_rm_lower;
      int at = 0 == t->mon  ? (t->year >= 0 ? 0 : 11)
               : t->mon < 0 ? -(t->mon + 1)
                            : 12 - t->mon;

      put_str(o, m[at], strlen(m[at]), (n->suffix & S_FM) ? 0 : 4);
      break;
    }
    case K_W:
      put_num(o, n, (t->mday - 1) / 7 + 1, 0);
      break;
    case K_J:
      put_num(o, n, ty_date2j(t->year, t->mon, t->mday), 0);
      break;
    default:
      /* FX writes nothing */
      break;
    }
  }
  return 0;
}

/*
 * Write a value's fields by a format into a new text.
 * 1. Take room for 12 bytes of output a byte of format, as PostgreSQL does:
 *    no field writes more than 12 a byte of its keyword, prefix and suffix.
 * 2. Write the fields, and set the text's length.
 */
int format_fields(const ty_dch_format_t *f, const fields_t *t, bool is_interval,
                  const ty_tochar_locale_t *loc, ty_varlena_t **out) {
  out_t o;

  if (0 != ty_vl_new(f->len * k_item, out, &o.base)) {
    return -1;
  }
  o.p = o.base;
  if (0 != write_fields(f, t, is_interval, loc, &o)) {
    ty_free(*out);
    *out = nullptr;
    return -1;
  }
  ty_vl_set_len(*out, (size_t)(o.p - o.base));
  return 0;
}

/* The fields of a date and time; its weekday and its day of the year. */
void date_fields(const ty_tm_t *tm, int32_t fsec, fields_t *t) {
  int jd = ty_date2j(tm->year, tm->mon, tm->mday);

  t->year = tm->year;
  t->mon = tm->mon;
  t->mday = tm->mday;
  t->hour = tm->hour;
  t->min = tm->min;
  t->sec = tm->sec;
  t->fsec = fsec;
  t->wday = (jd + 1) % 7;
  t->yday = jd - ty_date2j(tm->year, 1, 1) + 1;
  t->gmtoff = 0;
  t->tzn = nullptr;
}

} // namespace

/* to_char of a timestamp: its fields, with no zone. */
int ty_timestamp_to_char(int64_t ts, const ty_dch_format_t *f,
                         const ty_tochar_locale_t *loc, ty_varlena_t **out) {
  ty_tm_t tm;
  int32_t fsec;
  fields_t t;

  if (0 == f->len || !ty_timestamp_finite(ts)) {
    *out = nullptr;
    return 1;
  }
  if (0 != ty_timestamp_to_tm(ts, &tm, &fsec)) {
    return ty_internal::set_error(TY_ERR_RANGE, "timestamp out of range");
  }
  date_fields(&tm, fsec, &t);
  return format_fields(f, &t, false, loc, out);
}

/* to_char of a timestamptz: its fields in the zone, its offset and name. */
int ty_timestamptz_to_char(int64_t ts, const ty_tz_t *tz,
                           const ty_dch_format_t *f,
                           const ty_tochar_locale_t *loc, ty_varlena_t **out) {
  ty_tm_t tm;
  int32_t fsec;
  int west;
  const char *abbrev;
  fields_t t;

  if (0 == f->len || !ty_timestamp_finite(ts)) {
    *out = nullptr;
    return 1;
  }
  if (0 !=
      ty_timestamptz_to_local(tz, ts, &tm, &fsec, &west, nullptr, &abbrev)) {
    return -1;
  }
  date_fields(&tm, fsec, &t);
  t.gmtoff = -west;
  t.tzn = abbrev;
  return format_fields(f, &t, false, loc, out);
}

/*
 * to_char of an interval: its parts; no weekday, and a day of the year that
 * counts 30 days a month.
 */
int ty_interval_to_char(const ty_interval_t *iv, const ty_dch_format_t *f,
                        const ty_tochar_locale_t *loc, ty_varlena_t **out) {
  ty_itm_t itm;
  fields_t t;

  if (0 == f->len || ty_interval_not_finite(iv)) {
    *out = nullptr;
    return 1;
  }
  ty_interval_to_itm(iv, &itm);
  t.fsec = itm.usec;
  t.sec = itm.sec;
  t.min = itm.min;
  t.hour = itm.hour;
  t.mday = itm.mday;
  t.mon = itm.mon;
  t.year = itm.year;
  t.wday = 0;
  /* PostgreSQL's int, which wraps for an interval of 6 million years */
  t.yday = (int)(uint32_t)(((uint32_t)t.year * 12 + (uint32_t)t.mon) * 30 +
                           (uint32_t)t.mday);
  t.gmtoff = 0;
  t.tzn = nullptr;
  return format_fields(f, &t, true, loc, out);
}
