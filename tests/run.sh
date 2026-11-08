#!/bin/sh
# Native unit tests for the pure-C parts (netsid_proto, minifmt). Needs cc/clang.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
${HOSTCC:-cc} -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
  -o build/test_proto tests/test_proto.c src/netsid_proto.c src/minifmt.c
build/test_proto
