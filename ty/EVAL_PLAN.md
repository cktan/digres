# EVAL_PLAN: an evaluation engine in ty

Status: a design for review. Nothing is built. §10 lists what the
user decided.

ty gets an engine that evaluates an expression. Its text has
Janet's syntax. The engine compiles the text into bytecode, and
a stack machine runs the bytecode, on one value or on a batch of
rows. Each instruction is one call of ty: a scalar operation
(`ty_int32_add`) or a vector operation (`tyvec_int32_add`).

Nothing calls the engine yet. It is built and tested in ty
alone, as the vector operations were. A later step wires it into
sm, for CHECK constraints, generated columns and expression
indexes, or into a columnar executor.

---

## 1. A worked example

A filter on a batch: `x > lim + 1 AND y < 10`, where `x` and `y`
are columns and `lim` is a parameter of the statement.

```janet
(fn [x:i32.v y:i32.v lim:i32]
  (and.bool.v
    (cmp.gt.i32.v x (splat.i32 (add.i32 lim 1)))
    (cmp.lt.i32.v y (splat.i32 10))))
```

- **A name says everything**: `cmp.gt.i32.v` is a compare of two
  vectors of `int32`. Without `.v` it is of two scalars. Nothing
  is inferred, and nothing is cast that the text does not cast.
- **The parameters say their types**, and which are vectors.
- **Scalar parts run once a batch**: `(add.i32 lim 1)`. `splat`
  makes a vector of a scalar, for the vector compare.

The bytecode, a stack machine's:

```
 0  load      x            ; a vector: its flags are copied
 1  load      lim          ; a scalar
 2  const     1
 3  add.i32                ; ty_int32_add, once
 4  splat.i32
 5  cmp.gt.i32.v           ; tyvec_int32_gt, on n rows
 6  load      y
 7  const     10
 8  splat.i32
 9  cmp.lt.i32.v
10  and.bool.v             ; tyvec_bool_and
11  ret
```

The host runs it on each batch:

```c
ty_prog_t *prog;
ty_run_t *run;
ty_prog_compile(text, len, &prog);   /* once */
ty_run_new(prog, 2048, &run);        /* once a thread */
for (each batch) {
  ty_arg_t args[3] = {{x, xflag}, {y, yflag}, {&lim, &limflag}};
  ty_result_t r;
  if (0 != ty_run(run, n, args, &r)) /* the lowest failing row */
    ... ty_errnum(), ty_erridx() ...
  /* r.values and r.flags: n bools, valid until the next run */
}
```

---

## 2. Terms

| Term | Meaning |
| --- | --- |
| program | the compiled form of one text: its bytecode and its constants. Read only, and shared by threads |
| run | one thread's state for a program: its stack's arrays, sized for a batch of at most `max_n` rows |
| slot | one place on the stack: a scalar, or a vector of n rows, with its flags and its errors |
| mark | a row's error, put in its flags in place of a failure of the whole call |
| arm | one of the branches of `if` or `cond`, or the right side of `and` or `or` |

---

## 3. Errors are marks

**An operation that can fail no longer fails the call.** It marks
each row that fails, and goes on. A mark is a bit in the row's
flag and an entry in an error list. The other rows are computed.

```c
#define TY_VEC_NOTVALID 0x01
#define TY_VEC_NULL     0x02
#define TY_VEC_ERROR    0x04 /* new: the row failed */
#define TY_VEC_SKIP     (TY_VEC_NOTVALID | TY_VEC_NULL | TY_VEC_ERROR)

typedef struct {
  uint32_t row;
  uint8_t  code; /* TY_ERR_RANGE, _DIVZERO, _UNDERFLOW, ... */
} ty_rowerr_t;

/* 0 if no row failed; 1 if some did, marked in flag_a and err */
int tyvec_int32_div(uint32_t n, uint8_t *flag_a, const int32_t *a,
                    const uint8_t *flag_b, const int32_t *b,
                    int32_t *out, std::vector<ty_rowerr_t> &err);
```

- **`err` takes an entry for each failing row**, appended in the
  order the errors happen. Nothing is written to it when no row
  fails, which is nearly always. An operation appends its own
  alone; the run keeps a list for each slot, and adds `b`'s with
  `tyvec_err_merge` after the call.
- **Each input comes with its flags**, and the operation ORs them
  into `flag_a` as it computes. `flag_b` may be NULL.
- **A marked row is not computed again.** `TY_VEC_SKIP` has the
  error bit, so a later operation skips the row, as it skips a
  null.
- **Only an operation that can fail takes `err`**: the
  arithmetic of the integers and the floats, and a cast that can
  overflow. A compare, `not` and the IS tests cannot fail. They
  keep the call they have.
- **A caller that wants the error at once asks.**
  `tyvec_flag_error(n, flag, err)` returns -1 with `ty_errnum()`
  and `ty_erridx()` of the lowest valid row that is marked, or
  0.
- **The undo code stays.** When `out` is over an input, a block
  that failed must be computed again, row by row, to find each
  row that failed. That needs the input, which `undo_a`, `undo_b`
  and `saved[]` in `src/vec.cc` give back.

**The rules of a mark**, row by row, as PostgreSQL evaluates
left to right:

| Operation | A row with a marked input gives |
| --- | --- |
| an operator of two inputs | marked; with `a`'s error if both are marked |
| `not`, a cast | marked |
| `isnull`, `isnotnull`, `istrue`, `isfalse` | marked: PostgreSQL raises before it tests |
| `and` | `false` if `a` is `false`, whatever `b` is; else marked if `a` or `b` is |
| `or` | `true` if `a` is `true`, whatever `b` is; else marked if `a` or `b` is |
| `if`, `cond` | the chosen arm's row; marked if the condition is |
| `splat` of a marked scalar | every row marked, with the scalar's code |

- **Only the left side of `and` and `or` drops an error.**
  `false AND <error>` is `false`, but `<error> AND false` is an
  error, as in PostgreSQL. A test against PostgreSQL then agrees
  on errors too.
- **An error beats a null.** `NULL + 1/0` raises in PostgreSQL,
  as the arguments are computed first.
- **A dropped error leaves the list.** The operation that drops
  a row's mark drops its entry, so no entry names a row that did
  not fail.

**The run raises.** When a program ends, `ty_run` fails if a
valid row of the result is marked. It reports the lowest such
row, with that row's code. That is the row and the error
PostgreSQL would raise, going row by row, but for the order of a
`WHERE`'s conditions, which PostgreSQL does not promise either.

---

## 4. The language

The syntax is Janet's: forms in parentheses, a vector in
brackets for the parameters. It is not Janet: no macro, no
function value, no loop and no variable that changes. A program
is one `fn` form. The reader is ty's own, a few hundred lines.

| Form | Meaning |
| --- | --- |
| `(fn [p:t ...] e)` | the program: its parameters, each with its type, `.v` for a vector |
| `123`, `-4`, `1.5`, `true`, `false`, `nil` | a constant; its type is the operator's operand |
| `x` | a parameter, or a name `let` gave |
| `(let [n e ...] e)` | names for values, each computed once |
| `(op.t a b)`, `(op.t.v a b)` | an operation of type `t`, on scalars or on vectors |
| `(cast.from.to e)`, `.v` | a cast |
| `(splat.t e)` | a vector of `n` rows, each the scalar `e` |
| `(if.t c a b)`, `.v` | `a` where `c` is true, else `b` |
| `(cond.t c1 a1 c2 a2 ... e)`, `.v` | the first `a` whose `c` is true, else `e` |
| `(and.bool a b)`, `(or.bool a b)`, `.v` | SQL's three values, with §3's errors |

**The types** are LLVM's short names: `i16`, `i32`, `i64`, `f32`,
`f64`, `bool`. The first version has these alone (§9).

**The operations**, each `.t` and `.t.v`:

| Name | C function | Types |
| --- | --- | --- |
| `add`, `sub`, `mul`, `div` | `ty_int32_add`, `tyvec_int32_add`, ... | the integers, the floats |
| `mod` | `ty_int32_mod`, ... | the integers |
| `cmp.eq`, `cmp.ne`, `cmp.lt`, `cmp.le`, `cmp.gt`, `cmp.ge` | `ty_int32_eq`, `tyvec_int32_eq`, ... | all |
| `not`, `and`, `or` | `ty_bool_not`, `tyvec_bool_not`, ... | `bool` |
| `isnull`, `isnotnull`, `istrue`, `isfalse` | `tyvec_flag_isnull`, ... | all; `istrue` and `isfalse` of `bool` |
| `cast.from.to` | `ty_int32_to_int16`, ... | between the six |

- **A name maps to one C function**, letter for letter but the
  type: `i32` is `int32`, `f64` is `fp64`.
- **The compiler checks every operand** against its operator:
  its type, and whether it is a vector. A mismatch is a compile
  error, with its line and column.

---

## 5. The bytecode

The compiler reads the text into forms, checks the types, and
emits the bytecode in one pass.

- **An instruction is a 16-bit opcode and a 16-bit operand**: an
  index into the constants or the parameters, or the target of a
  jump.
- **The stack's depth, and the type of each slot at each point,
  are known at compile time.** So a run has its arrays when it
  is made, and a batch allocates nothing.
- **A vector arm never jumps.** Each arm of `if.t.v` and
  `cond.t.v` runs on every row, and the select takes each row's
  value, flags and mark from its arm (§3). A program of vectors
  is a straight line.
- **A scalar arm jumps.** `if.t`, `cond.t`, `and.bool` and
  `or.bool` emit `br` and `br.if`, so an arm that is not needed
  does not run. A scalar that fails in an arm that runs is
  marked, as a row is: a scalar slot has a flag and an error
  entry. Then a `splat` in a vector arm carries the mark to its
  rows, and an arm that drops it drops it.
- **The instruction set lives in memory only.** A catalog keeps
  the text and compiles it again. The opcodes may change in any
  build, and need no version.

### 5.1 What a run holds

| Part | Size |
| --- | --- |
| a vector slot | `max_n` values of 8 bytes, `max_n` flag bytes, and an error list |
| a scalar slot | one value, one flag, one error entry |
| the constants | the program's, shared |

- **A `load` of a vector parameter copies its flags**, as an
  operation writes over its flags. Its values are not copied: a
  slot may point at the caller's array.
- **The stack's slots are reused.** A binary operation writes
  over its left operand's slot, as each operation allows `out` to
  be `a`.
- **A program and a run are apart.** A program is read only, and
  many threads share it. A run is one thread's.

---

## 6. The interface

```c
/* Compile text. -1 with TY_ERR_INVALID and "line:col: why". */
int  ty_prog_compile(const char *text, size_t len, ty_prog_t **out);
void ty_prog_free(ty_prog_t *prog);

/* The bytecode as text, one instruction a line: for tests. */
int  ty_prog_dump(const ty_prog_t *prog, ty_varlena_t **out);

/* A thread's state for batches of at most max_n rows. */
int  ty_run_new(const ty_prog_t *prog, uint32_t max_n, ty_run_t **out);
void ty_run_free(ty_run_t *run);

/* An input: a vector's values and flags, or a scalar's. */
typedef struct {
  const void    *values;
  const uint8_t *flags;   /* NULL: no row is null */
} ty_arg_t;

/* The result: n values and flags, valid until the next run. */
typedef struct {
  const void    *values;
  const uint8_t *flags;
} ty_result_t;

/* Run on n rows. -1 with the lowest valid marked row's error. */
int  ty_run(ty_run_t *run, uint32_t n, const ty_arg_t *args,
            ty_result_t *out);
```

- **The memory is the thread's alloc function** (`ty_set_alloc`),
  taken by `ty_prog_compile` and `ty_run_new`, and never by
  `ty_run`.
- **A flags pointer of NULL means no null**, so a column with no
  null costs no copy of a flag it does not have: NULLs are rare.
- **A scalar program is a run with `n` of 1**, whose parameters
  are all scalars.

---

## 7. The cases

| Case | What happens |
| --- | --- |
| `(or (= b 0) (> (/ a b) 1))`, vector | the division marks the rows where `b` is 0; the `or` drops their marks, as its left side is true there |
| the same, scalar | `br.if` jumps past the division when `b` is 0 |
| `(and (> (/ a b) 1) (= b 0))` | an error where `b` is 0, as in PostgreSQL: the left side's error is not dropped |
| `(if.i32.v (cmp.eq.i32.v b z) z (div.i32.v a b))` | the division marks the rows where `b` is 0; the select takes the other arm there |
| a null and an error in one row | the error |
| a row not valid on input | never computed, never marked, never raised |
| every row of a batch fails | `err` takes `n` entries; the run raises the lowest valid one |
| an input of another type than declared | the caller's error: the program reads it as declared |
| a text that is no program | `ty_prog_compile` fails: its line, its column and why |
| a run of more than `max_n` rows | `TY_ERR_INVALID` |
| two threads, one program | each has its own run |

---

## 8. The work

1. **Marks in the vector operations.** `TY_VEC_ERROR`,
   `ty_rowerr_t`, the `err` parameter, the return of 0 or 1,
   `tyvec_flag_error`, `tyvec_err_merge`; `and` and `or` take
   §3's rules; each input with its flags. → verify:
   `test/test_vec.cc` changed to marks; a row is marked exactly
   where the scalar operation fails; no row is marked twice.
   Built: `and` costs 18% more for the error, 3,664 instructions
   for 2,048 rows, from 3,096, and `or` 16%: 4 more instructions
   for 16 rows. `add.i32` costs 0.8% more. With its two inputs'
   flags merged in its loop, `add.i32` of 2,048 rows costs 6,843
   instructions, where a merge first and then the add cost 7,375.
2. **The reader.** Janet's forms: lists, brackets, symbols,
   numbers, `true`, `false`, `nil`, comments. → verify: forms
   read back, and each malformed text gives its line and
   column.
3. **The compiler.** The table of names (§4), the type checks,
   the stack's layout, the jumps of scalar arms, `ty_prog_dump`.
   → verify: the dump of §1's example, and an error for each
   mismatch.
4. **The machine.** `ty_run_new`, `ty_run`. → verify: §7's
   cases.
5. **Random programs.** A generator writes a program of vectors
   and its twin with each `.v` taken out. The vector program on a
   batch must give, in each row, what the scalar one gives on
   that row: its value, its null, and its error. → verify: some
   millions of rows, with nulls and with values that fail.
6. **Measure.** Instructions a row of §1's example, against the
   same calls written by hand in C. → verify: the machine adds
   less than 5%, at a batch of 2,048.

---

## 9. What is not in this plan

| Part | Why not now |
| --- | --- |
| decimals, dates, text | they have no vector operations yet; text needs memory for its results |
| vector-with-constant operations | `splat` serves; one costs a copy of `n` values |
| aggregates | a program is an expression; an aggregate keeps a state across batches |
| a caller in sm or in digres | the step after; this plan makes the engine and its tests |
| the bytecode on disk | §5: the text is kept |

---

## 10. Decided

By the user, on 2026-11-25.

| Question | Decision | Rejected |
| --- | --- | --- |
| what a run evaluates | both: scalars and vectors, with an instruction of each | vectors alone, with a row a batch of 1 |
| the syntax | Janet's alone; ty's own reader | Janet's library, or Janet whole |
| the language | expressions | aggregates; a programming language |
| types | explicit in every name, and nothing inferred | inferred from the inputs, with or without SQL's casts |
| vector and scalar in one text | named in the text, and they mix; `splat` | one text, compiled two ways |
| names | `op.type[.v]`, LLVM's short types: `add.i32.v` | ty's type names; LLVM's own opcodes |
| the control forms | typed, as the operators: `if.i32.v` | inferred from the arms |
| the first caller | none: ty's tests | sm; a columnar executor |
| what is kept | the text; bytecode in memory only | versioned bytecode |
| a failing row | marked in its flag and an error list, and the call goes on | the call fails at once |
| how a caller chooses | always mark; `tyvec_flag_error` asks | a second function; a parameter |
| the error list | `(row, code)` entries in the order the errors happen, in a `std::vector` the caller passes | a code in the flag's bits; a code a row |
| `and`, `or` | only the left side drops the right's error | either side |
| who raises | the run, at its end | the caller |
| scalar arms | jump | compute all, as vector arms |
| the first types | those with vector operations | all of ty's scalars; the integers alone |
| memory | a run's, sized at compile | an alloc each run |
