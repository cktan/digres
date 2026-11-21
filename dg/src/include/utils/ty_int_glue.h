/*
 * ty_int_glue.h -- what digres's integer functions need to call ty.
 *
 * ty's integer operations (ty_int.h) return -1 and set the thread's error
 * when they fail; the functions of int.c and int8.c then raise
 * PostgreSQL's error for it, with the text of the type of the result.
 */
#ifndef TY_INT_GLUE_H
#define TY_INT_GLUE_H

#include "ty_int.h"

/*
 * Raise PostgreSQL's error for the failure of a ty integer call: division
 * by zero, or "<type> out of range" with type "smallint", "integer" or
 * "bigint". It is cold: it runs only on a failure.
 */
static pg_noreturn pg_noinline pg_attribute_cold void
ty_int_raise(const char *type) {
  if (ty_errnum() == TY_ERR_DIVZERO)
    ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("%s out of range", type)));
}

/*
 * Raise PostgreSQL's error for a shift whose count ty refused: a count
 * below 0 or not below the width of the type. It is cold, as the other is.
 */
static pg_noreturn pg_noinline pg_attribute_cold void
ty_int_raise_shift(int bits) {
  ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                  errmsg("shift count out of range for a %d-bit integer "
                         "(0 to %d)",
                         bits, bits - 1)));
}

#endif /* TY_INT_GLUE_H */
