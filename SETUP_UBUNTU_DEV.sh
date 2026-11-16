#!/bin/bash
# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# SETUP_UBUNTU_DEV.sh -- make a minimal Ubuntu machine ready to build,
# test and benchmark digres: README.md's "Prerequisites", as a script.
#
#   ./SETUP_UBUNTU_DEV.sh
#
# Each step does nothing once its result is there, so the script can
# be run again after a failure. What it installs:
#
#   - the apt packages
#   - Janet, commit 86ca958e, into /usr/local, for sm/rel
#   - ext/pg18.6/install/bin put first on PATH in ~/.profile, for psql
#     and pgbench
#
# PostgreSQL 18.6 and RocksDB are not installed here: make downloads
# and builds them in ext/ the first time (ext/Makefile). SRC, where
# Janet is cloned, is ~/p by default.
set -euo pipefail

top=$(cd "$(dirname "$0")" && pwd)
src=${SRC:-$HOME/p}

# Root in a container has no sudo, and needs none.
sudo=sudo
[ "$(id -u)" -eq 0 ] && sudo=

step() { echo "== $*"; }

# 1. The packages: the compilers and generators, wget for ext/'s
#    downloads, the libraries sm and digres link (OpenSSL among them,
#    for TLS, and RocksDB's compression libraries), PostgreSQL's build
#    dependencies, and openssl for the TLS tests' certificates. No
#    PostgreSQL package: ext/'s build has the server, psql, pgbench
#    and libpq.
step "apt packages"
$sudo apt-get update
DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y \
  build-essential cmake git wget ca-certificates flex bison python3 \
  clang-format libicu-dev \
  libsnappy-dev liblz4-dev libzstd-dev zlib1g-dev libbz2-dev \
  libreadline-dev libssl-dev openssl pkg-config bc linux-tools-generic

mkdir -p "$src"

# 2. Janet, which sm/rel embeds, at the commit its tests run on.
if [ ! -f /usr/local/include/janet.h ]; then
  step "janet"
  [ -d "$src/janet" ] ||
    git clone https://github.com/janet-lang/janet.git "$src/janet"
  git -C "$src/janet" checkout 86ca958e
  make -C "$src/janet" -j4
  $sudo make -C "$src/janet" install
  # Refresh the loader's cache, so libjanet.so is found at run time.
  $sudo ldconfig
fi

# 3. ext/'s PostgreSQL 18.6's psql and pgbench on PATH.
line="export PATH=$top/ext/pg18.6/install/bin:\$PATH"
if ! grep -qxF "$line" "$HOME/.profile" 2>/dev/null; then
  step "PATH in ~/.profile"
  echo "$line" >> "$HOME/.profile"
fi

step "done"
echo "Log in again, or run: . ~/.profile"
echo "Then: DEBUG=1 make -j4 && make -j4 test"
