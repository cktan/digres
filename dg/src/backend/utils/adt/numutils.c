/*-------------------------------------------------------------------------
 *
 * numutils.c
 *	  utility functions for I/O of built-in numeric types.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/numutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <ctype.h>

#include "common/int.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "ty_int.h"

/*
 * A table of all two-digit numbers. This is used to speed up decimal digit
 * generation by copying pairs of digits into the final output.
 */
static const char DIGIT_TABLE[201] = "00"
                                     "01"
                                     "02"
                                     "03"
                                     "04"
                                     "05"
                                     "06"
                                     "07"
                                     "08"
                                     "09"
                                     "10"
                                     "11"
                                     "12"
                                     "13"
                                     "14"
                                     "15"
                                     "16"
                                     "17"
                                     "18"
                                     "19"
                                     "20"
                                     "21"
                                     "22"
                                     "23"
                                     "24"
                                     "25"
                                     "26"
                                     "27"
                                     "28"
                                     "29"
                                     "30"
                                     "31"
                                     "32"
                                     "33"
                                     "34"
                                     "35"
                                     "36"
                                     "37"
                                     "38"
                                     "39"
                                     "40"
                                     "41"
                                     "42"
                                     "43"
                                     "44"
                                     "45"
                                     "46"
                                     "47"
                                     "48"
                                     "49"
                                     "50"
                                     "51"
                                     "52"
                                     "53"
                                     "54"
                                     "55"
                                     "56"
                                     "57"
                                     "58"
                                     "59"
                                     "60"
                                     "61"
                                     "62"
                                     "63"
                                     "64"
                                     "65"
                                     "66"
                                     "67"
                                     "68"
                                     "69"
                                     "70"
                                     "71"
                                     "72"
                                     "73"
                                     "74"
                                     "75"
                                     "76"
                                     "77"
                                     "78"
                                     "79"
                                     "80"
                                     "81"
                                     "82"
                                     "83"
                                     "84"
                                     "85"
                                     "86"
                                     "87"
                                     "88"
                                     "89"
                                     "90"
                                     "91"
                                     "92"
                                     "93"
                                     "94"
                                     "95"
                                     "96"
                                     "97"
                                     "98"
                                     "99";

/*
 * Convert input string to a signed 16 bit integer.  Input strings may be
 * expressed in base-10, hexadecimal, octal, or binary format, all of which
 * can be prefixed by an optional sign character, either '+' (the default) or
 * '-' for negative numbers.  Hex strings are recognized by the digits being
 * prefixed by 0x or 0X while octal strings are recognized by the 0o or 0O
 * prefix.  The binary representation is recognized by the 0b or 0B prefix.
 *
 * Allows any number of leading or trailing whitespace characters.  Digits may
 * optionally be separated by a single underscore character.  These can only
 * come between digits and not before or after the digits.  Underscores have
 * no effect on the return value and are supported only to assist in improving
 * the human readability of the input strings.
 *
 * pg_strtoint16() will throw ereport() upon bad input format or overflow;
 * while pg_strtoint16_safe() instead returns such complaints in *escontext,
 * if it's an ErrorSaveContext.
 *
 * NB: Accumulate input as an unsigned number, to deal with two's complement
 * representation of the most negative number, which can't be represented as a
 * signed positive number.
 */
int16 pg_strtoint16(const char *s) { return pg_strtoint16_safe(s, NULL); }

int16 pg_strtoint16_safe(const char *s, Node *escontext) {
  int16 result;

  /* ty reads the text: decimal, hex, octal and binary, with underscores */
  if (ty_int16_parse_z(s, &result) == 0)
    return result;

  if (ty_errnum() == TY_ERR_RANGE)
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, "smallint")));

  ereturn(escontext, 0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "smallint", s)));
}

/*
 * Convert input string to a signed 32 bit integer.  Input strings may be
 * expressed in base-10, hexadecimal, octal, or binary format, all of which
 * can be prefixed by an optional sign character, either '+' (the default) or
 * '-' for negative numbers.  Hex strings are recognized by the digits being
 * prefixed by 0x or 0X while octal strings are recognized by the 0o or 0O
 * prefix.  The binary representation is recognized by the 0b or 0B prefix.
 *
 * Allows any number of leading or trailing whitespace characters.  Digits may
 * optionally be separated by a single underscore character.  These can only
 * come between digits and not before or after the digits.  Underscores have
 * no effect on the return value and are supported only to assist in improving
 * the human readability of the input strings.
 *
 * pg_strtoint32() will throw ereport() upon bad input format or overflow;
 * while pg_strtoint32_safe() instead returns such complaints in *escontext,
 * if it's an ErrorSaveContext.
 *
 * NB: Accumulate input as an unsigned number, to deal with two's complement
 * representation of the most negative number, which can't be represented as a
 * signed positive number.
 */
int32 pg_strtoint32(const char *s) { return pg_strtoint32_safe(s, NULL); }

int32 pg_strtoint32_safe(const char *s, Node *escontext) {
  int32 result;

  /* ty reads the text: decimal, hex, octal and binary, with underscores */
  if (ty_int32_parse_z(s, &result) == 0)
    return result;

  if (ty_errnum() == TY_ERR_RANGE)
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, "integer")));

  ereturn(escontext, 0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "integer", s)));
}

/*
 * Convert input string to a signed 64 bit integer.  Input strings may be
 * expressed in base-10, hexadecimal, octal, or binary format, all of which
 * can be prefixed by an optional sign character, either '+' (the default) or
 * '-' for negative numbers.  Hex strings are recognized by the digits being
 * prefixed by 0x or 0X while octal strings are recognized by the 0o or 0O
 * prefix.  The binary representation is recognized by the 0b or 0B prefix.
 *
 * Allows any number of leading or trailing whitespace characters.  Digits may
 * optionally be separated by a single underscore character.  These can only
 * come between digits and not before or after the digits.  Underscores have
 * no effect on the return value and are supported only to assist in improving
 * the human readability of the input strings.
 *
 * pg_strtoint64() will throw ereport() upon bad input format or overflow;
 * while pg_strtoint64_safe() instead returns such complaints in *escontext,
 * if it's an ErrorSaveContext.
 *
 * NB: Accumulate input as an unsigned number, to deal with two's complement
 * representation of the most negative number, which can't be represented as a
 * signed positive number.
 */
int64 pg_strtoint64(const char *s) { return pg_strtoint64_safe(s, NULL); }

int64 pg_strtoint64_safe(const char *s, Node *escontext) {
  int64 result;

  /* ty reads the text: decimal, hex, octal and binary, with underscores */
  if (ty_int64_parse_z(s, &result) == 0)
    return result;

  if (ty_errnum() == TY_ERR_RANGE)
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, "bigint")));

  ereturn(escontext, 0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "bigint", s)));
}

/*
 * Convert input string to an unsigned 32 bit integer.
 *
 * Allows any number of leading or trailing whitespace characters.
 *
 * If endloc isn't NULL, store a pointer to the rest of the string there,
 * so that caller can parse the rest.  Otherwise, it's an error if anything
 * but whitespace follows.
 *
 * typname is what is reported in error messages.
 *
 * If escontext points to an ErrorSaveContext node, that is filled instead
 * of throwing an error; the caller must check SOFT_ERROR_OCCURRED()
 * to detect errors.
 */
uint32 uint32in_subr(const char *s, char **endloc, const char *typname,
                     Node *escontext) {
  uint32 result;
  unsigned long cvt;
  char *endptr;

  errno = 0;
  cvt = strtoul(s, &endptr, 0);

  /*
   * strtoul() normally only sets ERANGE.  On some systems it may also set
   * EINVAL, which simply means it couldn't parse the input string.  Be sure
   * to report that the same way as the standard error indication (that
   * endptr == s).
   */
  if ((errno && errno != ERANGE) || endptr == s)
    ereturn(escontext, 0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", typname, s)));

  if (errno == ERANGE)
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, typname)));

  if (endloc) {
    /* caller wants to deal with rest of string */
    *endloc = endptr;
  } else {
    /* allow only whitespace after number */
    while (*endptr && isspace((unsigned char)*endptr))
      endptr++;
    if (*endptr)
      ereturn(escontext, 0,
              (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
               errmsg("invalid input syntax for type %s: \"%s\"", typname, s)));
  }

  result = (uint32)cvt;

  /*
   * Cope with possibility that unsigned long is wider than uint32, in which
   * case strtoul will not raise an error for some values that are out of
   * the range of uint32.
   *
   * For backwards compatibility, we want to accept inputs that are given
   * with a minus sign, so allow the input value if it matches after either
   * signed or unsigned extension to long.
   *
   * To ensure consistent results on 32-bit and 64-bit platforms, make sure
   * the error message is the same as if strtoul() had returned ERANGE.
   */
#if PG_UINT32_MAX != ULONG_MAX
  if (cvt != (unsigned long)result && cvt != (unsigned long)((int)result))
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, typname)));
#endif

  return result;
}

/*
 * Convert input string to an unsigned 64 bit integer.
 *
 * Allows any number of leading or trailing whitespace characters.
 *
 * If endloc isn't NULL, store a pointer to the rest of the string there,
 * so that caller can parse the rest.  Otherwise, it's an error if anything
 * but whitespace follows.
 *
 * typname is what is reported in error messages.
 *
 * If escontext points to an ErrorSaveContext node, that is filled instead
 * of throwing an error; the caller must check SOFT_ERROR_OCCURRED()
 * to detect errors.
 */
uint64 uint64in_subr(const char *s, char **endloc, const char *typname,
                     Node *escontext) {
  uint64 result;
  char *endptr;

  errno = 0;
  result = strtou64(s, &endptr, 0);

  /*
   * strtoul[l] normally only sets ERANGE.  On some systems it may also set
   * EINVAL, which simply means it couldn't parse the input string.  Be sure
   * to report that the same way as the standard error indication (that
   * endptr == s).
   */
  if ((errno && errno != ERANGE) || endptr == s)
    ereturn(escontext, 0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("invalid input syntax for type %s: \"%s\"", typname, s)));

  if (errno == ERANGE)
    ereturn(escontext, 0,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("value \"%s\" is out of range for type %s", s, typname)));

  if (endloc) {
    /* caller wants to deal with rest of string */
    *endloc = endptr;
  } else {
    /* allow only whitespace after number */
    while (*endptr && isspace((unsigned char)*endptr))
      endptr++;
    if (*endptr)
      ereturn(escontext, 0,
              (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
               errmsg("invalid input syntax for type %s: \"%s\"", typname, s)));
  }

  return result;
}

/*
 * pg_itoa: converts a signed 16-bit integer to its string representation
 * and returns strlen(a).
 *
 * Caller must ensure that 'a' points to enough memory to hold the result
 * (at least 7 bytes, counting a leading sign and trailing NUL).
 *
 * It doesn't seem worth implementing this separately.
 */
int pg_itoa(int16 i, char *a) { return (int)ty_int16_format(i, a); }

/*
 * pg_ultoa_n: converts an unsigned 32-bit integer to its string representation,
 * not NUL-terminated, and returns the length of that string representation
 *
 * Caller must ensure that 'a' points to enough memory to hold the result (at
 * least 10 bytes)
 */
int pg_ultoa_n(uint32 value, char *a) { return (int)ty_uint32_format_n(value, a); }

/*
 * pg_ltoa: converts a signed 32-bit integer to its string representation and
 * returns strlen(a).
 *
 * It is the caller's responsibility to ensure that a is at least 12 bytes long,
 * which is enough room to hold a minus sign, a maximally long int32, and the
 * above terminating NUL.
 */
int pg_ltoa(int32 value, char *a) { return (int)ty_int32_format(value, a); }

/*
 * Get the decimal representation, not NUL-terminated, and return the length of
 * same.  Caller must ensure that a points to at least MAXINT8LEN bytes.
 */
int pg_ulltoa_n(uint64 value, char *a) { return (int)ty_uint64_format_n(value, a); }

/*
 * pg_lltoa: converts a signed 64-bit integer to its string representation and
 * returns strlen(a).
 *
 * Caller must ensure that 'a' points to enough memory to hold the result
 * (at least MAXINT8LEN + 1 bytes, counting a leading sign and trailing NUL).
 */
int pg_lltoa(int64 value, char *a) { return (int)ty_int64_format(value, a); }

/*
 * pg_ultostr_zeropad
 *		Converts 'value' into a decimal string representation stored at
 *'str'. 'minwidth' specifies the minimum width of the result; any extra space
 *		is filled up by prefixing the number with zeros.
 *
 * Returns the ending address of the string result (the last character written
 * plus 1).  Note that no NUL terminator is written.
 *
 * The intended use-case for this function is to build strings that contain
 * multiple individual numbers, for example:
 *
 *	str = pg_ultostr_zeropad(str, hours, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, mins, 2);
 *	*str++ = ':';
 *	str = pg_ultostr_zeropad(str, secs, 2);
 *	*str = '\0';
 *
 * Note: Caller must ensure that 'str' points to enough memory to hold the
 * result.
 */
char *pg_ultostr_zeropad(char *str, uint32 value, int32 minwidth) {
  int len;

  Assert(minwidth > 0);

  if (value < 100 && minwidth == 2) /* Short cut for common case */
  {
    memcpy(str, DIGIT_TABLE + value * 2, 2);
    return str + 2;
  }

  len = pg_ultoa_n(value, str);
  if (len >= minwidth)
    return str + len;

  memmove(str + minwidth - len, str, len);
  memset(str, '0', minwidth - len);
  return str + minwidth;
}

/*
 * pg_ultostr
 *		Converts 'value' into a decimal string representation stored at
 *'str'.
 *
 * Returns the ending address of the string result (the last character written
 * plus 1).  Note that no NUL terminator is written.
 *
 * The intended use-case for this function is to build strings that contain
 * multiple individual numbers, for example:
 *
 *	str = pg_ultostr(str, a);
 *	*str++ = ' ';
 *	str = pg_ultostr(str, b);
 *	*str = '\0';
 *
 * Note: Caller must ensure that 'str' points to enough memory to hold the
 * result.
 */
char *pg_ultostr(char *str, uint32 value) {
  int len = pg_ultoa_n(value, str);

  return str + len;
}
