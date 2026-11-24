/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * dch.h -- the compiled date format that to_char (tochar.cc) and the
 * reverse parse (fromchar.cc) share: the keywords, as PostgreSQL's
 * DCH_keywords, and the nodes of a format.
 */
#ifndef TY_DCH_H
#define TY_DCH_H

#include <stddef.h>
#include <stdint.h>

#include "ty_tochar.h"

namespace ty_internal {
namespace dch {

/* The keywords. A lowercase one that reads and writes as its uppercase one
 * has its id. */
enum id_t : uint8_t {
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

/* The date convention a keyword belongs to, as FromCharDateMode. */
enum mode_t : uint8_t { M_NONE = 0, M_GREGORIAN = 1, M_ISOWEEK = 2 };

/* A keyword: its name, its length, the id it reads and writes as, whether
 * its field is digits, and its date convention. */
struct keyword_t {
  const char *name;
  uint8_t len;
  uint8_t id;
  uint8_t is_digit;
  uint8_t mode;
};

/* PostgreSQL's DCH_keywords, in its order; k_nkeywords of them. */
extern const keyword_t k_keywords[];
extern const int k_nkeywords;

/* The prefixes and the suffixes. */
enum : uint8_t {
  S_FM = 0x01,
  S_TH = 0x02,
  S_th = 0x04,
  S_SP = 0x08,
  S_TM = 0x10
};

/* The kinds of a character node, as NODE_TYPE_CHAR, _SEPARATOR, _SPACE. */
enum : uint8_t { T_CHAR = 0, T_SEPARATOR = 1, T_SPACE = 2 };

/* A node: a keyword with its prefix and suffix, or a character. */
struct node_t {
  uint8_t is_key; /* a keyword, else a character */
  uint8_t id;     /* the keyword's id */
  uint8_t kw;     /* the keyword's place in k_keywords */
  uint8_t len;    /* the keyword's length, or the character's bytes */
  uint8_t suffix; /* the prefix and suffix bits */
  uint8_t type;   /* a character's kind */
  char ch[4];     /* the character */
};

} // namespace dch
} // namespace ty_internal

/* A compiled format: the bytes of its text, and its nodes. */
struct ty_dch_format_t {
  size_t len;
  size_t n;
  ty_internal::dch::node_t node[1];
};

#endif /* TY_DCH_H */
