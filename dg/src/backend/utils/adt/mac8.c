/*-------------------------------------------------------------------------
 *
 * mac8.c
 *	  PostgreSQL type definitions for 8 byte (EUI-64) MAC addresses.
 *
 * EUI-48 (6 byte) MAC addresses are accepted as input and are stored in
 * EUI-64 format, with the 4th and 5th bytes set to FF and FE, respectively.
 *
 * Output is always in 8 byte (EUI-64) format.
 *
 * The following code is written with the assumption that the OUI field
 * size is 24 bits.
 *
 * Portions Copyright (c) 1998-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/digres/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *		  src/backend/utils/adt/mac8.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "nodes/nodes.h"
#include "utils/fmgrprotos.h"
#include "utils/inet.h"
#include "ty_mac.h"

/*
 *	Utility macros used for sorting and comparing:
 */
#define hibits(addr) \
  ((unsigned long)(((addr)->a<<24) | ((addr)->b<<16) | ((addr)->c<<8) | ((addr)->d)))

#define lobits(addr) \
  ((unsigned long)(((addr)->e<<24) | ((addr)->f<<16) | ((addr)->g<<8) | ((addr)->h)))

static unsigned char hex2_to_uchar(const unsigned char *ptr, bool *badhex);

static const signed char hexlookup[128] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,
    9,  -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

/*
 * hex2_to_uchar - convert 2 hex digits to a byte (unsigned char)
 *
 * Sets *badhex to true if the end of the string is reached ('\0' found), or if
 * either character is not a valid hex digit.
 */
static inline unsigned char hex2_to_uchar(const unsigned char *ptr,
                                          bool *badhex) {
  unsigned char ret;
  signed char lookup;

  /* Handle the first character */
  if (*ptr > 127)
    goto invalid_input;

  lookup = hexlookup[*ptr];
  if (lookup < 0)
    goto invalid_input;

  ret = lookup << 4;

  /* Move to the second character */
  ptr++;

  if (*ptr > 127)
    goto invalid_input;

  lookup = hexlookup[*ptr];
  if (lookup < 0)
    goto invalid_input;

  ret += lookup;

  return ret;

invalid_input:
  *badhex = true;
  return 0;
}

/*
 * MAC address (EUI-48 and EUI-64) reader. Accepts several common notations.
 */
Datum macaddr8_in(PG_FUNCTION_ARGS) {
  const char *str = PG_GETARG_CSTRING(0);
  Node *escontext = fcinfo->context;
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  /* ty reads six or eight pairs of hex digits, and widens six */
  if (ty_macaddr8_parse(str, (ty_macaddr8_t *)result) != 0)
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "macaddr8", str)));

  PG_RETURN_MACADDR8_P(result);
}

/*
 * MAC8 address (EUI-64) output function. Fixed format.
 */
Datum macaddr8_out(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  char *result = (char *)palloc(TY_MACADDR8_TEXT_SIZE);

  ty_macaddr8_format((const ty_macaddr8_t *)addr, result);

  PG_RETURN_CSTRING(result);
}

/*
 * macaddr8_recv - converts external binary format(EUI-48 and EUI-64) to
 * macaddr8
 *
 * The external representation is just the eight bytes, MSB first.
 */
Datum macaddr8_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  macaddr8 *addr;

  addr = (macaddr8 *)palloc0(sizeof(macaddr8));

  addr->a = pq_getmsgbyte(buf);
  addr->b = pq_getmsgbyte(buf);
  addr->c = pq_getmsgbyte(buf);

  if (buf->len == 6) {
    addr->d = 0xFF;
    addr->e = 0xFE;
  } else {
    addr->d = pq_getmsgbyte(buf);
    addr->e = pq_getmsgbyte(buf);
  }

  addr->f = pq_getmsgbyte(buf);
  addr->g = pq_getmsgbyte(buf);
  addr->h = pq_getmsgbyte(buf);

  PG_RETURN_MACADDR8_P(addr);
}

/*
 * macaddr8_send - converts macaddr8(EUI-64) to binary format
 */
Datum macaddr8_send(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  StringInfoData buf;

  pq_begintypsend(&buf);
  pq_sendbyte(&buf, addr->a);
  pq_sendbyte(&buf, addr->b);
  pq_sendbyte(&buf, addr->c);
  pq_sendbyte(&buf, addr->d);
  pq_sendbyte(&buf, addr->e);
  pq_sendbyte(&buf, addr->f);
  pq_sendbyte(&buf, addr->g);
  pq_sendbyte(&buf, addr->h);

  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * macaddr8_cmp_internal - comparison function for sorting:
 */
static int32 macaddr8_cmp_internal(macaddr8 *a1, macaddr8 *a2) {
  return ty_macaddr8_cmp((const ty_macaddr8_t *)a1, (const ty_macaddr8_t *)a2);
}

Datum macaddr8_cmp(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_INT32(macaddr8_cmp_internal(a1, a2));
}

/*
 * Boolean comparison functions.
 */

Datum macaddr8_lt(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) < 0);
}

Datum macaddr8_le(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) <= 0);
}

Datum macaddr8_eq(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) == 0);
}

Datum macaddr8_ge(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) >= 0);
}

Datum macaddr8_gt(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) > 0);
}

Datum macaddr8_ne(PG_FUNCTION_ARGS) {
  macaddr8 *a1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *a2 = PG_GETARG_MACADDR8_P(1);

  PG_RETURN_BOOL(macaddr8_cmp_internal(a1, a2) != 0);
}

/*
 * Support function for hash indexes on macaddr8.
 */
Datum hashmacaddr8(PG_FUNCTION_ARGS) {
  macaddr8 *key = PG_GETARG_MACADDR8_P(0);

  PG_RETURN_UINT32((uint32)ty_macaddr8_hash((const ty_macaddr8_t *)key, 0));
}

Datum hashmacaddr8extended(PG_FUNCTION_ARGS) {
  macaddr8 *key = PG_GETARG_MACADDR8_P(0);

  PG_RETURN_UINT64(ty_macaddr8_hash((const ty_macaddr8_t *)key,
                                    (uint64)PG_GETARG_INT64(1)));
}

/*
 * Arithmetic functions: bitwise NOT, AND, OR.
 */
Datum macaddr8_not(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr8_not((const ty_macaddr8_t *)addr);
  PG_RETURN_MACADDR8_P(result);
}

Datum macaddr8_and(PG_FUNCTION_ARGS) {
  macaddr8 *addr1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *addr2 = PG_GETARG_MACADDR8_P(1);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr8_and((const ty_macaddr8_t *)addr1,
                                              (const ty_macaddr8_t *)addr2);
  PG_RETURN_MACADDR8_P(result);
}

Datum macaddr8_or(PG_FUNCTION_ARGS) {
  macaddr8 *addr1 = PG_GETARG_MACADDR8_P(0);
  macaddr8 *addr2 = PG_GETARG_MACADDR8_P(1);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr8_or((const ty_macaddr8_t *)addr1,
                                              (const ty_macaddr8_t *)addr2);
  PG_RETURN_MACADDR8_P(result);
}

/*
 * Truncation function to allow comparing macaddr8 manufacturers.
 */
Datum macaddr8_trunc(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr8_trunc((const ty_macaddr8_t *)addr);
  PG_RETURN_MACADDR8_P(result);
}

/*
 * Set 7th bit for modified EUI-64 as used in IPv6.
 */
Datum macaddr8_set7bit(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr8_set7bit((const ty_macaddr8_t *)addr);
  PG_RETURN_MACADDR8_P(result);
}

/*----------------------------------------------------------
 *	Conversion operators.
 *---------------------------------------------------------*/

Datum macaddrtomacaddr8(PG_FUNCTION_ARGS) {
  macaddr *addr6 = PG_GETARG_MACADDR_P(0);
  macaddr8 *result = (macaddr8 *)palloc0(sizeof(macaddr8));

  *(ty_macaddr8_t *)result = ty_macaddr_to_macaddr8((const ty_macaddr_t *)addr6);
  PG_RETURN_MACADDR8_P(result);
}

Datum macaddr8tomacaddr(PG_FUNCTION_ARGS) {
  macaddr8 *addr = PG_GETARG_MACADDR8_P(0);
  macaddr *result = (macaddr *)palloc0(sizeof(macaddr));

  if (ty_macaddr8_to_macaddr((const ty_macaddr8_t *)addr,
                             (ty_macaddr_t *)result) != 0)
    ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("macaddr8 data out of range to convert to macaddr"),
             errhint("Only addresses that have FF and FE as values in the "
                     "4th and 5th bytes from the left, for example "
                     "xx:xx:xx:ff:fe:xx:xx:xx, are eligible to be converted "
                     "from macaddr8 to macaddr.")));

  PG_RETURN_MACADDR_P(result);
}
