/*
 * Portions Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2026, CK Tan: the changes for ty,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 */

/*
 * unicode.cc -- Unicode's normal forms, per
 * https://www.unicode.org/reports/tr15/, and the escapes of unistr()
 * (ty_unicode.h).
 *
 * The normal forms are derived from PostgreSQL's src/common/unicode_norm.c,
 * with its tables, src/unicode/unicode_norm*.h. The functions on a text are
 * ty's.
 */
#include "ty_unicode.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "error.h"
#include "mem.h"

using ty_internal::mem_alloc;
using ty_internal::mem_free;

namespace {

typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef uint32_t uint32;

/* What the quick check says of a character; see UAX #15. */
enum {
  UNICODE_NORM_QC_NO = 0,
  UNICODE_NORM_QC_YES = 1,
  UNICODE_NORM_QC_MAYBE = -1
};

#include "unicode/unicode_norm_hashfunc.h"
#include "unicode/unicode_normprops_table.h"

/* Constants for calculations with Hangul characters. */
const uint32_t SBASE = 0xAC00;
const uint32_t LBASE = 0x1100;
const uint32_t VBASE = 0x1161;
const uint32_t TBASE = 0x11A7;
const uint32_t LCOUNT = 19;
const uint32_t VCOUNT = 21;
const uint32_t TCOUNT = 28;
const uint32_t SCOUNT = LCOUNT * VCOUNT * TCOUNT;

/* The most characters of an array: its bytes are PostgreSQL's MaxAllocSize. */
const size_t MAX_CHARS = 0x3fffffff / sizeof(uint32_t);

/* code with its highest byte first: the key of the perfect hashes. */
inline uint32_t hash_key(uint32_t code) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap32(code);
#else
  return code;
#endif
}

/*
 * The entry of code in the decomposition table, by a perfect hash; NULL if
 * it has none.
 */
const pg_unicode_decomposition *get_code_entry(uint32_t code) {
  uint32_t key = hash_key(code);
  int h = UnicodeDecompInfo.hash(&key);

  /* Out of range is no match, and the hash is sure of one entry only. */
  if (h < 0 || h >= UnicodeDecompInfo.num_decomps ||
      code != UnicodeDecompInfo.decomps[h].codepoint) {
    return nullptr;
  }
  return &UnicodeDecompInfo.decomps[h];
}

/* The combining class of code: 0 if it has no entry. */
uint8_t get_canonical_class(uint32_t code) {
  const pg_unicode_decomposition *entry = get_code_entry(code);

  return entry ? entry->comb_class : 0;
}

/* Whether code is a Hangul syllable, which decomposes by arithmetic. */
bool is_hangul(uint32_t code) { return code >= SBASE && code < SBASE + SCOUNT; }

/* The characters that entry decomposes to, *n; one that is inline is in *x. */
inline const uint32_t *
get_code_decomposition(const pg_unicode_decomposition *entry, uint32_t *x,
                       int *n) {
  *n = DECOMPOSITION_SIZE(entry);
  if (DECOMPOSITION_IS_INLINE(entry)) {
    *x = entry->dec_index;
    return x;
  }
  return &UnicodeDecomp_codepoints[entry->dec_index];
}

/* Whether a character with this entry stays as it is. */
bool is_whole(const pg_unicode_decomposition *entry, bool compat) {
  return nullptr == entry || 0 == DECOMPOSITION_SIZE(entry) ||
         (!compat && DECOMPOSITION_IS_COMPAT(entry));
}

/* How many characters code decomposes to, each decomposed in turn. */
size_t get_decomposed_size(uint32_t code, bool compat) {
  size_t size = 0;
  uint32_t x;
  int n;

  if (is_hangul(code)) {
    return (code - SBASE) % TCOUNT != 0 ? 3 : 2;
  }
  const pg_unicode_decomposition *entry = get_code_entry(code);

  if (is_whole(entry, compat)) {
    return 1;
  }
  const uint32_t *decomp = get_code_decomposition(entry, &x, &n);

  for (int i = 0; i < n; i++) {
    size += get_decomposed_size(decomp[i], compat);
  }
  return size;
}

/* Write what code decomposes to at out; returns the place after it. */
uint32_t *decompose_code(uint32_t code, bool compat, uint32_t *out) {
  uint32_t x;
  int n;

  if (is_hangul(code)) {
    uint32_t sindex = code - SBASE;
    uint32_t tindex = sindex % TCOUNT;

    *out++ = LBASE + sindex / (VCOUNT * TCOUNT);
    *out++ = VBASE + (sindex % (VCOUNT * TCOUNT)) / TCOUNT;
    if (tindex != 0) {
      *out++ = TBASE + tindex;
    }
    return out;
  }
  const pg_unicode_decomposition *entry = get_code_entry(code);

  if (is_whole(entry, compat)) {
    *out++ = code;
    return out;
  }
  const uint32_t *decomp = get_code_decomposition(entry, &x, &n);

  for (int i = 0; i < n; i++) {
    out = decompose_code(decomp[i], compat, out);
  }
  return out;
}

/*
 * The one character that start and code compose to, in *result, if there is
 * one: a Hangul syllable by arithmetic, any other by the inverse table.
 */
bool recompose_code(uint32_t start, uint32_t code, uint32_t *result) {
  if (start >= LBASE && start < LBASE + LCOUNT && code >= VBASE &&
      code < VBASE + VCOUNT) {
    /* L and V make a syllable LV */
    *result = SBASE + ((start - LBASE) * VCOUNT + (code - VBASE)) * TCOUNT;
    return true;
  }
  if (is_hangul(start) && 0 == (start - SBASE) % TCOUNT && code > TBASE &&
      code < TBASE + TCOUNT) {
    /* LV and T make a syllable LVT */
    *result = start + (code - TBASE);
    return true;
  }

  uint32_t key[2] = {hash_key(start), hash_key(code)};
  int h = UnicodeRecompInfo.hash(key);

  if (h < 0 || h >= UnicodeRecompInfo.num_recomps) {
    return false;
  }

  const pg_unicode_decomposition *entry =
      &UnicodeDecompMain[UnicodeRecompInfo.inverse_lookup[h]];

  if (start == UnicodeDecomp_codepoints[entry->dec_index] &&
      code == UnicodeDecomp_codepoints[entry->dec_index + 1]) {
    *result = entry->codepoint;
    return true;
  }
  return false;
}

/* What the quick check says of ch, by the table of one of the C forms. */
inline int qc_is_allowed(const pg_unicode_norminfo &info, uint32_t ch) {
  uint32_t key = hash_key(ch);
  int h = info.hash(&key);

  if (h < 0 || h >= info.num_normprops || ch != info.normprops[h].codepoint) {
    return UNICODE_NORM_QC_YES;
  }
  return info.normprops[h].quickcheck;
}

/*
 * The quick check: whether the characters are in the normal form. MAYBE for
 * the D forms: their tables would be large, and the slow way is fast for
 * them, as it recomposes nothing.
 */
int norm_quickcheck(int form, const uint32_t *chars) {
  uint8_t last = 0;
  int result = UNICODE_NORM_QC_YES;

  if (TY_NORM_NFD == form || TY_NORM_NFKD == form) {
    return UNICODE_NORM_QC_MAYBE;
  }
  for (const uint32_t *p = chars; *p; p++) {
    uint8_t ch_class = get_canonical_class(*p);
    int check;

    if (last > ch_class && ch_class != 0) {
      return UNICODE_NORM_QC_NO;
    }
    check = TY_NORM_NFC == form ? qc_is_allowed(UnicodeNormInfo_NFC_QC, *p)
                                : qc_is_allowed(UnicodeNormInfo_NFKC_QC, *p);
    if (UNICODE_NORM_QC_NO == check) {
      return UNICODE_NORM_QC_NO;
    }
    if (UNICODE_NORM_QC_MAYBE == check) {
      result = UNICODE_NORM_QC_MAYBE;
    }
    last = ch_class;
  }
  return result;
}

/*
 * The code point at p, before end, in *c: returns the place after it. The
 * text is valid UTF-8; one that is not is never read past its end.
 */
inline const unsigned char *utf8_get(const unsigned char *p,
                                     const unsigned char *end, uint32_t *c) {
  if (p[0] < 0x80) {
    *c = p[0];
    return p + 1;
  }
  if (0xc0 == (p[0] & 0xe0) && end - p >= 2) {
    *c = (uint32_t)(p[0] & 0x1f) << 6 | (p[1] & 0x3f);
    return p + 2;
  }
  if (0xe0 == (p[0] & 0xf0) && end - p >= 3) {
    *c = (uint32_t)(p[0] & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 |
         (p[2] & 0x3f);
    return p + 3;
  }
  if (0xf0 == (p[0] & 0xf8) && end - p >= 4) {
    *c = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 |
         (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
    return p + 4;
  }
  *c = 0xfffd;
  return p + 1;
}

/* How many bytes c is in UTF-8. */
size_t utf8_len(uint32_t c) {
  return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

/* Write c at p in UTF-8; returns the place after it. */
unsigned char *utf8_put(uint32_t c, unsigned char *p) {
  switch (utf8_len(c)) {
  case 1:
    *p++ = (unsigned char)c;
    return p;
  case 2:
    *p++ = (unsigned char)(0xc0 | c >> 6);
    break;
  case 3:
    *p++ = (unsigned char)(0xe0 | c >> 12);
    *p++ = (unsigned char)(0x80 | (c >> 6 & 0x3f));
    break;
  default:
    *p++ = (unsigned char)(0xf0 | c >> 18);
    *p++ = (unsigned char)(0x80 | (c >> 12 & 0x3f));
    *p++ = (unsigned char)(0x80 | (c >> 6 & 0x3f));
    break;
  }
  *p++ = (unsigned char)(0x80 | (c & 0x3f));
  return p;
}

/* An array of n characters and a 0; NULL with the thread's error set. */
uint32_t *new_chars(size_t n) {
  uint32_t *chars;

  if (n >= MAX_CHARS) {
    ty_internal::set_error(TY_ERR_RANGE, "requested length too large");
    return nullptr;
  }
  chars = (uint32_t *)mem_alloc((n + 1) * sizeof(uint32_t));
  if (nullptr == chars) {
    ty_internal::set_error(TY_ERR_NOMEM, "out of memory");
  }
  return chars;
}

/*
 * The characters of s, in a new array, and how many in *n: as many as the
 * bytes that start one, at most.
 */
uint32_t *text_chars(const ty_varlena_t *s, size_t *n) {
  const unsigned char *p = (const unsigned char *)ty_vl_data(s);
  const unsigned char *end = p + ty_vl_len(s);
  uint32_t *chars, *out;
  size_t count = 0;

  for (const unsigned char *q = p; q < end; q++) {
    count += 0x80 != (*q & 0xc0);
  }
  chars = new_chars(count);
  if (nullptr == chars) {
    return nullptr;
  }
  for (out = chars; p < end && out < chars + count; out++) {
    p = utf8_get(p, end, out);
  }
  *out = 0;
  *n = (size_t)(out - chars);
  return chars;
}

/* The value of the n hexadecimal digits at p, in *v; false if not all are. */
bool hex_value(const char *p, const char *end, int n, uint32_t *v) {
  *v = 0;
  if (end - p < n) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    uint32_t d;

    if (c >= '0' && c <= '9') {
      d = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      d = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      d = c - 'A' + 10;
    } else {
      return false;
    }
    *v = *v << 4 | d;
  }
  return true;
}

/*
 * The escape at p, a backslash that does not stand before another: its code
 * in *v, and how long it is; 0 if it is not one.
 */
int get_escape(const char *p, const char *end, uint32_t *v) {
  if (hex_value(p + 1, end, 4, v)) {
    return 5;
  }
  if (end - p < 2) {
    return 0;
  }
  switch (p[1]) {
  case 'u':
    return hex_value(p + 2, end, 4, v) ? 6 : 0;
  case '+':
    return hex_value(p + 2, end, 6, v) ? 8 : 0;
  case 'U':
    return hex_value(p + 2, end, 8, v) ? 10 : 0;
  default:
    return 0;
  }
}

/* Free a text that is not given, and say why: -1. */
int unistr_error(ty_varlena_t *v, int *why, int kind, const char *msg) {
  mem_free(v);
  *why = kind;
  return ty_internal::set_error(TY_ERR_INVALID, msg);
}

} // namespace

/* The normal form of a name. */
int ty_norm_form(const char *name, int *form) {
  static const char *const names[] = {"NFC", "NFD", "NFKC", "NFKD"};

  for (int i = 0; i < 4; i++) {
    if (0 == strcasecmp(name, names[i])) {
      *form = i;
      return 0;
    }
  }
  return ty_internal::set_error(TY_ERR_INVALID, "invalid normalization form");
}

/*
 * The normal form of an array of characters.
 * 1. Decompose each character, its parts in turn.
 * 2. Order the marks after each starter by their classes: exchange two that
 *    are not in order, and look back.
 * 3. For a C form, compose each starter with the marks after it that no
 *    mark of its own class or a higher one keeps from it.
 */
uint32_t *ty_norm_chars(int form, const uint32_t *chars) {
  bool compat = TY_NORM_NFKC == form || TY_NORM_NFKD == form;
  bool recompose = TY_NORM_NFC == form || TY_NORM_NFKC == form;
  uint32_t *decomp, *recomp, *out;
  size_t size = 0;

  // 1. Decompose each character, its parts in turn.
  for (const uint32_t *p = chars; *p && size < MAX_CHARS; p++) {
    size += get_decomposed_size(*p, compat);
  }
  decomp = new_chars(size);
  if (nullptr == decomp) {
    return nullptr;
  }
  out = decomp;
  for (const uint32_t *p = chars; *p; p++) {
    out = decompose_code(*p, compat, out);
  }
  *out = 0;
  if (0 == size) {
    return decomp;
  }

  // 2. Order the marks after each starter by their classes.
  for (size_t i = 1; i < size; i++) {
    uint32_t prev = decomp[i - 1];
    uint32_t next = decomp[i];
    uint8_t prev_class = get_canonical_class(prev);
    uint8_t next_class = get_canonical_class(next);

    if (0 == prev_class || 0 == next_class || prev_class <= next_class) {
      continue;
    }
    decomp[i - 1] = next;
    decomp[i] = prev;
    if (i > 1) {
      i -= 2;
    }
  }
  if (!recompose) {
    return decomp;
  }

  // 3. Compose each starter with the marks after it.
  recomp = new_chars(size);
  if (nullptr == recomp) {
    mem_free(decomp);
    return nullptr;
  }

  int last_class = -1; /* this eliminates a special check */
  size_t starter_pos = 0;
  size_t target_pos = 1;
  uint32_t starter = recomp[0] = decomp[0];

  for (size_t i = 1; i < size; i++) {
    uint32_t ch = decomp[i];
    int ch_class = get_canonical_class(ch);
    uint32_t composite;

    if (last_class < ch_class && recompose_code(starter, ch, &composite)) {
      recomp[starter_pos] = composite;
      starter = composite;
    } else if (0 == ch_class) {
      starter_pos = target_pos;
      starter = ch;
      last_class = -1;
      recomp[target_pos++] = ch;
    } else {
      last_class = ch_class;
      recomp[target_pos++] = ch;
    }
  }
  recomp[target_pos] = 0;
  mem_free(decomp);
  return recomp;
}

/* normalize(s, form). */
int ty_text_normalize(const ty_varlena_t *s, int form, ty_varlena_t **out) {
  size_t n, len = 0;
  uint32_t *in = text_chars(s, &n), *chars;
  char *d;
  int rc;

  if (nullptr == in) {
    return -1;
  }
  chars = ty_norm_chars(form, in);
  mem_free(in);
  if (nullptr == chars) {
    return -1;
  }
  for (const uint32_t *p = chars; *p; p++) {
    len += utf8_len(*p);
  }
  rc = ty_vl_new(len, out, &d);
  if (0 == rc) {
    unsigned char *q = (unsigned char *)d;

    for (const uint32_t *p = chars; *p; p++) {
      q = utf8_put(*p, q);
    }
  }
  mem_free(chars);
  return rc;
}

/* s IS form NORMALIZED: the quick check, then the form itself. */
int ty_text_is_normalized(const ty_varlena_t *s, int form, int *r) {
  size_t n, m = 0;
  uint32_t *in = text_chars(s, &n), *chars;
  int check;

  if (nullptr == in) {
    return -1;
  }
  check = norm_quickcheck(form, in);
  if (UNICODE_NORM_QC_MAYBE != check) {
    mem_free(in);
    *r = UNICODE_NORM_QC_YES == check;
    return 0;
  }
  chars = ty_norm_chars(form, in);
  if (nullptr == chars) {
    mem_free(in);
    return -1;
  }
  while (chars[m]) {
    m++;
  }
  *r = n == m && 0 == memcmp(in, chars, n * sizeof(uint32_t));
  mem_free(in);
  mem_free(chars);
  return 0;
}

/* Whether every character is assigned. */
int ty_text_is_assigned(const ty_varlena_t *s) {
  const unsigned char *p = (const unsigned char *)ty_vl_data(s);
  const unsigned char *end = p + ty_vl_len(s);
  uint32_t c;

  while (p < end) {
    p = utf8_get(p, end, &c);
    if (TY_U_UNASSIGNED == ty_u_category(c)) {
      return 0;
    }
  }
  return 1;
}

/*
 * unistr(s). No escape is shorter than its character, so the result is no
 * longer than s.
 * 1. A first surrogate waits for a second one, and nothing else may follow.
 * 2. A double backslash is one; any other is an escape.
 * 3. A byte of no escape is copied.
 */
int ty_text_unistr(const ty_varlena_t *s, ty_varlena_t **out, int *why) {
  static const char bad_pair[] = "invalid Unicode surrogate pair";
  const char *p = ty_vl_data(s);
  const char *end = p + ty_vl_len(s);
  uint32_t first = 0, c;
  ty_varlena_t *v;
  unsigned char *q;
  char *d;

  *why = 0;
  if (0 != ty_vl_new(ty_vl_len(s), &v, &d)) {
    return -1;
  }
  q = (unsigned char *)d;
  while (p < end) {
    if ('\\' != *p || (end - p >= 2 && '\\' == p[1])) {
      // 3. A byte of no escape is copied; a double backslash is one.
      if (first) {
        return unistr_error(v, why, TY_UNISTR_PAIR, bad_pair);
      }
      *q++ = (unsigned char)*p;
      p += '\\' == *p ? 2 : 1;
      continue;
    }

    // 2. An escape.
    int n = get_escape(p, end, &c);

    if (0 == n) {
      return unistr_error(v, why, TY_UNISTR_ESCAPE, "invalid Unicode escape");
    }
    if (0 == c || c > 0x10ffff) {
      char msg[64];

      snprintf(msg, sizeof msg, "invalid Unicode code point: %04X", c);
      return unistr_error(v, why, TY_UNISTR_POINT, msg);
    }
    p += n;

    // 1. A first surrogate waits for a second one.
    bool second = c >= 0xdc00 && c <= 0xdfff;

    if (second != (first != 0)) {
      return unistr_error(v, why, TY_UNISTR_PAIR, bad_pair);
    }
    if (second) {
      c = ((first & 0x3ff) << 10) + 0x10000 + (c & 0x3ff);
      first = 0;
    } else if (c >= 0xd800 && c <= 0xdbff) {
      first = c;
      continue;
    }
    q = utf8_put(c, q);
  }
  if (first) {
    return unistr_error(v, why, TY_UNISTR_PAIR, bad_pair);
  }
  ty_vl_set_len(v, (size_t)((char *)q - d));
  *out = v;
  return 0;
}
