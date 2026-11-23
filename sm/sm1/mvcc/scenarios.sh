#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# scenarios.sh -- run every isolation scenario, versioned and chained, JOBS
# at a time, and compare each transcript with the expected one
# (DRIVER.md §7). A difference is a failure: good/ is the claim, out/ is
# what the engine did.
#
#   mvcc/scenarios.sh DRIVER OUT GOOD JOBS SCRIPT...
#
# Each run of the driver has a home directory of its own, so runs side by
# side see nothing of each other.
# 1. Remove the differences of an earlier run.
# 2. Run each scenario twice, versioned then chained, JOBS scenarios at a
#    time; a run that fails or differs leaves OUT/NAME.diff.
# 3. Print each difference, and fail if there is one.
driver=$1 out=$2 good=$3 jobs=$4
shift 4
mkdir -p "$out"
# 1. No difference of an earlier run.
rm -f "$out"/*.diff
# 2. Each scenario, in a shell of its own.
export driver out good
printf '%s\n' "$@" | xargs -P "$jobs" -I{} bash -c '
  f={}
  b=$(basename "$f" .t)
  for mode in versioned chained; do
    name=$b
    flag=
    if [ $mode = chained ]; then
      name=$b.chained
      flag=--chained
    fi
    if ! "$driver" $flag "$f" > "$out/$name.out"; then
      echo "$name: the driver failed" > "$out/$name.diff"
    fi
    diff -u "$good/$b.out" "$out/$name.out" >> "$out/$name.diff" ||
      true
    [ -s "$out/$name.diff" ] || rm -f "$out/$name.diff"
  done
'
# 3. The differences, if any.
diffs=$(ls "$out"/*.diff 2> /dev/null)
if [ -n "$diffs" ]; then
  cat $diffs
  echo "scenario transcripts differ; see the diffs above"
  exit 1
fi
echo "scenarios: $# scripts match, versioned and chained"
