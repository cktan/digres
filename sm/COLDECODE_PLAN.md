# COLDECODE_PLAN: a CG decodes into arrays

Status: built. §9 lists what the user decided, and §10 has the
measures. [`COLSTORE.md`](COLSTORE.md) §5.5 is the design of
record.

Before this plan `cg_open` (`sm2/src/col/cg.cc`) decoded a CG
into one `std::string` a row. This plan makes it decode into
arrays: the form that ty's vector operations read
([`../ty/DESIGN.md`](../ty/DESIGN.md#21-vectors) §21). It is the
first step of
[`../REPORT.md`](../REPORT.md#7-the-recommendation-as-steps) §7,
columnar decode in sm2. The row scan of today uses the same
arrays, so it gets faster with no change of its interface.

Out of scope: the batch scan in `sm.h`, the executor, and a new
layout of the values in a file. §8 says what each would need.
The one change of the file is that nulls are flag bytes (§9).

---

## 1. A worked example

Table `ev` has `kind text` and `amount int4`. An RG holds four
rows:

| row | `kind` | `amount` |
| --- | --- | --- |
| 0 | `click` | 3 |
| 1 | `view` | null |
| 2 | null | 7 |
| 3 | `view` | 9 |

The CG of `amount` has plain encoding. It decodes to:

```
values   int32_t[4]   { 3, 0, 7, 9 }
flags    uint8_t[4]   { 0, 2, 0, 0 }     2 is TY_VEC_NULL
```

Suppose the CG of `kind` has a dictionary. It decodes to:

```
values   const char*[2]   { -> click, -> view }   varlenas
codes    uint32_t[4]      { 0, 1, 0, 1 }
dict_n   2
flags    uint8_t[4]       { 0, 0, 2, 0 }
```

How to read it:

- **`values` holds each distinct value once.** `values[0]` is
  `click`, and `values[1]` is `view`.
- **`dict_n` is the count of `values`**, here 2. It is not the
  count of rows, which is `n`, here 4.
- **`codes` has one entry a row: an index into `values`.** The
  value of row i is `values[codes[i]]`.
- **`flags` has one entry a row**, as for `amount`. A dictionary
  changes `values` alone, and adds `codes`.

| row | `flags[i]` | `codes[i]` | `values[codes[i]]` | the row's value |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | `click` | `click` |
| 1 | 0 | 1 | `view` | `view` |
| 2 | 2 | 0 | `click` | null: the flag decides, and the code means nothing |
| 3 | 0 | 1 | `view` | `view` |

Then `amount > 5` is one call, on a copy of the flags and a
vector of fives:

```c
tyvec_int32_gt(4, flag, amount.values, fives, out);
```

`out` is `{ 0, -, 1, 1 }`: row 1 is null, so its `out` means
nothing. A test of `kind = 'view'` compares two dictionary
values, not four rows.

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| RG, CG | row group, column group ([`COLSTORE.md`](COLSTORE.md#2-terms) §2) |
| body | the bytes of a CG file before zstd: the encoded values |
| decode | body to arrays |
| flat | one value a row, in an array |
| code | a row's place in the CG's dictionary |
| fixed type | a type whose `len` is not -1: `int4`, `date`, `decimal64`, `uuid`, ... |
| flag byte | a byte a row, ty's; bit 1, `TY_VEC_NULL`, is 1 for a null |

---

## 3. What decode cost before

Instructions a value, for CGs of 10,000 rows made like TPC-H's
`lineitem`, at `-O3`. `cg_open` is three parts: the CRC32C, zstd,
and the decode. The last column is a prototype of §5's decode,
of the same files, but without §5.3's checks.

| CG | encoding | CRC32C | zstd | decode before | decode, prototype |
| --- | --- | --- | --- | --- | --- |
| `_id`, 1 to n | delta | 9 | 0 | 143 | 21 |
| `returnflag`, 3 texts | dictionary | 2 | 6 | 124 | 16 |
| `shipdate`, 2,526 dates | dictionary | 15 | 31 | 263 | 24 |
| `quantity`, 50 `decimal64` | dictionary | 6 | 1 | 167 | 16 |
| `extendedprice`, `decimal64` | plain | 37 | 159 | 96 | 12 |
| random `float8` | plain | 61 | 113 | 96 | 12 |
| random `int4`, 10% null | plain | 38 | 100 | 111 | 20 |
| `comment`, 10 to 43 bytes | plain | 58 | 338 | 456 | 10 |
| **sum** | | **225** | **748** | **1,456** | **130** |

- **The decode is 60% of `cg_open`**, and an array decode takes
  a tenth of it. `cg_open` as a whole gets 2.2 times faster:
  2,428 to 1,103.
- **Two causes.** One string a value, with a `malloc` when it is
  longer than 15 bytes. And `unpack`, which reads packed codes
  one bit at a time.
- **After it, zstd and CRC32C are the cost.** sm3 computes the
  CRC32C once more than the table shows, over the whole file, to
  compare it with the RG key's. It is made a byte at a time from
  a table, 7 instructions a byte. §8 has both.
- **Memory falls too.** A decoded `int8` CG of 10,000 rows is
  320 KB of `std::string` before, and 80 KB as an array.

---

## 4. The interface

```cpp
/* A CG's values as arrays, valid until cg_close. */
struct cg_vec_t {
    uint32_t        n;      /* rows */
    const void*     values; /* flat: one a row. dict: dict_n of them.
                               A fixed type: type->len bytes each,
                               aligned to 8. A varlena type: const
                               char*, each at a varlena. */
    const uint32_t* codes;  /* dict: a code a row, each below dict_n;
                               nullptr: flat */
    uint32_t        dict_n; /* dict: the values; 0: flat */
    const uint8_t*  flags;  /* a flag byte a row: 0 or TY_VEC_NULL;
                               nullptr: no row is null */
};

/*
 * Check and decode a CG file of a column of type. Returns 0 with the
 * CG in out, or -1 with SM_ERR_CORRUPT for a bad file.
 */
int cg_open(std::string_view file, const type_t* type, cg_t*& out);

/* A CG's arrays. */
void cg_vec(const cg_t* cg, cg_vec_t& out);
```

- **`cg_open` gains the type.** The file does not say whether
  its values are fixed or varlenas, and an array needs to know.
  sm3 has the type: `int8` for the `_id` CG, the column's for
  the others.
- **`cg_rows`, `cg_get` and `cg_close` stay as they are.**
  `cg_get` reads the arrays. In sm3, `rg_read_open` passes the
  type. `rg_read_next`, and the scan, `ANALYZE`, RG compaction
  and elimination that read by it, need no change.
- **The arrays do not change after `cg_open`.** An operation of
  ty writes over its flags, so its caller copies them first. The
  copy is also where the batch scan puts `TY_VEC_NOTVALID` for a
  deleted row.
- **A `cg_t` refers to nothing of sm1.** A later batch may keep
  it past the scan, and free it on any thread.

A null row's value means nothing, but it is there:

| Form | A null row holds |
| --- | --- |
| flat, fixed | zero bytes |
| flat, varlena | a pointer at an empty varlena |
| dictionary | code 0 |

So an operation that computes every row reads no bad address.

**Every encoding has one form:**

| Encoding | Form |
| --- | --- |
| plain | flat |
| delta | flat |
| dictionary | dictionary |

**Values are sm2's datums, unchanged.** A date is an `int32` of
days since 2000-01-01, and a `decimal64` is an `int64`, its value
shifted left by 5 with the scale in the low 5 bits. A column's
values have one scale, so `tyvec_int64_lt` compares two values of
a `decimal64` column, as it does timestamps. Its arithmetic needs
an operation of its own.

---

## 5. How it decodes

### 5.1 What a `cg_t` holds

| Part | For |
| --- | --- |
| the values, 8-byte aligned | a fixed type's bytes; a varlena type's pointers |
| the codes | a dictionary |
| the flag bytes | a CG with a null |
| the body | a varlena type: the pointers point into it; freed for a fixed type |

A varlena is not copied. The zstd output is the memory it lives
in.

### 5.2 The steps

1. Check the tail, the CRC32C and the sizes, and decompress, as
   today.
2. Copy the flag bytes, if there are any, and check them.
3. Decode the values:
   1. **plain, fixed:** read each length, which must be the
      type's, and copy the value to its slot. A null's length
      is 0.
   2. **plain, varlena:** check each length against the varlena's
      header, and store its pointer.
   3. **delta:** unpack the deltas, and sum them into the slots.
   4. **dictionary:** decode its values as plain's, with no null.
      Then unpack the codes.
4. Free the body of a fixed type.

**Unpack reads words, not bits.** A field of up to 56 bits is one
8-byte load, a shift and a mask. The body has 8 spare bytes at
its end, so the last load stays inside it. A wider field, which
only a delta can have, keeps today's loop.

**A varlena with a 4-byte header is aligned to 4 bytes**, as
PostgreSQL's macros need. In the body each varlena follows its
4-byte length. When the varlena is not aligned, step 3 moves it
left by 1 to 3 bytes, over the length it has just read. A varlena
with a 1-byte header needs no alignment.

### 5.3 What it checks

The CRC32C finds damage. These checks keep a bad writer from
making a reader run past the body. Each fails with
`SM_ERR_CORRUPT`.

| Check | Before |
| --- | --- |
| each length is inside the body | yes |
| a fixed value's length is the type's | no |
| a varlena's header gives its length, and is neither compressed nor a TOAST pointer | no |
| a delta CG's type is `int2`, `int4` or `int8`, and its width is the type's | no |
| the dictionary has no more values than the CG has rows | no |
| a count of values is at most a quarter of the body's bytes, before memory is taken for it | no |
| a flag byte is 0 or 2 | - |
| every code is below `dict_n`, a null row's too | not a null row's |

The codes are checked by a loop with no branch: OR the failures,
and test once.

---

## 6. The cases

The change is below the level where rows have versions: a CG file
is immutable, and the bytes that `cg_get` gives do not change.

| Case | What changes |
| --- | --- |
| TOAST | nothing: a CG has no TOAST pointer, and a value is 2,048 bytes at most |
| indexes: unique, primary key, expression, partial, `INCLUDE` | none: a column store table has no index |
| update, A to B to A, delete and insert again | nothing: delete markers and the delta store decide |
| abort, savepoint rollback, crash, 2PC | nothing: no write path changes |
| two writers, an old snapshot, READ COMMITTED's re-read, an open cursor | nothing: sm3 chooses the rows |
| SERIALIZABLE | refused, as today |
| GC, the sweep of files, cleanup | nothing: an open `cg_t` holds no file |
| seal, fold | they read no CG |
| `ANALYZE`, RG compaction, elimination | they read through `cg_get` |
| `ADD COLUMN`, no CG in an RG | sm3's `added`, as today; no `cg_t` |
| `DROP COLUMN` | the CG is not opened, as today |
| `ALTER COLUMN TYPE` | a rewrite, or the values stay: the same type code, or `text` and `varchar`, which are both varlenas. So the column's type always fits the CG |
| `TRUNCATE`, temporary tables, a bulk load and its rollback | nothing |
| a CG of nulls alone | plain; every flag `TY_VEC_NULL` |
| a CG of no rows | `n` is 0, and no array is read |
| one distinct value | a dictionary of one, with codes of 0 bits |
| a non-deterministic collation | equal strings are distinct dictionary values, as today: two codes may be equal in SQL. An executor must not group by code there |
| NaN, the infinities, -0 | bytes, unchanged |
| a damaged or badly made file | `SM_ERR_CORRUPT` (§5.3) |

---

## 7. The work

1. **Decode into arrays.** `cg_open`, `cg_vec` and `cg_get` in
   `sm2`; sm3 passes the type. → verify: the tests below, and
   the suites of sm, aa and dg with no change of their output.
2. **Measure.** The table of §3 again, with the built code, and
   `rg_read_next` through a scan of `sum(amount)`. → verify:
   fewer instructions for every CG of §3, and none worse.
3. **Documents.** [`COLSTORE.md`](COLSTORE.md) §5.2, whose "a
   read expands each code to its datum" is no longer so, and
   [`SM2_DESIGN.md`](sm2/SM2_DESIGN.md).

**Tests**, in `sm2/test/test_cg.cc`:

- Each type of sm2, with each encoding it can have, with no
  null, some and all: `cg_vec`'s arrays and `cg_get` both give
  the values that were built.
- 0 and 1 rows, sizes around 8, and 10,000.
- Field widths of 0 to 64 bits, with deltas up to the range of
  `int8`.
- Varlenas of 126 and 127 bytes of data, the two sides of the
  header's forms; each with a 4-byte header is aligned.
- Each check of §5.3: a file made bad, with a right CRC32C.
- A body cut short at each of its bytes fails, under the address
  sanitizer.

The scenarios of `sm/sm1/mvcc` and the `.rel` tests get nothing
new: no rule of visibility changes. The column store's `.rel`
tests and `dg/test/columnar.sql` run through the new decode.

---

## 8. What is not in this plan

| Part | Why not now | What it needs |
| --- | --- | --- |
| the batch scan in `sm.h` ([`../REPORT.md`](../REPORT.md) §8) | a step of its own | the flags' copy with `TY_VEC_NOTVALID`, and a hold on a `cg_t` |
| flat values of a dictionary CG, for arithmetic | only an executor needs them: TPC-H's `quantity` and `discount` are dictionaries | one gather operation, in ty or in the batch scan |
| a new body layout: a fixed type as an aligned array, varlenas end to end with no length | this decode is needed first, and the gain is small | measured: 7% smaller in sum, though the `shipdate` dictionary is 24% larger. Decode takes 96 instructions, not 130, and zstd 679, not 748 |
| a faster CRC32C | it changes sm0, not the decode | the machine's CRC32C instruction, where it has one: this ARM machine does. It takes 8 bytes at a time |
| one CRC32C a read, not two | it changes a decision in [`COLSTORE.md`](COLSTORE.md) §5.3 | a decision on which of the two checks to keep |
| a cache of decoded CGs | nothing shows it is needed | a measure |

Once decode is built, the two CRC32Cs are a third of what a read
of a CG costs: 450 of 1,328 in the sums of §3. A faster CRC32C is
the cheapest of these to build, and should be next.

---

## 9. Decided, and open

By the user, on 2026-11-25.

| # | Question | Decision | Rejected |
| --- | --- | --- | --- |
| 1 | how is a null marked? | a flag byte a row, as ty's operations take | a validity bitmap, as [`../REPORT.md`](../REPORT.md) §8.2 has. It is 1/8 the size, but each batch must expand it for ty |
| 2 | what is a string in an array? | a pointer to its varlena: 8 bytes, no copy, and what ty's text functions and PostgreSQL take | `sm_str_t` of [`../REPORT.md`](../REPORT.md) §8.2: 16 bytes, with its first bytes in line, so many compares need no second read, and DuckDB takes it with no conversion. The batch scan can make it from the pointers, if DuckDB is chosen |
| 3 | does a dictionary stay as codes? | yes: a `GROUP BY` or a filter may work on the codes | expand to flat always: a simpler reader, but a code of 2 bits becomes 16 bytes of `decimal128`, and a filter or a `GROUP BY` sees no dictionary |
| 4 | do a file's values change their layout? | no (§8) | yes, at once: §8's new layout |
| 5 | how does a file store nulls? | a flag byte a row: nulls are rare, and zstd squeezes the bytes | a bitmap, expanded at each read |

Question 5, measured on a plain `int4` CG of 10,000 rows, at
`-O3`. The cost is of the nulls alone, in instructions a row: for
the bitmap zstd and a SIMD expand, and for flag bytes zstd, a
check and a copy.

| Nulls | file, no nulls stored | bitmap | flag bytes | cost, bitmap | cost, flag bytes |
| --- | --- | --- | --- | --- | --- |
| one | 42,062 | 42,075 | 42,065 | 1.1 | 1.0 |
| all | 22 | 26 | 26 | 1.0 | 0.9 |
| 10%, in runs of 100 | 37,930 | 37,958 | 38,017 | 1.1 | 1.2 |
| 1%, at random | 42,240 | 42,699 | 42,968 | 2.3 | 3.0 |
| 10%, at random | 41,609 | 42,841 | 44,025 | 1.3 | 10.8 |
| 50%, at random | 28,560 | 29,833 | 31,107 | 3.1 | 12.9 |

The two are equal for few nulls and for nulls in runs. Many nulls
at random cost more as flag bytes, and are rare.

---

## 10. Measured

Instructions a value, as in §3, of the built code at `-O3`. The
files are §3's, but the `int4` one has flag bytes.

| CG | `cg_open` before | `cg_open` | decode | `cg_get` before | `cg_get` |
| --- | --- | --- | --- | --- | --- |
| `_id`, 1 to n | 152 | 28 | 19 | 26 | 27 |
| `returnflag`, 3 texts | 131 | 25 | 18 | 26 | 32 |
| `shipdate`, 2,526 dates | 308 | 66 | 20 | 26 | 29 |
| `quantity`, 50 `decimal64` | 174 | 24 | 18 | 26 | 29 |
| `extendedprice`, `decimal64` | 292 | 208 | 12 | 26 | 27 |
| random `float8` | 269 | 185 | 12 | 26 | 27 |
| random `int4`, 10% null | 249 | 181 | 33 | 25 | 29 |
| `comment`, 10 to 43 bytes | 852 | 422 | 26 | 26 | 30 |
| **sum** | **2,428** | **1,140** | **157** | **207** | **230** |

- **`cg_open` is 2.1 times faster**, and the decode 9 times. The
  checks of §5.3 are the difference from the prototype.
- **`cg_get` costs 1 to 6 instructions more a row**, for the code
  and the varlena's header. Both calls together cost less for
  every CG.

A scan in digres, instructions a row, of a column store table of
100,000 rows on the optimized build (the slope of 10 and 40
prepared statements, under `perf stat`):

| Query | before | after | change |
| --- | --- | --- | --- |
| `sum(a)`, `int4` | 2,436 | 2,231 | -8% |
| `sum(p)`, `decimal64` | 2,614 | 2,410 | -8% |
| `sum(f)`, `float8` | 2,556 | 2,351 | -8% |
| `count(*)` where `k = 'A'`, 3 texts | 2,695 | 2,476 | -8% |
| `count` of five columns | 5,972 | 4,999 | -16% |

**The decode is a small part of a row scan.** The rest, some
2,000 instructions a row, makes each row a tuple, takes it apart
and runs PostgreSQL's executor. The batch scan (§8) removes that.
