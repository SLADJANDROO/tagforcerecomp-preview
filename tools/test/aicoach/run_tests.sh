#!/bin/sh
# Unit tests for plugins/aicoach_core (no game needed; any g++ with C++17). Usage: sh tools/test/aicoach/run_tests.sh
set -e
cd "$(dirname "$0")/../../.."
OUT="${TMPDIR:-/tmp}/aicoach_tests"; mkdir -p "$OUT"
FLAGS="-std=c++17 -O1 -Wall -Wextra -Wno-misleading-indentation -fsanitize=address,undefined -g -DSRCDIR=\"$(pwd)\" -Iplugins/aicoach_core"
CORE="plugins/aicoach_core/json.cpp plugins/aicoach_core/model.cpp plugins/aicoach_core/store.cpp"
g++ $FLAGS $CORE tools/test/aicoach/test_core.cpp -o "$OUT/test_core"
g++ $FLAGS $CORE plugins/aicoach_core/engine.cpp tools/test/aicoach/test_engine.cpp -o "$OUT/test_engine"
g++ $FLAGS $CORE plugins/aicoach_core/engine.cpp plugins/aicoach_core/game_view.cpp tools/test/aicoach/test_view.cpp -o "$OUT/test_view"
g++ $FLAGS -D'__declspec(x)=' -Iplugins/sdk $CORE plugins/aicoach_core/engine.cpp plugins/aicoach_core/game_view.cpp tools/test/aicoach/test_plugin.cpp -o "$OUT/test_plugin"
echo "== data model, store =="; "$OUT/test_core"
echo "== engine =="; "$OUT/test_engine"
echo "== game view =="; "$OUT/test_view"
echo "== plugin glue (fake TfModApi) =="; "$OUT/test_plugin"
