# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# ext.mk -- where ext/ keeps the outside software it builds, for the
# Makefiles that use it (ext/Makefile says what it builds). Include it
# by its path from the including Makefile:
#
#   include ../../ext/ext.mk
#
# A library missing when a link wants it is built by ext/Makefile.

EXT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# RocksDB, the store under sm. A new version is a new directory: the
# version below names it, so changing it builds the new one.
ROCKSDB_VERSION := 11.13.2
ROCKSDB_DIR := $(EXT)/rocksdb-$(ROCKSDB_VERSION)
# Its source, unpacked: a stamp, as tar keeps the archive's times.
ROCKSDB_SRC := $(ROCKSDB_DIR)/.unpacked
ROCKSDB_INC := $(ROCKSDB_DIR)/include
# The static libraries: optimized without assertions, which the debug
# and release builds both link; and under the thread sanitizer.
ROCKSDB_REL := $(ROCKSDB_DIR)/release/librocksdb.a
ROCKSDB_TSAN := $(ROCKSDB_DIR)/tsan/librocksdb.a
# The compression libraries a static RocksDB needs, the system's.
ROCKSDB_DEPS := -lsnappy -llz4 -lzstd -lz -lbz2

# doctest, the unit tests' framework: one header, with digres's --echo
# (-E), and the listener every test binary links (doctest/doctest.h).
DOCTEST_INC := $(EXT)/doctest

# PostgreSQL 18.6, the version digres forked: its source tree, built
# in place, and installed into its install/.
PG18 := $(EXT)/pg18.6

# The rules that hand a missing file to ext/Makefile, in every Makefile
# but that one. They keep the including Makefile's default goal: one set
# empty is taken by the next target that file names.
ifneq ($(abspath $(CURDIR)),$(EXT))
ext_goal := $(.DEFAULT_GOAL)
$(ROCKSDB_SRC):
	$(MAKE) -C $(EXT) rocksdb-src
$(ROCKSDB_REL):
	$(MAKE) -C $(EXT) rocksdb-release
$(ROCKSDB_TSAN):
	$(MAKE) -C $(EXT) rocksdb-tsan
.DEFAULT_GOAL := $(ext_goal)
endif
