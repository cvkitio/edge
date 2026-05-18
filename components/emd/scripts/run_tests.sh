#!/usr/bin/env bash
# scripts/run_tests.sh
#
# Builds emd in Debug mode, runs the full unit test suite under ctest,
# then runs the end-to-end test harness.
#
# Usage:
#   bash scripts/run_tests.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."

echo "=== EMD Test Suite ==="
echo ""

# ---------------------------------------------------------------------------
# [1/3] Build (Debug, with tests)
# ---------------------------------------------------------------------------
echo "[1/3] Building emd (Debug + tests)..."
cmake -S "$ROOT" -B "$ROOT/build" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTS=ON \
    > /dev/null 2>&1 || \
    cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build "$ROOT/build" --parallel > /dev/null 2>&1 || \
    cmake --build "$ROOT/build" --parallel
echo "      Build OK"

# ---------------------------------------------------------------------------
# [2/3] Unit tests
# ---------------------------------------------------------------------------
echo "[2/3] Running unit tests..."
(
    cd "$ROOT/build"
    ctest --output-on-failure
)
echo "      Unit tests OK"

# ---------------------------------------------------------------------------
# [3/3] E2E tests
# ---------------------------------------------------------------------------
echo "[3/3] Running E2E tests..."
bash "$SCRIPT_DIR/e2e_test.sh"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "All tests passed."
