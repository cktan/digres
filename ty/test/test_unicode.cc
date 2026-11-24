/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * Tests of Unicode's normal forms, unicode_assigned and unistr
 * (ty_unicode.h), by hand and by the vectors of test/gen_unicode.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "doctest.h"
#include "ty_text.h"
#include "ty_unicode.h"
#include "vl.h"

namespace {

/* The bytes that the hex digits h stand for; "-" is empty. */
std::string unhex(const std::string &h) {
  std::string s;

  if ("-" == h) {
    return s;
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    s += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  }
  return s;
}

/* A text that ty made, and its memory freed. */
std::string take(ty_varlena_t *v) {
  std::string s(ty_vl_data(v), ty_vl_len(v));

  ty_free(v);
  return s;
}

/* s in the normal form. */
std::string norm(const std::string &s, int form) {
  vl_t v(s);
  ty_varlena_t *out;

  REQUIRE(0 == ty_text_normalize(v, form, &out));
  return take(out);
}

/* Whether s is in the normal form. */
bool is_norm(const std::string &s, int form) {
  vl_t v(s);
  int r;

  REQUIRE(0 == ty_text_is_normalized(v, form, &r));
  return 0 != r;
}

/* unistr(s), or "ERR why". */
std::string unistr(const std::string &s) {
  vl_t v(s);
  ty_varlena_t *out;
  int why;

  if (0 != ty_text_unistr(v, &out, &why)) {
    return "ERR " + std::to_string(why);
  }
  return take(out);
}

/* The character c, as UTF-8. */
std::string chr(int32_t c) {
  ty_varlena_t *out;

  REQUIRE(0 == ty_text_chr(c, &out));
  return take(out);
}

/* The form that a vector names. */
int form_of(const char *name) {
  int form;

  REQUIRE(0 == ty_norm_form(name, &form));
  return form;
}

} // namespace

// Verify the normal forms, the properties and unistr by hand.
// 1. The names of the forms.
// 2. Composed and decomposed, marks put in order, Hangul, the compatible
//    forms, and the empty text.
// 3. Assigned or not; the classes of some characters.
// 4. unistr: each escape, a pair of surrogates, and each error.
TEST_CASE("normal forms, unicode_assigned and unistr") {
  // 1. The names of the forms.
  int form;
  CHECK(form_of("nfkC") == TY_NORM_NFKC);
  CHECK(-1 == ty_norm_form("NFX", &form));
  CHECK(TY_ERR_INVALID == ty_errnum());

  // 2. The forms.
  CHECK(norm("e\u0301", TY_NORM_NFC) == "\u00e9");
  CHECK(norm("\u00e9", TY_NORM_NFD) == "e\u0301");
  CHECK(norm("q\u0307\u0323", TY_NORM_NFD) == "q\u0323\u0307");
  CHECK(norm("\u1112\u1161\u11ab", TY_NORM_NFC) == "\ud55c");
  CHECK(norm("\ud55c", TY_NORM_NFD) == "\u1112\u1161\u11ab");
  CHECK(norm("\ufb01", TY_NORM_NFKC) == "fi");
  CHECK(norm("\ufb01", TY_NORM_NFC) == "\ufb01");
  CHECK(norm("", TY_NORM_NFKD) == "");
  CHECK(is_norm("\u00e9", TY_NORM_NFC));
  CHECK(!is_norm("e\u0301", TY_NORM_NFC));
  CHECK(is_norm("e\u0301", TY_NORM_NFD));
  CHECK(is_norm("", TY_NORM_NFD));

  // 3. Assigned or not; the classes.
  std::string known = "a\u00e9\U0001f600", unknown = "a\u0378";
  vl_t k(known), u(unknown);
  CHECK(ty_text_is_assigned(k));
  CHECK(!ty_text_is_assigned(u));
  CHECK(ty_u_isalpha(0xe9));
  CHECK(ty_u_islower(0xe9));
  CHECK(ty_u_isupper(0xc9));
  CHECK(ty_u_isdigit(0x665, false));
  CHECK(!ty_u_isdigit(0x665, true));
  CHECK(ty_u_isspace(0x2003));
  CHECK(ty_u_ispunct('$', true));
  CHECK(!ty_u_ispunct('$', false));
  CHECK(!ty_u_isprint('\t'));

  // 4. unistr.
  CHECK(unistr("a\\\\b") == "a\\b");
  CHECK(unistr("\\00e9\\u00E9\\+0000e9\\U000000E9") ==
        "\u00e9\u00e9\u00e9\u00e9");
  CHECK(unistr("\\d83d\\ude00") == "\U0001f600");
  CHECK(unistr("\\d83d") == "ERR " + std::to_string(TY_UNISTR_PAIR));
  CHECK(unistr("\\de00") == "ERR " + std::to_string(TY_UNISTR_PAIR));
  CHECK(unistr("\\x") == "ERR " + std::to_string(TY_UNISTR_ESCAPE));
  CHECK(unistr("\\0000") == "ERR " + std::to_string(TY_UNISTR_POINT));
  CHECK(std::string(ty_errmsg()) == "invalid Unicode code point: 0000");
}

// Verify they are digres's, by what a digres built before they moved gave.
// 1. Read test/unicode_known.txt.
// 2. all: the forms of every character, in runs of 4,096, by their hash.
// 3. assigned: how many of a run are.
// 4. norm: the forms of texts of marks and starters.
// 5. unistr: the text, or the kind of the error.
TEST_CASE("normal forms, unicode_assigned and unistr are digres's") {
  const char *env = getenv("TY_VECTORS");
  std::string dir = env ? env : "test/vectors.txt";
  FILE *fp = fopen(
      (dir.substr(0, dir.rfind('/') + 1) + "unicode_known.txt").c_str(), "r");
  char line[8192];
  int n = 0, bad = 0;

  REQUIRE(nullptr != fp);

  // 1. Read test/unicode_known.txt.
  while (fgets(line, sizeof line, fp)) {
    char f[16], a[2048], b[2048], c[2048];
    int lo, hi;
    std::string got, want;

    if (5 ==
        sscanf(line, "all %15s %d %d => %2047s %2047s", f, &lo, &hi, a, b)) {
      // 2. all: the forms of every character.
      int form = form_of(f), normal = 0;
      std::string all;

      for (int32_t g = lo > 0 ? lo : 1; g <= hi && g <= 0x10ffff; g++) {
        if (g >= 0xd800 && g <= 0xdfff) {
          continue;
        }
        std::string s = chr(g);
        all += (all.empty() ? "" : ",") + norm(s, form);
        normal += is_norm(s, form);
      }
      vl_t v(all);
      want = std::string(a) + " " + b;
      got = std::to_string((int64_t)ty_text_hash_extended(v, 0)) + " " +
            std::to_string(normal);
    } else if (3 == sscanf(line, "assigned %d %d => %2047s", &lo, &hi, a)) {
      // 3. assigned: how many of a run are.
      int count = 0;

      for (int32_t g = lo > 0 ? lo : 1; g <= hi && g <= 0x10ffff; g++) {
        if (g >= 0xd800 && g <= 0xdfff) {
          continue;
        }
        std::string s = chr(g);
        vl_t v(s);
        count += ty_text_is_assigned(v);
      }
      want = a;
      got = std::to_string(count);
    } else if (4 ==
               sscanf(line, "norm %15s %2047s => %2047s %2047s", f, a, b, c)) {
      // 4. norm: the forms of texts.
      std::string s = unhex(a);

      want = unhex(b) + " " + c;
      got = norm(s, form_of(f)) + " " + (is_norm(s, form_of(f)) ? "t" : "f");
    } else if (3 == sscanf(line, "unistr %2047s => %15s %2047s", a, f, b)) {
      // 5. unistr: the text, or the kind of the error.
      got = unistr(unhex(a));
      if (0 == strcmp(f, "ok")) {
        want = unhex(b);
      } else if (0 == strcmp(b, "22023")) {
        want = "ERR " + std::to_string(TY_UNISTR_POINT);
      } else {
        want = got == "ERR 1" || got == "ERR 2" ? got : "a syntax error";
      }
    } else {
      continue;
    }
    n++;
    if (got != want && bad++ < 20) {
      INFO(line);
      CHECK(got == want);
    }
  }
  fclose(fp);
  CHECK(0 == bad);
  CHECK(n > 3500);
}
