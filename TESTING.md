# Testing digres

```
make -j4 test
```

It runs every part's tests on the debug build, whatever
`DEBUG` is, in order: ty, sm, sq, aa, then digres, which runs on
the other three, stopping at the first that fails. Every
commit that touches source passes it. digres's part ends with
these lines:

```
run: PASSED (124 files)
run: PASSED (12 files, alone and in 8 sessions)
initdb: PASSED
server: PASSED
slt: PASSED (49 files on digres, 10711 records, 11 skipped)
all 231 ok
```

One part's alone is `make -j4 test-ty`, `test-sm`, `test-sq`,
`test-aa` or `test-dg`, on the debug build too. The output is
long: save it to a file and read that.

**Which test is running**: while a doctest binary runs, a file
beside it, `BINARY.active` (`sm/sm2/build/test_sm2.active`, for
one), holds the test case it is in, its name, then its file and
line. It goes when the run ends; a run that crashed or hangs
leaves it naming its test
(`sm/sm0/include/sm0_doctest_active.h`).

```
cat sm/*/build/*.active aa/build/*.active sq/build/*.active 2>/dev/null
```

What each is:

- **sm**: each level's unit tests (`sm/smN/test`), the
  isolation scenarios of sm1's concurrency control
  (`sm/sm1/mvcc`, its specification: change one only with
  the maintainer's approval), and `rel` (`sm/rel/test`), which
  drives `sm.h` from scripts.
- **sq**: the parser's tests, unit tests and the SQL files of
  `sq/test`; `make -C sq corpus-test` checks it over
  PostgreSQL's regression SQL.
- **aa**: its unit tests.
- **dg**: the `.sql` files of `dg/test`, alone and in eight
  sessions; `initdb`; `server.sh`, a server driven by `psql`; the
  sqllogictest files of `dg/test/slt`, whose results are
  PostgreSQL 18's
  ([`dg/test/slt/README.md`](dg/test/slt/README.md)); and
  pgregress, PostgreSQL 18's regression files with what digres
  does not run commented out, against PostgreSQL's output
  ([`dg/test/pgregress/README.md`](dg/test/pgregress/README.md)).

**Keep test data off `/tmp`.** Set `TMPDIR` to a directory on a
real disk, on every machine, as `/tmp` may be in memory:

```bash
mkdir -p bench/build/tmp
export TMPDIR=$PWD/bench/build/tmp
```

Run on their own, outside `make test`:

| Command | What it runs |
| --- | --- |
| `make -C dg -j4 slt` | every sqllogictest file, `select5.test` too |
| `dg/test/slt.sh FILE...` | some of them; `--pg18` runs them on PostgreSQL 18 instead |
| `make -C dg -j4 pgregress` | the ported regression files alone |
| `make -C dg -j4 isolation` | PostgreSQL 18's isolation specs, against `dg/test/isolation_specs.txt`; for any change to locks or waits; ten minutes |
| `make -C dg -j4 regress` | PostgreSQL 18's whole regression suite, unported: only measures what is missing |
| `make -j4 tsan` | the TSan build and its tests; for any change to threads, latches, locks or shared state |

A test opens with a one-line synopsis and numbered steps.
Before it is committed, it is shown to fail by breaking what
it checks.

