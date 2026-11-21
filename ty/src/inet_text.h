/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * inet_text.h -- the functions, derived from ISC's code in PostgreSQL, that
 * ty_inet.h's text parse and format call. Not part of ty's interface.
 */
#ifndef TY_INET_TEXT_H
#define TY_INET_TEXT_H

#include <stddef.h>

/* The address families of an inet value, as PostgreSQL stores them. */
#define TY_INET_AF_V4 2
#define TY_INET_AF_V6 3

/*
 * Text to bytes. Return the number of bits, or -1 with errno set. A size of
 * (size_t)-1 means an inet (any host bits); otherwise a cidr in size bytes.
 */
int ty_inet_net_pton(int af, const char *src, void *dst, size_t size);

/* Bytes to text, with the /bits unless it is the whole address (inet). */
char *ty_inet_net_ntop(int af, const void *src, int bits, char *dst,
                       size_t size);

/* Bytes to text of a cidr: the /bits always, and the zero tail dropped. */
char *ty_inet_cidr_ntop(int af, const void *src, int bits, char *dst,
                        size_t size);

/*
 * The numbers the ISC code writes, with the NUL after them, as sprintf
 * would; they return the characters written. glibc's sprintf is several
 * times slower than these on a number of three digits.
 */
static inline size_t put_u(char *dst, unsigned v) {
  char tmp[12];
  size_t n = 0, len;

  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v != 0);
  for (len = 0; len < n; len++) {
    dst[len] = tmp[n - 1 - len];
  }
  dst[len] = '\0';
  return len;
}

static inline size_t put_slash_u(char *dst, unsigned v) {
  dst[0] = '/';
  return 1 + put_u(dst + 1, v);
}

static inline size_t put_x(char *dst, unsigned v) {
  static const char hex[] = "0123456789abcdef";
  char tmp[9];
  size_t n = 0, len;

  do {
    tmp[n++] = hex[v & 15];
    v >>= 4;
  } while (v != 0);
  for (len = 0; len < n; len++) {
    dst[len] = tmp[n - 1 - len];
  }
  dst[len] = '\0';
  return len;
}

#endif
