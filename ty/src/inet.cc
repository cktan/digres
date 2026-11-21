/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * inet.cc -- the operations of inet and cidr, from the cases of PostgreSQL's
 * network.c; the text is the ISC code in inet_pton.cc and the files like it.
 */
#include "ty_inet.h"

#include <errno.h>
#include <string.h>

#include "error.h"
#include "inet_text.h"
#include "ty_hash.h"

namespace {

/* Whether the cidr address has no bit set to the right of its mask. */
bool address_ok(const uint8_t *a, int bits, int maxbits, int maxbytes) {
  if (bits == maxbits) {
    return true;
  }
  int byte = bits / 8;
  uint8_t mask = 0xff;

  if (bits != 0) {
    mask = (uint8_t)(mask >> (bits % 8));
  }
  for (; byte < maxbytes; byte++) {
    if ((a[byte] & mask) != 0) {
      return false;
    }
    mask = 0xff;
  }
  return true;
}

/* A value with the family and mask of a, and an address of zeros. */
void blank(const ty_inet_t *a, ty_inet_t *out) {
  memset(out, 0, sizeof *out);
  out->family = a->family;
  out->bits = a->bits;
}

} // namespace

/*
 * Parse the text of an inet or a cidr.
 * 1. A colon anywhere means IPv6; if none, IPv4.
 * 2. The ISC parser gives the bits, or -1.
 * 3. A cidr must have no bit set right of its mask.
 */
int ty_inet_parse(const char *s, int is_cidr, ty_inet_t *out) {
  memset(out, 0, sizeof *out);
  out->family = strchr(s, ':') != nullptr ? TY_INET_V6 : TY_INET_V4;

  int bits =
      ty_inet_net_pton(out->family, s, out->addr,
                       is_cidr ? (size_t)ty_inet_addrsize(out) : (size_t)-1);

  if (bits < 0 || bits > ty_inet_maxbits(out)) {
    return ty_internal::set_error(TY_ERR_INVALID, "invalid inet text");
  }
  if (is_cidr && !address_ok(out->addr, bits, ty_inet_maxbits(out),
                             ty_inet_addrsize(out))) {
    return ty_internal::set_error(TY_ERR_RANGE,
                                  "Value has bits set to right of mask.");
  }
  out->bits = (uint8_t)bits;
  return 0;
}

/* Whether a cidr value has no bit set to the right of its mask. */
int ty_inet_address_ok(const ty_inet_t *a) {
  return address_ok(a->addr, a->bits, ty_inet_maxbits(a), ty_inet_addrsize(a));
}

/* Format in one of the modes. */
int ty_inet_format(const ty_inet_t *a, int mode, char *out) {
  const size_t size = TY_INET_TEXT_MAX;
  char *r;

  if (mode == TY_INET_CIDR_ABBREV) {
    r = ty_inet_cidr_ntop(a->family, a->addr, a->bits, out, size);
  } else {
    int bits = (mode == TY_INET_HOST || mode == TY_INET_SHOW)
                   ? ty_inet_maxbits(a)
                   : a->bits;

    r = ty_inet_net_ntop(a->family, a->addr, bits, out, size);
  }
  if (r == nullptr) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "could not format inet value");
  }
  char *slash = strchr(out, '/');

  if (mode == TY_INET_HOST && slash != nullptr) {
    *slash = '\0';
  } else if ((mode == TY_INET_CIDR || mode == TY_INET_SHOW) &&
             slash == nullptr) {
    /* the /n is at most 4 characters, and the buffer has room */
    put_slash_u(out + strlen(out), a->bits);
  }
  return 0;
}

/* The first n bits of l and r compared. */
int ty_inet_bitncmp(const uint8_t *l, const uint8_t *r, int n) {
  int b = n / 8;
  int x = memcmp(l, r, (size_t)b);

  if (x || (n % 8) == 0) {
    return x;
  }
  unsigned lb = l[b], rb = r[b];

  for (b = n % 8; b > 0; b--) {
    if ((lb & 0x80) != (rb & 0x80)) {
      return (lb & 0x80) ? 1 : -1;
    }
    lb <<= 1;
    rb <<= 1;
  }
  return 0;
}

/* How many leading bits of the first n are the same. */
int ty_inet_bitncommon(const uint8_t *l, const uint8_t *r, int n) {
  int byte, nbits = n % 8;

  for (byte = 0; byte < n / 8; byte++) {
    if (l[byte] != r[byte]) {
      /* at least one bit in the last byte is not common */
      nbits = 7;
      break;
    }
  }
  if (nbits != 0) {
    unsigned diff = l[byte] ^ r[byte];

    while ((diff >> (8 - nbits)) != 0) {
      nbits--;
    }
  }
  return 8 * byte + nbits;
}

/* Compare: common network bits, then the mask length, then the address. */
int ty_inet_cmp(const ty_inet_t *a, const ty_inet_t *b) {
  if (a->family == b->family) {
    int order = ty_inet_bitncmp(a->addr, b->addr,
                                a->bits < b->bits ? a->bits : b->bits);

    if (order != 0) {
      return order;
    }
    order = (int)a->bits - (int)b->bits;
    if (order != 0) {
      return order;
    }
    return ty_inet_bitncmp(a->addr, b->addr, ty_inet_maxbits(a));
  }
  return (int)a->family - (int)b->family;
}

/* a is inside b, and not the same size. */
int ty_inet_sub(const ty_inet_t *a, const ty_inet_t *b) {
  return a->family == b->family && a->bits > b->bits &&
         ty_inet_bitncmp(a->addr, b->addr, b->bits) == 0;
}

/* a is inside b, or the same. */
int ty_inet_subeq(const ty_inet_t *a, const ty_inet_t *b) {
  return a->family == b->family && a->bits >= b->bits &&
         ty_inet_bitncmp(a->addr, b->addr, b->bits) == 0;
}

/* a contains b, and is not the same size. */
int ty_inet_sup(const ty_inet_t *a, const ty_inet_t *b) {
  return a->family == b->family && a->bits < b->bits &&
         ty_inet_bitncmp(a->addr, b->addr, a->bits) == 0;
}

/* a contains b, or is the same. */
int ty_inet_supeq(const ty_inet_t *a, const ty_inet_t *b) {
  return a->family == b->family && a->bits <= b->bits &&
         ty_inet_bitncmp(a->addr, b->addr, a->bits) == 0;
}

/* One contains the other. */
int ty_inet_overlap(const ty_inet_t *a, const ty_inet_t *b) {
  return a->family == b->family &&
         ty_inet_bitncmp(a->addr, b->addr,
                         a->bits < b->bits ? a->bits : b->bits) == 0;
}

/* Copy a with its mask set to bits, and the bits right of it cleared. */
void ty_inet_set_masklen_cidr(const ty_inet_t *a, int bits, ty_inet_t *out) {
  memset(out, 0, sizeof *out);
  out->family = a->family;
  out->bits = (uint8_t)bits;
  if (bits > 0) {
    /* keep the bytes that the mask covers, then clear the pad of the last */
    memcpy(out->addr, a->addr, (size_t)(bits + 7) / 8);
    if (bits % 8) {
      out->addr[bits / 8] &= (uint8_t)~(0xFF >> (bits % 8));
    }
  }
}

/* The address with every host bit set. */
void ty_inet_broadcast(const ty_inet_t *a, ty_inet_t *out) {
  int bits = a->bits;

  blank(a, out);
  for (int byte = 0; byte < ty_inet_addrsize(a); byte++) {
    uint8_t mask;

    if (bits >= 8) {
      mask = 0x00;
      bits -= 8;
    } else if (bits == 0) {
      mask = 0xff;
    } else {
      mask = (uint8_t)(0xff >> bits);
      bits = 0;
    }
    out->addr[byte] = a->addr[byte] | mask;
  }
}

/* The address with every host bit cleared. */
void ty_inet_network(const ty_inet_t *a, ty_inet_t *out) {
  int bits = a->bits, byte = 0;

  blank(a, out);
  while (bits) {
    uint8_t mask;

    if (bits >= 8) {
      mask = 0xff;
      bits -= 8;
    } else {
      mask = (uint8_t)(0xff << (8 - bits));
      bits = 0;
    }
    out->addr[byte] = a->addr[byte] & mask;
    byte++;
  }
}

/* The mask as an address, with the full mask length. */
void ty_inet_netmask(const ty_inet_t *a, ty_inet_t *out) {
  int bits = a->bits, byte = 0;

  blank(a, out);
  while (bits) {
    uint8_t mask;

    if (bits >= 8) {
      mask = 0xff;
      bits -= 8;
    } else {
      mask = (uint8_t)(0xff << (8 - bits));
      bits = 0;
    }
    out->addr[byte++] = mask;
  }
  out->bits = (uint8_t)ty_inet_maxbits(a);
}

/* The mask of the host bits as an address, with the full mask length. */
void ty_inet_hostmask(const ty_inet_t *a, ty_inet_t *out) {
  int bits = ty_inet_maxbits(a) - a->bits, byte = ty_inet_addrsize(a) - 1;

  blank(a, out);
  while (bits) {
    uint8_t mask;

    if (bits >= 8) {
      mask = 0xff;
      bits -= 8;
    } else {
      mask = (uint8_t)(0xff >> (8 - bits));
      bits = 0;
    }
    out->addr[byte--] = mask;
  }
  out->bits = (uint8_t)ty_inet_maxbits(a);
}

/* The smallest cidr that holds both. */
int ty_inet_merge(const ty_inet_t *a, const ty_inet_t *b, ty_inet_t *out) {
  if (a->family != b->family) {
    return ty_internal::set_error(
        TY_ERR_INVALID, "cannot merge addresses from different families");
  }
  int common = ty_inet_bitncommon(a->addr, b->addr,
                                  a->bits < b->bits ? a->bits : b->bits);

  ty_inet_set_masklen_cidr(a, common, out);
  return 0;
}

/* NOT of each address byte. */
void ty_inet_not(const ty_inet_t *a, ty_inet_t *out) {
  blank(a, out);
  for (int i = 0; i < ty_inet_addrsize(a); i++) {
    out->addr[i] = (uint8_t)~a->addr[i];
  }
}

/* AND of the addresses; the longer mask. */
int ty_inet_and(const ty_inet_t *a, const ty_inet_t *b, ty_inet_t *out) {
  if (a->family != b->family) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "cannot AND inet values of different sizes");
  }
  blank(a, out);
  for (int i = 0; i < ty_inet_addrsize(a); i++) {
    out->addr[i] = a->addr[i] & b->addr[i];
  }
  out->bits = a->bits > b->bits ? a->bits : b->bits;
  return 0;
}

/* OR of the addresses; the longer mask. */
int ty_inet_or(const ty_inet_t *a, const ty_inet_t *b, ty_inet_t *out) {
  if (a->family != b->family) {
    return ty_internal::set_error(TY_ERR_INVALID,
                                  "cannot OR inet values of different sizes");
  }
  blank(a, out);
  for (int i = 0; i < ty_inet_addrsize(a); i++) {
    out->addr[i] = a->addr[i] | b->addr[i];
  }
  out->bits = a->bits > b->bits ? a->bits : b->bits;
  return 0;
}

/*
 * a + n, byte by byte from the low end, with a carry.
 * 1. Add the low byte of n to each address byte; n then loses that byte,
 *    with the sign kept.
 * 2. At the end n must be 0 and the carry 0, or n -1 and the carry 1 (n was
 *    negative): else the result is out of range.
 */
int ty_inet_add(const ty_inet_t *a, int64_t n, ty_inet_t *out) {
  int carry = 0;

  if (a->family == TY_INET_V4) {
    /* the common case: the 32-bit value in an int64 cannot overflow */
    int64_t v = ((int64_t)a->addr[0] << 24 | a->addr[1] << 16 |
                 a->addr[2] << 8 | a->addr[3]);
    int64_t r;

    if (__builtin_add_overflow(v, n, &r) || r < 0 || r > 0xFFFFFFFFLL) {
      return ty_err_range();
    }
    blank(a, out);
    out->addr[0] = (uint8_t)(r >> 24);
    out->addr[1] = (uint8_t)(r >> 16);
    out->addr[2] = (uint8_t)(r >> 8);
    out->addr[3] = (uint8_t)r;
    return 0;
  }

  blank(a, out);
  for (int nb = ty_inet_addrsize(a) - 1; nb >= 0; nb--) {
    carry = a->addr[nb] + (int)(n & 0xFF) + carry;
    out->addr[nb] = (uint8_t)(carry & 0xFF);
    carry >>= 8;
    n &= ~((int64_t)0xFF);
    n /= 0x100;
  }
  if (!((n == 0 && carry == 0) || (n == -1 && carry == 1))) {
    return ty_err_range();
  }
  return 0;
}

/*
 * a - b: add the complement of b, and 1 as the first carry (two's
 * complement); the low 8 bytes make the result, and the bytes above must be
 * the sign.
 */
int ty_inet_sub_inet(const ty_inet_t *a, const ty_inet_t *b, int64_t *out) {
  if (a->family != b->family) {
    return ty_internal::set_error(
        TY_ERR_INVALID, "cannot subtract inet values of different sizes");
  }
  int64_t res = 0;
  int byte = 0, carry = 1;

  for (int nb = ty_inet_addrsize(a) - 1; nb >= 0; nb--) {
    carry = a->addr[nb] + (~b->addr[nb] & 0xFF) + carry;
    int lobyte = carry & 0xFF;

    if (byte < (int)sizeof(int64_t)) {
      res = (int64_t)((uint64_t)res | ((uint64_t)lobyte << (byte * 8)));
    } else if ((res < 0) ? (lobyte != 0xFF) : (lobyte != 0)) {
      return ty_err_range();
    }
    carry >>= 8;
    byte++;
  }
  if (carry == 0 && byte < (int)sizeof(int64_t)) {
    res = (int64_t)((uint64_t)res | (UINT64_MAX << (byte * 8)));
  }
  *out = res;
  return 0;
}

/* The hash of family, bits and the address, as PostgreSQL's hashinet. */
uint32_t ty_inet_hash(const ty_inet_t *a) {
  return ty_hash_bytes_std(reinterpret_cast<const uint8_t *>(a),
                           (size_t)ty_inet_addrsize(a) + 2);
}

/* The same with a seed. */
uint64_t ty_inet_hash_extended(const ty_inet_t *a, uint64_t seed) {
  return ty_hash_bytes(reinterpret_cast<const uint8_t *>(a),
                       (size_t)ty_inet_addrsize(a) + 2, seed);
}
