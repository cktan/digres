/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * ty_error.h -- ty's error codes, in C.
 *
 * A code says what kind of thing went wrong, and there are few.
 * A type adds its own kinds (out of range, division by zero) when
 * its operations move into ty; each is appended below.
 *
 * Values are explicit and stable: new codes are appended, and
 * existing values never change.
 */
#ifndef TY_ERROR_H
#define TY_ERROR_H

typedef enum ty_errnum_t {
    TY_OK            = 0,

    TY_ERR_INVALID   = 1,   /* a bad argument */
    TY_ERR_NOMEM     = 2,   /* the alloc function returned NULL */

    /*
     * An exception reached a call's catch-all, which means a bug.
     */
    TY_ERR_EXCEPTION = 3,

    /* The highest code; move it when one is appended. */
    TY_ERR_LAST      = TY_ERR_EXCEPTION
} ty_errnum_t;

#endif /* TY_ERROR_H */
