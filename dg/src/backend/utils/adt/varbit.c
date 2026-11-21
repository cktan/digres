/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  Functions for the SQL datatypes BIT() and BIT VARYING().
 *
 * The data structure contains the following elements:
 *   header  -- length of the whole data structure (incl header)
 *              in bytes (as with all varying length datatypes)
 *   data section -- private data section for the bits data structures
 *     bitlength -- length of the bit string in bits
 *     bitdata   -- bit string, most significant byte first
 *
 * The length of the bitdata vector should always be exactly as many
 * bytes as are needed for the given bitlength.  If the bitlength is
 * not a multiple of 8, the extra low-order padding bits of the last
 * byte must be zeroes.
 *
 * attypmod is defined as the length of the bit string in bits, or for
 * varying bits the maximum length.
 *
 * Code originally contributed by Adriaan Joubert.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varbit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/varbit.h"
#include "ty_bits.h"

#define HEXDIG(z)	 ((z)<10 ? ((z)+'0') : ((z)-10+'A'))

/* Mask off any bits that should be zero in the last byte of a bitstring */
#define VARBIT_PAD(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*(VARBITS(vb) + VARBITBYTES(vb) - 1) &= BITMASK << pad_; \
	} while (0)

/*
 * Many functions work byte-by-byte, so they have a pointer handy to the
 * last-plus-one byte, which saves a cycle or two.
 */
#define VARBIT_PAD_LAST(vb, ptr) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*((ptr) - 1) &= BITMASK << pad_; \
	} while (0)

/* Assert proper padding of a bitstring */
#ifdef USE_ASSERT_CHECKING
#define VARBIT_CORRECTLY_PADDED(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		Assert(pad_ == 0 || \
			   (*(VARBITS(vb) + VARBITBYTES(vb) - 1) & ~(BITMASK << pad_)) == 0); \
	} while (0)
#else
#define VARBIT_CORRECTLY_PADDED(vb) ((void) 0)
#endif

static VarBit *bit_catenate(VarBit *arg1, VarBit *arg2);
static VarBit *bitsubstring(VarBit *arg, int32 s, int32 l,
                            bool length_not_specified);
static VarBit *bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl);

/*
 * common code for bittypmodin and varbittypmodin
 */
static int32 anybit_typmodin(ArrayType *ta, const char *typename_) {
  int32 typmod;
  int32 *tl;
  int n;

  tl = ArrayGetIntegerTypmods(ta, &n);

  /*
   * we're not too tense about good error message here because grammar
   * shouldn't allow wrong number of modifiers for BIT
   */
  if (n != 1)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid type modifier")));

  if (*tl < 1)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("length for type %s must be at least 1", typename_)));
  if (*tl > (MaxAttrSize * BITS_PER_BYTE))
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("length for type %s cannot exceed %d", typename_,
                           MaxAttrSize * BITS_PER_BYTE)));

  typmod = *tl;

  return typmod;
}

/*
 * common code for bittypmodout and varbittypmodout
 */
static char *anybit_typmodout(int32 typmod) {
  char *res = (char *)palloc(64);

  if (typmod >= 0)
    snprintf(res, 64, "(%d)", typmod);
  else
    *res = '\0';

  return res;
}

/*
 * A bit string of nbits bits, whose size is set, for ty to fill: ty writes
 * every byte, and the pad, so there is no zeroing here.
 */
static VarBit *varbit_make(int32 nbits) {
  Size len = VARBITTOTALLEN(nbits);
  VarBit *result = (VarBit *)palloc(len);

  SET_VARSIZE(result, len);
  VARBITLEN(result) = nbits;
  return result;
}

/* Make the bit string's size that of nbits bits, after ty has filled it. */
static VarBit *varbit_shrink(VarBit *result, int32 nbits) {
  SET_VARSIZE(result, VARBITTOTALLEN(nbits));
  VARBITLEN(result) = nbits;
  return result;
}

/*
 * Raise PostgreSQL's error for a ty bit string call that failed, with ty's
 * message, which is PostgreSQL's text, and the SQLSTATE of the failure's kind.
 */
static pg_noreturn void varbit_ty_error(int sqlstate_invalid,
                                        int sqlstate_range) {
  ereport(ERROR,
          (errcode(ty_errnum() == TY_ERR_INVALID ? sqlstate_invalid
                                                 : sqlstate_range),
           errmsg("%s", ty_errmsg())));
}


/*
 * bit_in -
 *	  converts a char string to the internal representation of a bitstring.
 *		  The length is determined by the number of bits required plus
 *		  VARHDRSZ bytes or from atttypmod.
 */
Datum bit_in(PG_FUNCTION_ARGS) {
  char *input_string = PG_GETARG_CSTRING(0);
  int32 atttypmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  VarBit *result;
  int32 bitlen;
  size_t slen = strlen(input_string);
  uint8 *buf = (uint8 *)palloc(ty_bits_parse_room(slen));

  /* ty reads the text: binary digits, or hex after an x */
  if (ty_bits_parse(input_string, slen, buf, &bitlen) != 0) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereturn(escontext, (Datum)0,
              (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
               errmsg("%s", ty_errmsg())));
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("%s", ty_errmsg())));
  }

  if (atttypmod <= 0)
    atttypmod = bitlen;
  else if (bitlen != atttypmod)
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
             errmsg("bit string length %d does not match type bit(%d)", bitlen,
                    atttypmod)));

  result = varbit_make(atttypmod);
  memcpy(VARBITS(result), buf, VARBITBYTES(result));
  /* the pad of a string cut short must be zero */
  VARBIT_PAD(result);
  pfree(buf);

  PG_RETURN_VARBIT_P(result);
}

Datum bit_out(PG_FUNCTION_ARGS) {
#if 1
  /* same as varbit output */
  return varbit_out(fcinfo);
#else

  /*
   * This is how one would print a hex string, in case someone wants to
   * write a formatting function.
   */
  VarBit *s = PG_GETARG_VARBIT_P(0);
  char *result, *r;
  bits8 *sp;
  int i, len, bitlen;

  /* Assertion to help catch any bit functions that don't pad correctly */
  VARBIT_CORRECTLY_PADDED(s);

  bitlen = VARBITLEN(s);
  len = (bitlen + 3) / 4;
  result = (char *)palloc(len + 2);
  sp = VARBITS(s);
  r = result;
  *r++ = 'X';
  /* we cheat by knowing that we store full bytes zero padded */
  for (i = 0; i < len; i += 2, sp++) {
    *r++ = HEXDIG((*sp) >> 4);
    *r++ = HEXDIG((*sp) & 0xF);
  }

  /*
   * Go back one step if we printed a hex number that was not part of the
   * bitstring anymore
   */
  if (i > len)
    r--;
  *r = '\0';

  PG_RETURN_CSTRING(result);
#endif
}

/*
 *		bit_recv			- converts external binary
 *format to bit
 */
Datum bit_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 atttypmod = PG_GETARG_INT32(2);
  VarBit *result;
  int len, bitlen;

  bitlen = pq_getmsgint(buf, sizeof(int32));
  if (bitlen < 0 || bitlen > VARBITMAXLEN)
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid length in external bit string")));

  /*
   * Sometimes atttypmod is not supplied. If it is supplied we need to make
   * sure that the bitstring fits.
   */
  if (atttypmod > 0 && bitlen != atttypmod)
    ereport(ERROR, (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                    errmsg("bit string length %d does not match type bit(%d)",
                           bitlen, atttypmod)));

  len = VARBITTOTALLEN(bitlen);
  result = (VarBit *)palloc(len);
  SET_VARSIZE(result, len);
  VARBITLEN(result) = bitlen;

  pq_copymsgbytes(buf, VARBITS(result), VARBITBYTES(result));

  /* Make sure last byte is correctly zero-padded */
  VARBIT_PAD(result);

  PG_RETURN_VARBIT_P(result);
}

/*
 *		bit_send			- converts bit to binary format
 */
Datum bit_send(PG_FUNCTION_ARGS) {
  /* Exactly the same as varbit_send, so share code */
  return varbit_send(fcinfo);
}

/*
 * bit()
 * Converts a bit() type to a specific internal length.
 * len is the bitlength specified in the column definition.
 *
 * If doing implicit cast, raise error when source data is wrong length.
 * If doing explicit cast, silently truncate or zero-pad to specified length.
 */
Datum bit(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  int32 len = PG_GETARG_INT32(1);
  bool isExplicit = PG_GETARG_BOOL(2);
  VarBit *result;

  /* no change if the length is not wanted, or is the string's own */
  if (len <= 0 || len > VARBITMAXLEN || len == VARBITLEN(arg))
    PG_RETURN_VARBIT_P(arg);

  if (!isExplicit)
    ereport(ERROR, (errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
                    errmsg("bit string length %d does not match type bit(%d)",
                           VARBITLEN(arg), len)));

  result = varbit_make(len);
  ty_bits_resize(VARBITS(arg), VARBITLEN(arg), len, VARBITS(result));

  PG_RETURN_VARBIT_P(result);
}

Datum bittypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anybit_typmodin(ta, "bit"));
}

Datum bittypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anybit_typmodout(typmod));
}

/*
 * varbit_in -
 *	  converts a string to the internal representation of a bitstring.
 *		This is the same as bit_in except that atttypmod is taken as
 *		the maximum length, not the exact length to force the bitstring
 *to.
 */
Datum varbit_in(PG_FUNCTION_ARGS) {
  char *input_string = PG_GETARG_CSTRING(0);
  int32 atttypmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  VarBit *result;
  int32 bitlen;
  size_t slen = strlen(input_string);
  uint8 *buf = (uint8 *)palloc(ty_bits_parse_room(slen));

  /* ty reads the text: binary digits, or hex after an x */
  if (ty_bits_parse(input_string, slen, buf, &bitlen) != 0) {
    if (ty_errnum() == TY_ERR_RANGE)
      ereturn(escontext, (Datum)0,
              (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
               errmsg("%s", ty_errmsg())));
    ereturn(escontext, (Datum)0,
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
             errmsg("%s", ty_errmsg())));
  }

  if (atttypmod <= 0)
    atttypmod = bitlen;
  else if (bitlen > atttypmod)
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
         errmsg("bit string too long for type bit varying(%d)", atttypmod)));

  result = varbit_make(Min(bitlen, atttypmod));
  memcpy(VARBITS(result), buf, VARBITBYTES(result));
  /* the pad of a string cut short must be zero */
  VARBIT_PAD(result);
  pfree(buf);

  PG_RETURN_VARBIT_P(result);
}

/*
 * varbit_out -
 *	  Prints the string as bits to preserve length accurately
 *
 * XXX varbit_recv() and hex input to varbit_in() can load a value that this
 * cannot emit.  Consider using hex output for such values.
 */
Datum varbit_out(PG_FUNCTION_ARGS) {
  VarBit *s = PG_GETARG_VARBIT_P(0);
  char *result = (char *)palloc(VARBITLEN(s) + 1);

  ty_bits_format(VARBITS(s), VARBITLEN(s), result);

  PG_RETURN_CSTRING(result);
}

/*
 *		varbit_recv			- converts external binary
 *format to varbit
 *
 * External format is the bitlen as an int32, then the byte array.
 */
Datum varbit_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);

#ifdef NOT_USED
  Oid typelem = PG_GETARG_OID(1);
#endif
  int32 atttypmod = PG_GETARG_INT32(2);
  VarBit *result;
  int len, bitlen;

  bitlen = pq_getmsgint(buf, sizeof(int32));
  if (bitlen < 0 || bitlen > VARBITMAXLEN)
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid length in external bit string")));

  /*
   * Sometimes atttypmod is not supplied. If it is supplied we need to make
   * sure that the bitstring fits.
   */
  if (atttypmod > 0 && bitlen > atttypmod)
    ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
                    errmsg("bit string too long for type bit varying(%d)",
                           atttypmod)));

  len = VARBITTOTALLEN(bitlen);
  result = (VarBit *)palloc(len);
  SET_VARSIZE(result, len);
  VARBITLEN(result) = bitlen;

  pq_copymsgbytes(buf, VARBITS(result), VARBITBYTES(result));

  /* Make sure last byte is correctly zero-padded */
  VARBIT_PAD(result);

  PG_RETURN_VARBIT_P(result);
}

/*
 *		varbit_send			- converts varbit to binary
 *format
 */
Datum varbit_send(PG_FUNCTION_ARGS) {
  VarBit *s = PG_GETARG_VARBIT_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendint32(&buf, VARBITLEN(s));
  pq_sendbytes(&buf, VARBITS(s), VARBITBYTES(s));
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * varbit_support()
 *
 * Planner support function for the varbit() length coercion function.
 *
 * Currently, the only interesting thing we can do is flatten calls that set
 * the new maximum length >= the previous maximum length.  We can ignore the
 * isExplicit argument, since that only affects truncation cases.
 */
Datum varbit_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestSimplify)) {
    SupportRequestSimplify *req = (SupportRequestSimplify *)rawreq;
    FuncExpr *expr = req->fcall;
    Node *typmod;

    Assert(list_length(expr->args) >= 2);

    typmod = (Node *)lsecond(expr->args);

    if (IsA(typmod, Const) && !((Const *)typmod)->constisnull) {
      Node *source = (Node *)linitial(expr->args);
      int32 new_typmod = DatumGetInt32(((Const *)typmod)->constvalue);
      int32 old_max = exprTypmod(source);
      int32 new_max = new_typmod;

      /* Note: varbit() treats typmod 0 as invalid, so we do too */
      if (new_max <= 0 || (old_max > 0 && old_max <= new_max))
        ret = relabel_to_typmod(source, new_typmod);
    }
  }

  PG_RETURN_POINTER(ret);
}

/*
 * varbit()
 * Converts a varbit() type to a specific internal length.
 * len is the maximum bitlength specified in the column definition.
 *
 * If doing implicit cast, raise error when source data is too long.
 * If doing explicit cast, silently truncate to max length.
 */
Datum varbit(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  int32 len = PG_GETARG_INT32(1);
  bool isExplicit = PG_GETARG_BOOL(2);
  VarBit *result;

  /* no change if the length is not wanted, or the string already fits */
  if (len <= 0 || len >= VARBITLEN(arg))
    PG_RETURN_VARBIT_P(arg);

  if (!isExplicit)
    ereport(ERROR,
            (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
             errmsg("bit string too long for type bit varying(%d)", len)));

  result = varbit_make(len);
  ty_bits_resize(VARBITS(arg), VARBITLEN(arg), len, VARBITS(result));

  PG_RETURN_VARBIT_P(result);
}

Datum varbittypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);

  PG_RETURN_INT32(anybit_typmodin(ta, "varbit"));
}

Datum varbittypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);

  PG_RETURN_CSTRING(anybit_typmodout(typmod));
}

/*
 * Comparison operators
 *
 * We only need one set of comparison operators for bitstrings, as the lengths
 * are stored in the same way for zero-padded and varying bit strings.
 *
 * Note that the standard is not unambiguous about the comparison between
 * zero-padded bit strings and varying bitstrings. If the same value is written
 * into a zero padded bitstring as into a varying bitstring, but the zero
 * padded bitstring has greater length, it will be bigger.
 *
 * Zeros from the beginning of a bitstring cannot simply be ignored, as they
 * may be part of a bit string and may be significant.
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 */

/*
 * bit_cmp
 *
 * Compares two bitstrings and returns <0, 0, >0 depending on whether the first
 * string is smaller, equal, or bigger than the second. All bits are considered
 * and additional zero bits may make one string smaller/larger than the other,
 * even if their zero-padded values would be the same.
 */
static inline int32 bit_cmp(VarBit *arg1, VarBit *arg2) {
  /* the sizes are in the headers: no need to work them out from the bits */
  return ty_bits_cmp_bytes(VARBITS(arg1), VARBITBYTES(arg1),
                           &VARBITLEN(arg1), VARBITS(arg2),
                           VARBITBYTES(arg2), &VARBITLEN(arg2));
}

Datum biteq(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;
  int bitlen1, bitlen2;

  bitlen1 = VARBITLEN(arg1);
  bitlen2 = VARBITLEN(arg2);

  /* fast path for different-length inputs */
  if (bitlen1 != bitlen2)
    result = false;
  else
    result = (bit_cmp(arg1, arg2) == 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitne(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;
  int bitlen1, bitlen2;

  bitlen1 = VARBITLEN(arg1);
  bitlen2 = VARBITLEN(arg2);

  /* fast path for different-length inputs */
  if (bitlen1 != bitlen2)
    result = true;
  else
    result = (bit_cmp(arg1, arg2) != 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitlt(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;

  result = (bit_cmp(arg1, arg2) < 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitle(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;

  result = (bit_cmp(arg1, arg2) <= 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitgt(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;

  result = (bit_cmp(arg1, arg2) > 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitge(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  bool result;

  result = (bit_cmp(arg1, arg2) >= 0);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_BOOL(result);
}

Datum bitcmp(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  int32 result;

  result = bit_cmp(arg1, arg2);

  PG_FREE_IF_COPY(arg1, 0);
  PG_FREE_IF_COPY(arg2, 1);

  PG_RETURN_INT32(result);
}

/*
 * bitcat
 * Concatenation of bit strings
 */
Datum bitcat(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);

  PG_RETURN_VARBIT_P(bit_catenate(arg1, arg2));
}

static VarBit *bit_catenate(VarBit *arg1, VarBit *arg2) {
  VarBit *result;
  int32 nout;
  int64 maxbits = (int64)VARBITLEN(arg1) + VARBITLEN(arg2);

  /* a sum past the maximum is ty's error: make room for nothing then */
  result = varbit_make(maxbits > VARBITMAXLEN ? 0 : (int32)maxbits);
  if (ty_bits_cat(VARBITS(arg1), VARBITLEN(arg1), VARBITS(arg2),
                  VARBITLEN(arg2), VARBITS(result), &nout))
    varbit_ty_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                    ERRCODE_PROGRAM_LIMIT_EXCEEDED);

  return varbit_shrink(result, nout);
}

/*
 * bitsubstr
 * retrieve a substring from the bit string.
 * Note, s is 1-based.
 * SQL draft 6.10 9)
 */
Datum bitsubstr(PG_FUNCTION_ARGS) {
  PG_RETURN_VARBIT_P(bitsubstring(PG_GETARG_VARBIT_P(0), PG_GETARG_INT32(1),
                                  PG_GETARG_INT32(2), false));
}

Datum bitsubstr_no_len(PG_FUNCTION_ARGS) {
  PG_RETURN_VARBIT_P(
      bitsubstring(PG_GETARG_VARBIT_P(0), PG_GETARG_INT32(1), -1, true));
}

static VarBit *bitsubstring(VarBit *arg, int32 s, int32 l,
                            bool length_not_specified) {
  VarBit *result = varbit_make(VARBITLEN(arg));
  int32 nout;

  if (ty_bits_substr(VARBITS(arg), VARBITLEN(arg), s, l, !length_not_specified,
                     VARBITS(result), &nout))
    varbit_ty_error(ERRCODE_SUBSTRING_ERROR, ERRCODE_SUBSTRING_ERROR);

  return varbit_shrink(result, nout);
}

/*
 * bitoverlay
 *	Replace specified substring of first string with second
 *
 * The SQL standard defines OVERLAY() in terms of substring and concatenation.
 * This code is a direct implementation of what the standard says.
 */
Datum bitoverlay(PG_FUNCTION_ARGS) {
  VarBit *t1 = PG_GETARG_VARBIT_P(0);
  VarBit *t2 = PG_GETARG_VARBIT_P(1);
  int sp = PG_GETARG_INT32(2); /* substring start position */
  int sl = PG_GETARG_INT32(3); /* substring length */

  PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

Datum bitoverlay_no_len(PG_FUNCTION_ARGS) {
  VarBit *t1 = PG_GETARG_VARBIT_P(0);
  VarBit *t2 = PG_GETARG_VARBIT_P(1);
  int sp = PG_GETARG_INT32(2); /* substring start position */
  int sl;

  sl = VARBITLEN(t2); /* defaults to length(t2) */
  PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

static VarBit *bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl) {
  VarBit *result;
  int32 nout;
  int64 maxbits = (int64)VARBITLEN(t1) + VARBITLEN(t2);

  result = varbit_make(maxbits > VARBITMAXLEN ? 0 : (int32)maxbits);
  if (ty_bits_overlay(VARBITS(t1), VARBITLEN(t1), VARBITS(t2), VARBITLEN(t2),
                      sp, sl, VARBITS(result), &nout)) {
    if (ty_errnum() == TY_ERR_INVALID)
      ereport(ERROR, (errcode(ERRCODE_SUBSTRING_ERROR),
                      errmsg("negative substring length not allowed")));
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));
  }

  return varbit_shrink(result, nout);
}

/*
 * bit_count
 *
 * Returns the number of bits set in a bit string.
 */
Datum bit_bit_count(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);

  PG_RETURN_INT64(ty_bits_count(VARBITS(arg), VARBITLEN(arg)));
}

/*
 * bitlength, bitoctetlength
 * Return the length of a bit string
 */
Datum bitlength(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);

  PG_RETURN_INT32(VARBITLEN(arg));
}

Datum bitoctetlength(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);

  PG_RETURN_INT32(VARBITBYTES(arg));
}

/*
 * bit_and
 * perform a logical AND on two bit strings.
 */
Datum bit_and(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  VarBit *result = varbit_make(VARBITLEN(arg1));

  if (ty_bits_and(VARBITS(arg1), VARBITLEN(arg1), VARBITS(arg2),
                   VARBITLEN(arg2), VARBITS(result)))
    varbit_ty_error(ERRCODE_STRING_DATA_LENGTH_MISMATCH,
                    ERRCODE_STRING_DATA_LENGTH_MISMATCH);

  PG_RETURN_VARBIT_P(result);
}

/*
 * bit_or
 * perform a logical OR on two bit strings.
 */
Datum bit_or(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  VarBit *result = varbit_make(VARBITLEN(arg1));

  if (ty_bits_or(VARBITS(arg1), VARBITLEN(arg1), VARBITS(arg2),
                   VARBITLEN(arg2), VARBITS(result)))
    varbit_ty_error(ERRCODE_STRING_DATA_LENGTH_MISMATCH,
                    ERRCODE_STRING_DATA_LENGTH_MISMATCH);

  PG_RETURN_VARBIT_P(result);
}

/*
 * bitxor
 * perform a logical XOR on two bit strings.
 */
Datum bitxor(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *arg2 = PG_GETARG_VARBIT_P(1);
  VarBit *result = varbit_make(VARBITLEN(arg1));

  if (ty_bits_xor(VARBITS(arg1), VARBITLEN(arg1), VARBITS(arg2),
                   VARBITLEN(arg2), VARBITS(result)))
    varbit_ty_error(ERRCODE_STRING_DATA_LENGTH_MISMATCH,
                    ERRCODE_STRING_DATA_LENGTH_MISMATCH);

  PG_RETURN_VARBIT_P(result);
}

/*
 * bitnot
 * perform a logical NOT on a bit string.
 */
Datum bitnot(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  VarBit *result = varbit_make(VARBITLEN(arg));

  ty_bits_not(VARBITS(arg), VARBITLEN(arg), VARBITS(result));

  PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftleft
 * do a left shift (i.e. towards the beginning of the string)
 */
Datum bitshiftleft(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  VarBit *result = varbit_make(VARBITLEN(arg));

  ty_bits_shl(VARBITS(arg), VARBITLEN(arg), PG_GETARG_INT32(1),
               VARBITS(result));

  PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftright
 * do a right shift (i.e. towards the end of the string)
 */
Datum bitshiftright(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  VarBit *result = varbit_make(VARBITLEN(arg));

  ty_bits_shr(VARBITS(arg), VARBITLEN(arg), PG_GETARG_INT32(1),
               VARBITS(result));

  PG_RETURN_VARBIT_P(result);
}

/*
 * This is not defined in any standard. We retain the natural ordering of
 * bits here, as it just seems more intuitive.
 */
Datum bitfromint4(PG_FUNCTION_ARGS) {
  int32 a = PG_GETARG_INT32(0);
  int32 typmod = PG_GETARG_INT32(1);
  VarBit *result;
  int32 nbits;

  /* the default length is 1 */
  if (typmod <= 0 || typmod > VARBITMAXLEN)
    typmod = 1;
  result = varbit_make(typmod);
  ty_bits_from_int32(a, typmod, VARBITS(result), &nbits);

  PG_RETURN_VARBIT_P(result);
}

Datum bittoint4(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  int32 result;

  if (ty_bits_to_int32(VARBITS(arg), VARBITLEN(arg), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));

  PG_RETURN_INT32(result);
}

Datum bitfromint8(PG_FUNCTION_ARGS) {
  int64 a = PG_GETARG_INT64(0);
  int32 typmod = PG_GETARG_INT32(1);
  VarBit *result;
  int32 nbits;

  /* the default length is 1 */
  if (typmod <= 0 || typmod > VARBITMAXLEN)
    typmod = 1;
  result = varbit_make(typmod);
  ty_bits_from_int64(a, typmod, VARBITS(result), &nbits);

  PG_RETURN_VARBIT_P(result);
}

Datum bittoint8(PG_FUNCTION_ARGS) {
  VarBit *arg = PG_GETARG_VARBIT_P(0);
  int64 result;

  if (ty_bits_to_int64(VARBITS(arg), VARBITLEN(arg), &result))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("bigint out of range")));

  PG_RETURN_INT64(result);
}

/*
 * Determines the position of S2 in the bitstring S1 (1-based string).
 * If S2 does not appear in S1 this function returns 0.
 * If S2 is of length 0 this function returns 1.
 * Compatible in usage with POSITION() functions for other data types.
 */
Datum bitposition(PG_FUNCTION_ARGS) {
  VarBit *str = PG_GETARG_VARBIT_P(0);
  VarBit *substr = PG_GETARG_VARBIT_P(1);

  PG_RETURN_INT32(ty_bits_position(VARBITS(str), VARBITLEN(str),
                                   VARBITS(substr), VARBITLEN(substr)));
}

/*
 * bitsetbit
 *
 * Given an instance of type 'bit' creates a new one with
 * the Nth bit set to the given value.
 *
 * The bit location is specified left-to-right in a zero-based fashion
 * consistent with the other get_bit and set_bit functions, but
 * inconsistent with the standard substring, position, overlay functions
 */
Datum bitsetbit(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  VarBit *result = varbit_make(VARBITLEN(arg1));

  if (ty_bits_set(VARBITS(arg1), VARBITLEN(arg1), PG_GETARG_INT32(1),
                  PG_GETARG_INT32(2), VARBITS(result)))
    varbit_ty_error(ERRCODE_INVALID_PARAMETER_VALUE,
                    ERRCODE_ARRAY_SUBSCRIPT_ERROR);

  PG_RETURN_VARBIT_P(result);
}

/*
 * bitgetbit
 *
 * returns the value of the Nth bit of a bit array (0 or 1).
 *
 * The bit location is specified left-to-right in a zero-based fashion
 * consistent with the other get_bit and set_bit functions, but
 * inconsistent with the standard substring, position, overlay functions
 */
Datum bitgetbit(PG_FUNCTION_ARGS) {
  VarBit *arg1 = PG_GETARG_VARBIT_P(0);
  int32 bit;

  if (ty_bits_get(VARBITS(arg1), VARBITLEN(arg1), PG_GETARG_INT32(1), &bit))
    varbit_ty_error(ERRCODE_ARRAY_SUBSCRIPT_ERROR,
                    ERRCODE_ARRAY_SUBSCRIPT_ERROR);

  PG_RETURN_INT32(bit);
}
