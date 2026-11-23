#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# test.sh -- build every part once, on the debug build, then run their
# tests side by side, each to a log of its own: ty, sm, sq, aa and dg.
# "make test" at the top runs it.
#
#   ./test.sh             every part
#   ./test.sh sm dg       those parts
#
# The tests of a part print nothing here; each part's log is
# build/test-logs/PART.log, and a line says when a part ends, with its
# time. The run fails if a part fails.
# 1. Build each part, and rel, one after another: a part's tests build
#    what is stale, and two makes of one library at once would race.
# 2. Start each part's tests in the background.
# 3. As each ends, say whether it passed, and how long it took.
# 4. Fail if one failed.
cd "$(dirname "$0")" || exit 1
export DEBUG=1
parts=("$@")
[ ${#parts[@]} -eq 0 ] && parts=(ty sm sq aa dg)
logs=build/test-logs
mkdir -p "$logs"
start=$SECONDS

# 1. Everything built first, so the tests below build nothing.
for p in ty sm sq aa; do
  make -C "$p" -j4 all > "$logs/build.log" 2>&1 || {
    echo "build of $p failed; see $logs/build.log"
    exit 1
  }
done
make -C sm/rel -j4 > "$logs/build.log" 2>&1 &&
  make -C dg -j4 all > "$logs/build.log" 2>&1 || {
  echo "build of rel or dg failed; see $logs/build.log"
  exit 1
}
echo "built in $((SECONDS - start)) s; testing: ${parts[*]}"

# 2. Each part's tests, at once.
declare -A pid_part
for p in "${parts[@]}"; do
  make -C "$p" -j4 test > "$logs/$p.log" 2>&1 &
  pid_part[$!]=$p
done

# 3. Each part as it ends.
failed=0
while [ ${#pid_part[@]} -gt 0 ]; do
  wait -n -p done_pid "${!pid_part[@]}"
  rc=$?
  p=${pid_part[$done_pid]}
  unset "pid_part[$done_pid]"
  if [ $rc -eq 0 ]; then
    echo "$p: PASSED ($((SECONDS - start)) s)"
  else
    echo "$p: FAILED ($((SECONDS - start)) s; see $logs/$p.log)"
    failed=1
  fi
done

# 4. The run's result.
[ $failed -eq 0 ] && echo "test: PASSED (${parts[*]})"
exit $failed
