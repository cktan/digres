# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# ext -- the outside software digres builds with, downloaded by wget
# and built here once: PostgreSQL 18.6 and RocksDB (ext.mk names the
# version and the paths).
#
#   make -C ext -j4        both: PostgreSQL, and RocksDB's release build
#   make -C ext pg         PostgreSQL alone
#   make -C ext rocksdb-release, rocksdb-tsan
#                          one of RocksDB's static libraries
#   make -C ext clean      remove what is built and downloaded
#
# pg18.6/ is PostgreSQL 18.6's source tree, built in place with OpenSSL
# and installed into pg18.6/install. Its psql, pgbench, pg_regress and
# isolation tester run digres's tests, its regression SQL is sq's
# corpus, and its server is what the benchmark compares digres against.
#
# rocksdb-VERSION/ is RocksDB's source. Its release/ holds the library
# the debug and release builds of sm, aa and digres link, optimized and
# without assertions; its tsan/ the one built with -fsanitize=thread,
# whose handovers between threads the sanitizer can see. Each is
# static, with the system's compression libraries.
#
# The downloads are kept in dl/. Each build's output goes to a .log
# beside it, and its tail is printed when it fails.

include ext.mk

PG_TARBALL := dl/postgresql-18.6.tar.bz2
PG_URL := https://ftp.postgresql.org/pub/source/v18.6/postgresql-18.6.tar.bz2
ROCKSDB_TARBALL := dl/rocksdb-$(ROCKSDB_VERSION).tar.gz
ROCKSDB_URL := https://github.com/facebook/rocksdb/archive/refs/tags/v$(ROCKSDB_VERSION).tar.gz

# psql is installed last: when it is there, the whole build is.
PG_DONE := $(PG18)/install/bin/psql

.PHONY: all pg rocksdb-src rocksdb-release rocksdb-tsan clean

all: pg rocksdb-release

pg: $(PG_DONE)
rocksdb-src: $(ROCKSDB_SRC)
rocksdb-release: $(ROCKSDB_REL)
rocksdb-tsan: $(ROCKSDB_TSAN)

# A download goes to a .part first, so one cut short is not taken whole.
$(PG_TARBALL) $(ROCKSDB_TARBALL):
	@mkdir -p dl
	wget -q -O $@.part $(if $(filter $(PG_TARBALL),$@),$(PG_URL),$(ROCKSDB_URL))
	mv $@.part $@

# Unpack into a fresh directory, then stamp it.
$(PG18)/.unpacked: $(PG_TARBALL)
	rm -rf $(PG18)
	mkdir -p $(PG18)
	tar -xjf $< -C $(PG18) --strip-components=1
	touch $@

$(ROCKSDB_SRC): $(ROCKSDB_TARBALL)
	rm -rf $(ROCKSDB_DIR)
	mkdir -p $(ROCKSDB_DIR)
	tar -xzf $< -C $(ROCKSDB_DIR) --strip-components=1
	touch $@

# PostgreSQL with OpenSSL, so its psql can test digres's TLS. The tree's
# own make builds pg_regress and the isolation tester with the server.
# Its make runs as a top-level one, MAKELEVEL 0: PostgreSQL generates its
# headers (utils/errcodes.h and others) only at the top level.
$(PG_DONE): $(PG18)/.unpacked
	cd $(PG18) && ./configure --prefix=$(PG18)/install \
	    --with-ssl=openssl > $(EXT)/pg18.6.configure.log 2>&1 || \
	    { tail -20 $(EXT)/pg18.6.configure.log; exit 1; }
	MAKELEVEL=0 $(MAKE) -C $(PG18) > $(EXT)/pg18.6.build.log 2>&1 || \
	    { tail -20 $(EXT)/pg18.6.build.log; exit 1; }
	MAKELEVEL=0 $(MAKE) -C $(PG18)/src/test/isolation >> $(EXT)/pg18.6.build.log 2>&1 || \
	    { tail -20 $(EXT)/pg18.6.build.log; exit 1; }
	MAKELEVEL=0 $(MAKE) -C $(PG18) install >> $(EXT)/pg18.6.build.log 2>&1 || \
	    { tail -20 $(EXT)/pg18.6.build.log; exit 1; }

# The flags of each of RocksDB's builds; the name of its directory picks
# them. flags beside the library is what "digres --build" reports.
ROCKSDB_FLAGS_release := -O3 -DNDEBUG
ROCKSDB_FLAGS_tsan := -O1 -g -fsanitize=thread

# A static library of the store alone: no tools, tests or benchmarks,
# and no io_uring, so it links with the compression libraries alone.
$(ROCKSDB_DIR)/%/librocksdb.a: $(ROCKSDB_SRC)
	cmake -S $(ROCKSDB_DIR) -B $(ROCKSDB_DIR)/$* -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_C_FLAGS="$(ROCKSDB_FLAGS_$*)" \
	    -DCMAKE_CXX_FLAGS="$(ROCKSDB_FLAGS_$*)" \
	    -DCMAKE_C_FLAGS_RELEASE="" -DCMAKE_CXX_FLAGS_RELEASE="" \
	    -DPORTABLE=1 -DFAIL_ON_WARNINGS=OFF -DUSE_RTTI=1 \
	    -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON -DWITH_ZLIB=ON \
	    -DWITH_BZ2=ON -DWITH_GFLAGS=OFF -DWITH_LIBURING=OFF \
	    -DWITH_TESTS=OFF -DWITH_TOOLS=OFF -DWITH_BENCHMARK_TOOLS=OFF \
	    -DWITH_CORE_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF \
	    -DROCKSDB_BUILD_SHARED=OFF > $(EXT)/rocksdb-$*.cmake.log 2>&1 || \
	    { tail -20 $(EXT)/rocksdb-$*.cmake.log; exit 1; }
	cmake --build $(ROCKSDB_DIR)/$* --target rocksdb -j4 \
	    > $(EXT)/rocksdb-$*.build.log 2>&1 || \
	    { tail -20 $(EXT)/rocksdb-$*.build.log; exit 1; }
	echo "$(ROCKSDB_FLAGS_$*)" > $(ROCKSDB_DIR)/$*/flags

clean:
	rm -rf dl pg18.6 rocksdb-* *.log
