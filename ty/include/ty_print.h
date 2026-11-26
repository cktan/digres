/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_print.h -- a value's text, appended to a std::string, for a test to
 * compare: each type's own text function, as PostgreSQL's output writes
 * the value.
 *
 *   ty_print(v, out)
 *       Append v's text to out: an integer's digits (ty_intN_format), a
 *       float's fewest digits that read back (ty_fpN_format with 1 extra
 *       digit, PostgreSQL's default), NaN, Infinity and -0 as they are; a
 *       bool t or f (ty_bool_out). One for each type: int16_t, int32_t,
 *       int64_t, float, double and bool.
 *
 * tyvec_print (ty_vec.h) writes a vector by it, a row a line.
 */
#ifndef TY_PRINT_H
#define TY_PRINT_H

#include <stdint.h>

#include <string>

void ty_print(int16_t v, std::string &out);
void ty_print(int32_t v, std::string &out);
void ty_print(int64_t v, std::string &out);
void ty_print(float v, std::string &out);
void ty_print(double v, std::string &out);
void ty_print(bool v, std::string &out);

#endif /* TY_PRINT_H */
