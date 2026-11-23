#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# alone.sh -- verify that the library and its tests are built from
# nothing outside sq: not digres's, and not sm's.
# 1. Read the compiler's dependency files for the library, what is
#    generated into it, its tests and the tools they run.
# 2. Confirm no file under ../ is named in them. The tests alone may name
#    ../ext/doctest/, the test framework: outside software, as RocksDB
#    is, and no part of digres or sm.
cd "$(dirname "$0")/.." || exit 1
bad=$(
  grep -l '\.\./' build/src/*.d build/gen/*.d build/sq_*.d
  for f in build/test/*.d; do
    sed 's|\.\./ext/doctest/||g' "$f" | grep -q '\.\./' && echo "$f"
  done
)
if [ -n "$bad" ]; then
  echo "$bad"
  echo "alone: FAILED (the files above depend on files outside sq)"
  exit 1
fi
echo "alone: PASSED"
