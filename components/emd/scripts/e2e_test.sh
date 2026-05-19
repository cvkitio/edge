#!/usr/bin/env bash
# scripts/e2e_test.sh
#
# Convenience wrapper that builds emd, generates fixtures if needed, and
# runs the Python E2E test orchestrator for the "motion" scenario.
#
# Usage:
#   bash scripts/e2e_test.sh
#
# Environment overrides:
#   EMD_SCENARIO    — one of: static, motion, gradual (default: motion)
#   EMD_CONTAINER   — one of: mpegts, fmp4 (default: mpegts)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."

SCENARIO="${EMD_SCENARIO:-motion}"
CONTAINER="${EMD_CONTAINER:-mpegts}"

echo "=== EMD End-to-End Test ==="
echo "    Scenario:  $SCENARIO"
echo "    Container: $CONTAINER"
echo ""

# ---------------------------------------------------------------------------
# [1/4] Build
# ---------------------------------------------------------------------------
echo "[1/4] Building emd (Release)..."
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
    > /dev/null 2>&1 || \
    cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build "$ROOT/build" --parallel > /dev/null 2>&1 || \
    cmake --build "$ROOT/build" --parallel
echo "      Build OK"

# ---------------------------------------------------------------------------
# [2/4] Generate fixtures
# ---------------------------------------------------------------------------
echo "[2/4] Generating fixtures (if needed)..."
bash "$SCRIPT_DIR/generate_fixtures.sh"
echo "      Fixtures OK"

# ---------------------------------------------------------------------------
# [3/4] Run E2E test
# ---------------------------------------------------------------------------
echo "[3/4] Running E2E test (scenario=$SCENARIO, container=$CONTAINER)..."
python3 "$ROOT/tests/integration/e2e_test.py" \
    --binary "$ROOT/build/emd" \
    --fixtures-dir "$ROOT/tests/fixtures/streams" \
    --scenario "$SCENARIO" \
    --container "$CONTAINER"

# ---------------------------------------------------------------------------
# [4/4] Done
# ---------------------------------------------------------------------------
echo ""
echo "[4/4] Done."
