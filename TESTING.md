# Testing digres

```
make -j4 test
```

It runs every part's tests on the debug build, whatever
`DEBUG` is (`test.sh`). It builds each part once, then runs the
tests of ty, sm's levels and rel, sq, aa and digres side by side,
each to a log of its own, `build/test-logs/PART.log` (a `/` of
the part's name a `-`). A line says when each part ends, and how
long the run had taken; a run on this repository's 12-core
machine, its build already made, took 13 minutes. One run goes
at a time: a second refuses while the first holds its lock.

```
built in 4 s; testing: ty sm/sm0 sm/sm1 sm/sm2 sm/sm3 sm/rel sq aa dg
sq: PASSED (6 s)
ty: PASSED (6 s)
sm/sm0: PASSED (11 s)
aa: PASSED (81 s)
sm/rel: PASSED (118 s)
sm/sm2: PASSED (171 s)
sm/sm3: PASSED (497 s)
sm/sm1: PASSED (629 s)
dg: PASSED (761 s)
test: PASSED (ty sm/sm0 sm/sm1 sm/sm2 sm/sm3 sm/rel sq aa dg)
```

`./test.sh sm dg` runs those parts alone, sm as its levels and
rel; `./test.sh sm/sm1` one level. A commit on a `wip-`
branch runs the parts it changes and the parts that build on
them; a merge into `main` runs them all
([`CLAUDE.md`](CLAUDE.md)). digres's log ends with these lines:

```
run: PASSED (124 files)
run: PASSED (50 files, alone and in 8 sessions)
initdb: PASSED
server: PASSED
slt: PASSED (65 files on digres, 11076 records, 19 skipped)
all 231 ok
```

One part's alone is `make -j4 test-ty`, `test-sm`, `test-sq`,
`test-aa` or `test-dg`, on the debug build too. The output is
long: save it to a file and read that.

**Which test is running**: `make test` runs each doctest binary
with `-E` (`--echo`), which prints each test case's number and
name as it starts, and its time if it took 5 seconds or more. A
file beside the binary, `BINARY.active`
(`sm/sm2/build/test_sm2.active`, for one), holds the test case
it is in, its name, then its file and line. It goes when the run
ends; a run that crashed or hangs leaves it naming its test
(`ext/doctest/doctest_active.h`). Both live in `ext/doctest/`,
with the one copy of doctest.h.

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

