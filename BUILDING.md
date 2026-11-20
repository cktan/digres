# Building digres

What digres needs, and how to build it: the debug build for
the tests, the release build for the benchmark, and the TSan
build. [`QUICKSTART.md`](QUICKSTART.md) has the short path,
from a fresh checkout to a query.

## Prerequisites

Developed on Ubuntu 24.04, aarch64. It also builds on x86_64,
and with GCC 15 (Ubuntu 26.10).

**`./SETUP_UBUNTU_DEV.sh`** does all of this section on a
minimal Ubuntu machine. Each step is skipped once its result is
there, so it can be run again after a failure; `SRC=` sets where
it clones Janet, `~/p` by default. It does:

- **apt packages**: the list below, and `ca-certificates`.
- **Janet**: commit `86ca958e`, installed into `/usr/local`.
- **`PATH`**: `ext/pg18.6/install/bin` first, in `~/.profile`.

By hand, on Ubuntu:

```
sudo apt install build-essential cmake git wget flex bison python3 \
    clang-format libicu-dev \
    libsnappy-dev liblz4-dev libzstd-dev zlib1g-dev libbz2-dev \
    libreadline-dev libssl-dev openssl pkg-config bc \
    linux-tools-generic
```

`libssl-dev` is OpenSSL, which digres always links for TLS.
`wget` downloads `ext/`'s sources. `libsnappy-dev` to
`libbz2-dev` are RocksDB's compression libraries.
`libreadline-dev` and `pkg-config` are for building PostgreSQL
18.6, `openssl` for the certificates its TLS tests make, `bc`
for `bench.old/btree.sh`. No package holds PostgreSQL or
RocksDB: `ext/` builds both, with the server, `psql`,
`pgbench` and `libpq`.

- **`ext/`: PostgreSQL 18.6 and RocksDB**, downloaded with
  `wget` and built there by `make` the first time
  ([`ext/Makefile`](ext/Makefile)); `make -j4 ext` builds them
  alone. Both take a while, once.
  - `ext/pg18.6`, PostgreSQL 18.6, the version digres forked,
    built in its source tree with OpenSSL and installed into
    its `install/`, optimized, with no `--enable-cassert`.
    pgregress, the isolation specs and the regression suite run
    its `psql`, `pg_regress` and isolation tester; its `psql`
    tests digres's TLS (`dg/test/tls.sh`); the benchmark compares
    digres with its server
    ([`bench.old/DESIGN.md`](bench.old/DESIGN.md#2-optimized-builds) §2).
    `PG18_SRC` names another tree.
  - `ext/rocksdb-11.13.2`, RocksDB, static: `release/`,
    `-O3 -DNDEBUG`, which the debug and release builds both link,
    and `tsan/`, built by `make tsan`. `ext/ext.mk` names the
    version; a new one is a new directory.
- **Janet**, for `sm/rel`, which embeds its interpreter: built
  from source and installed into `/usr/local`, where
  `sm/rel/Makefile` looks for `janet.h` and `libjanet`. The
  tests run on 1.43.0-dev, commit `86ca958e`.

  ```
  git clone https://github.com/janet-lang/janet.git
  cd janet && git checkout 86ca958e
  make -j4 && sudo make install && sudo ldconfig
  ```
- **`psql` and `pgbench`**, `ext/pg18.6`'s: digres serves
  PostgreSQL's own clients. Put them on `PATH` by hand: `export
  PATH=$HOME/p/digres/ext/pg18.6/install/bin:$PATH`, in
  `~/.profile` to keep it ([`RUNNING.md`](RUNNING.md)). dg's
  tests put them first themselves.

## Building

From the top, as each part's own Makefile is driven by the
one there (`Makefile`):

```
export DEBUG=1; make -j4    # debug: dg/build/digres
unset DEBUG; make -j4       # release: dg/build/digres-release
make -j4 tsan               # dg/build/digres-tsan, and its tests
make format                 # clang-format ty, sm, sq and aa
make clean
```

| Part | Debug, `DEBUG=1 make -j4` | Release, `make -j4` |
| --- | --- | --- |
| dg | `dg/build/digres`, `-O0 -g`, with `Assert()` | `dg/build/digres-release`, `-O2 -DNDEBUG` |
| sm | `sm/build/libsm.a`, `-O0 -g`, with `assert()` | `sm/build/release/libsm.a`, `-O3 -DNDEBUG` |
| aa | `aa/build/libaa.a`, `-O0 -g`, with `assert()` | `aa/build/release/libaa.a`, `-O3 -DNDEBUG` |
| sq | `sq/build/libsq.a`, `-O0 -g`, with `assert()` | `sq/build/libsq-release.a`, `-O2 -DNDEBUG` |
| RocksDB 11.13.2 | `ext/`'s, `-O3 -DNDEBUG`, static | the same |
| For | the tests, and running by hand | the benchmark |

The TSan build, `dg/build/digres-tsan`, is digres under the
thread sanitizer, sm and RocksDB instrumented with it: for
changes to threads, latches, locks or shared state.

- **`make clean` keeps `ext/`**: it is outside every `build/`.
  `make -C ext clean` removes it, and the next `make` downloads
  and builds it again.
- **`digres --build` says how a binary was built**: the flags
  of digres, sq and sm, and which RocksDB it links. A release
  build shows `-DNDEBUG` on each, and RocksDB `static`.
- **Flags are not tracked** by the make of dg and sq: after
  changing them, delete `dg/build/rel` and `sq/build/rel` before
  building the release again.
- **A part alone** builds in its own directory, as
  `make -C sm/sm1 -j4`, `DEBUG=1` or not.
- **`make format`** before a commit. dg is formatted by the
  lines a change touches ([`dg/CODE.md`](dg/CODE.md#8-style) §8).
