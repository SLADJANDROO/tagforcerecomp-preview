#!/bin/sh
# Tests for the Destiny Draw mod (no game needed; any g++ with C++17). Usage: sh tools/test/destiny/run_tests.sh
set -e
cd "$(dirname "$0")/../../.."
OUT="${TMPDIR:-/tmp}/destiny_tests"; mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Wno-misleading-indentation -fsanitize=address,undefined -g -D'__declspec(x)=' -Iplugins/sdk tools/test/destiny/test_destiny.cpp -o "$OUT/test_destiny"
"$OUT/test_destiny"
