/*
 * ty_rename.h -- gives ty's copy of the Ryu printers names of their own.
 *
 * A host that links ty may have functions of the same names (digres has
 * double_to_shortest_decimal_buf and the others, which call ty), and a clash
 * would make ty call the host's. Included first by postgres_fe.h, and by
 * ty's float.cc before shortest_dec.h.
 */
#ifndef TY_RYU_RENAME_H
#define TY_RYU_RENAME_H

#define double_to_shortest_decimal_bufn ty_ryu_double_bufn
#define double_to_shortest_decimal_buf ty_ryu_double_buf
#define double_to_shortest_decimal ty_ryu_double_alloc
#define float_to_shortest_decimal_bufn ty_ryu_float_bufn
#define float_to_shortest_decimal_buf ty_ryu_float_buf
#define float_to_shortest_decimal ty_ryu_float_alloc

#endif /* TY_RYU_RENAME_H */
