# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# digres -- five parts, each with a Makefile of its own (README.md): ty/,
# the operations of the SQL types; sm/, the storage stack; sq/, the SQL
# parser; aa/, roles and privileges; and dg/, digres, which links the
# other four. This file builds and tests
# them together, after ext/: PostgreSQL 18.6 and RocksDB, downloaded and
# built there the first time (ext/Makefile).
#
#   make -j4 ext        ext/ alone
#   DEBUG=1 make -j4    the debug build, dg/build/digres: what tests run
#   make -j4            the release build, dg/build/digres-release: what
#                       the benchmark runs
#   make -j4 test       every part's tests, on the debug build, in order:
#                       ty, sm, sq, aa, then digres; make test-ty,
#                       test-sm, test-sq, test-aa or test-dg, one part's
#   make -j4 tsan       digres under the thread sanitizer, and its tests
#   make -j4 bench      the release build, then bench/run.sh
#   make format         clang-format ty, sm, sq and aa
#   make clean          remove every part's build directory
#
# dg is not formatted here: its tree is formatted by the lines a change
# touches (dg/CODE.md section 8).

.NOTPARALLEL:

DIRS := ty sm sq aa dg

TESTDIRS := $(DIRS:%=test-%)
CLEANDIRS := $(DIRS:%=clean-%)
FORMATDIRS := $(filter-out format-dg,$(DIRS:%=format-%))

# digres builds sq, sm and aa first, as it links them: -O0 -g with
# assertions for the tests, or optimized for the benchmark; both on
# ext/'s RocksDB.
all: ext
ifeq ($(DEBUG),1)
	$(MAKE) -C dg
else
	$(MAKE) -C dg release
endif

# The tests run on the debug build, whatever DEBUG is: sm first, as aa's
# tests link its library, and digres last, as it runs on all three.
test: ext $(TESTDIRS)

$(TESTDIRS): export DEBUG := 1
$(TESTDIRS):
	$(MAKE) -C $(@:test-%=%) test

tsan: ext
	$(MAKE) -C dg tsan

bench: ext
	$(MAKE) -C dg release
	bench/run.sh

# PostgreSQL 18.6 for the tests and the benchmark, and RocksDB; once.
ext:
	$(MAKE) -C ext

format: $(FORMATDIRS)

$(FORMATDIRS):
	$(MAKE) -C $(@:format-%=%) format

clean: $(CLEANDIRS)

$(CLEANDIRS):
	$(MAKE) -C $(@:clean-%=%) clean

.PHONY: $(TESTDIRS) $(CLEANDIRS) $(FORMATDIRS)
.PHONY: all ext test tsan bench format clean
