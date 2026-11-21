/*-------------------------------------------------------------------------
 *
 * encode.c
 *	  Various data encoding/decoding things.
 *
 * Copyright (c) 2001-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/encode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "varatt.h"
#include "ty_bytea.h"

/*
 * Encoding conversion API.
 * encode_len() and decode_len() compute the amount of space needed, while
 * encode() and decode() perform the actual conversions.  It is okay for
 * the _len functions to return an overestimate, but not an underestimate.
 * (Having said that, large overestimates could cause unnecessary errors,
 * so it's better to get it right.)  The conversion routines write to the
 * buffer at *res and return the true length of their output.
 */
/*
 * Raise PostgreSQL's error for a decode that ty refused: escape text has a
 * syntax error; the others a bad parameter, with ty's message, which is
 * PostgreSQL's (and the hint for the end of base64).
 */
static pg_noreturn void binary_decode_raise(ty_bytea_enc_t enc) {
  if (enc == TY_BYTEA_ENC_ESCAPE)
    ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                    errmsg("invalid input syntax for type %s", "bytea")));
  if (strcmp(ty_errmsg(), "invalid base64 end sequence") == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid base64 end sequence"),
                    errhint("Input data is missing padding, is truncated, or "
                            "is otherwise corrupted.")));
  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("%s", ty_errmsg())));
}

/*
 * SQL functions.
 */

Datum binary_encode(PG_FUNCTION_ARGS) {
  bytea *data = PG_GETARG_BYTEA_PP(0);
  Datum name = PG_GETARG_DATUM(1);
  text *result;
  char *namebuf;
  ty_bytea_enc_t enc;
  size_t datalen;
  size_t resultlen;
  size_t res;

  namebuf = TextDatumGetCString(name);

  if (ty_bytea_encoding(namebuf, &enc) != 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized encoding: \"%s\"", namebuf)));

  datalen = VARSIZE_ANY_EXHDR(data);

  /* ty tells the most the text can take, and writes into the varlena */
  resultlen = ty_bytea_encode_size(enc, (const uint8_t *)VARDATA_ANY(data),
                                   datalen);

  /*
   * resultlen possibly overflows uint32, therefore on 32-bit machines it's
   * unsafe to rely on palloc's internal check.
   */
  if (resultlen > MaxAllocSize - VARHDRSZ)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("result of encoding conversion is too large")));

  result = (text *)palloc(VARHDRSZ + resultlen);

  res = ty_bytea_encode(enc, (const uint8_t *)VARDATA_ANY(data), datalen,
                        VARDATA(result));

  /* Make this FATAL 'cause we've trodden on memory ... */
  if (res > resultlen)
    elog(FATAL, "overflow - encode estimate too small");

  SET_VARSIZE(result, VARHDRSZ + res);

  PG_RETURN_TEXT_P(result);
}

Datum binary_decode(PG_FUNCTION_ARGS) {
  text *data = PG_GETARG_TEXT_PP(0);
  Datum name = PG_GETARG_DATUM(1);
  bytea *result;
  char *namebuf;
  ty_bytea_enc_t enc;
  size_t datalen;
  size_t resultlen;
  size_t res;

  namebuf = TextDatumGetCString(name);

  if (ty_bytea_encoding(namebuf, &enc) != 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("unrecognized encoding: \"%s\"", namebuf)));

  datalen = VARSIZE_ANY_EXHDR(data);

  if (ty_bytea_decode_size(enc, VARDATA_ANY(data), datalen, &resultlen) != 0)
    binary_decode_raise(enc);

  /*
   * resultlen possibly overflows uint32, therefore on 32-bit machines it's
   * unsafe to rely on palloc's internal check.
   */
  if (resultlen > MaxAllocSize - VARHDRSZ)
    ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                    errmsg("result of decoding conversion is too large")));

  result = (bytea *)palloc(VARHDRSZ + resultlen);

  if (ty_bytea_decode(enc, VARDATA_ANY(data), datalen,
                      (uint8_t *)VARDATA(result), &res) != 0)
    binary_decode_raise(enc);

  /* Make this FATAL 'cause we've trodden on memory ... */
  if (res > resultlen)
    elog(FATAL, "overflow - decode estimate too small");

  SET_VARSIZE(result, VARHDRSZ + res);

  PG_RETURN_BYTEA_P(result);
}

/*
 * HEX
 */

/*
 * The hex expansion of each possible byte value (two chars per value).
 */
static const char hextbl[513] = "000102030405060708090a0b0c0d0e0f"
                                "101112131415161718191a1b1c1d1e1f"
                                "202122232425262728292a2b2c2d2e2f"
                                "303132333435363738393a3b3c3d3e3f"
                                "404142434445464748494a4b4c4d4e4f"
                                "505152535455565758595a5b5c5d5e5f"
                                "606162636465666768696a6b6c6d6e6f"
                                "707172737475767778797a7b7c7d7e7f"
                                "808182838485868788898a8b8c8d8e8f"
                                "909192939495969798999a9b9c9d9e9f"
                                "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
                                "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
                                "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                                "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
                                "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                                "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

static const int8 hexlookup[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,
    9,  -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

uint64 hex_encode(const char *src, size_t len, char *dst) {
  const char *end = src + len;

  while (src < end) {
    unsigned char usrc = *((const unsigned char *)src);

    memcpy(dst, &hextbl[2 * usrc], 2);
    src++;
    dst += 2;
  }
  return (uint64)len * 2;
}

static inline bool get_hex(const char *cp, char *out) {
  unsigned char c = (unsigned char)*cp;
  int res = -1;

  if (c < 127)
    res = hexlookup[c];

  *out = (char)res;

  return (res >= 0);
}

uint64 hex_decode(const char *src, size_t len, char *dst) {
  return hex_decode_safe(src, len, dst, NULL);
}

uint64 hex_decode_safe(const char *src, size_t len, char *dst,
                       Node *escontext) {
  const char *s, *srcend;
  char v1, v2, *p;

  srcend = src + len;
  s = src;
  p = dst;
  while (s < srcend) {
    if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') {
      s++;
      continue;
    }
    if (!get_hex(s, &v1))
      ereturn(escontext, 0,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid hexadecimal digit: \"%.*s\"",
                      pg_mblen_range(s, srcend), s)));
    s++;
    if (s >= srcend)
      ereturn(escontext, 0,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid hexadecimal data: odd number of digits")));
    if (!get_hex(s, &v2))
      ereturn(escontext, 0,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid hexadecimal digit: \"%.*s\"",
                      pg_mblen_range(s, srcend), s)));
    s++;
    *p++ = (v1 << 4) | v2;
  }

  return p - dst;
}

/*
 * Escape
 * Minimally escape bytea to text.
 * De-escape text to bytea.
 *
 * We must escape zero bytes and high-bit-set bytes to avoid generating
 * text that might be invalid in the current encoding, or that might
 * change to something else if passed through an encoding conversion
 * (leading to failing to de-escape to the original bytea value).
 * Also of course backslash itself has to be escaped.
 *
 * De-escaping processes \\ and any \### octal
 */

#define VAL(CH)			((CH) - '0')
#define DIG(VAL)		((VAL) + '0')
