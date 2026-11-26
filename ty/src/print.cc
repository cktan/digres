/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * print.cc -- a value's text, for tests (include/ty_print.h): each type's
 * own text function.
 */
#include "ty_print.h"

#include "ty_bool.h"
#include "ty_float.h"
#include "ty_int.h"

/* An int16's digits; see ty_print.h. */
void ty_print(int16_t v, std::string &out) {
  char buf[TY_INT16_TEXT_SIZE];
  out.append(buf, ty_int16_format(v, buf));
}

/* An int32's digits; see ty_print.h. */
void ty_print(int32_t v, std::string &out) {
  char buf[TY_INT32_TEXT_SIZE];
  out.append(buf, ty_int32_format(v, buf));
}

/* An int64's digits; see ty_print.h. */
void ty_print(int64_t v, std::string &out) {
  char buf[TY_INT64_TEXT_SIZE];
  out.append(buf, ty_int64_format(v, buf));
}

/* A float's fewest digits that read back; see ty_print.h. */
void ty_print(float v, std::string &out) {
  char buf[TY_FLOAT_TEXT_SIZE + 1];
  out.append(buf, ty_fp32_format(v, 1, buf));
}

/* A double's fewest digits that read back; see ty_print.h. */
void ty_print(double v, std::string &out) {
  char buf[TY_FLOAT_TEXT_SIZE + 1];
  out.append(buf, ty_fp64_format(v, 1, buf));
}

/* A bool, t or f; see ty_print.h. */
void ty_print(bool v, std::string &out) { out += ty_bool_out(v); }
