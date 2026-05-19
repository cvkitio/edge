#!/usr/bin/env bash
# scripts/local_test.sh — local single-camera test against the Axis P4708
#
# Builds emd-agent, runs it against the real camera for a bounded duration,
# then validates:
#   - At least one motion event was written to the JSONL eventlog
#   - Each event record has the required inspector signal fields
#   - At least one .ts clip file was produced
#   - The REST endpoint /api/cameras/axis_p4708/events returns records
#
# Usage:
#   bash components/emd/scripts/local_test.sh [--no-build] [--timeout 120]
#
# Environment overrides:
#   CONFIG          — path to agent TOML (default: configs/axis_p4708.toml)
#   AGENT_API_PORT  — port for emd-agent REST API (default: 8081)
#   AGENT_LOG       — path to save agent log (default: /tmp/emd_local_test/agent.log)
#   TEST_TIMEOUT    — seconds to wait for first motion event (default: 120)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPONENT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$COMPONENT_DIR/../.." && pwd)"

CONFIG="${CONFIG:-$COMPONENT_DIR/k8s/agent.toml}"
AGENT_API_PORT="${AGENT_API_PORT:-8081}"
TEST_TIMEOUT="${TEST_TIMEOUT:-120}"
AGENT_LOG="${AGENT_LOG:-/tmp/emd_local_test/agent.log}"

CLIP_ROOT="/tmp/emd_local_test/clips"
EVENTLOG_ROOT="/tmp/emd_local_test/eventlog"
INFLIGHT_ROOT="/tmp/emd_local_test/inflight"

# Camera to test — single camera from the k8s config
CAM_NAME="${CAM_NAME:-axis_81_1}"

BUILD=true
for arg in "$@"; do
    case "$arg" in
        --no-build) BUILD=false ;;
        --timeout)  shift; TEST_TIMEOUT="${1:-$TEST_TIMEOUT}" ;;
    esac
done

PASS=0
FAIL=0
AGENT_PID=""

cleanup() {
    echo ""
    echo "--- CLEANUP ---"
    [[ -n "$AGENT_PID" ]] && kill "$AGENT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

ok()   { echo "  PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

section() { echo ""; echo "=== $* ==="; }

# ---------------------------------------------------------------------------
# [0] Preflight
# ---------------------------------------------------------------------------
section "Preflight"

if [[ ! -f "$CONFIG" ]]; then
    echo "ERROR: config not found: $CONFIG"
    exit 1
fi

rm -rf /tmp/emd_local_test
mkdir -p "$CLIP_ROOT" "$EVENTLOG_ROOT" "$INFLIGHT_ROOT" "$(dirname "$AGENT_LOG")"

echo "  Config:     $CONFIG"
echo "  Camera:     $CAM_NAME"
echo "  Clip root:  $CLIP_ROOT"
echo "  Event log:  $EVENTLOG_ROOT"
echo "  Agent log:  $AGENT_LOG"
echo "  Timeout:    ${TEST_TIMEOUT}s"

# ---------------------------------------------------------------------------
# [1] Build emd-agent
# ---------------------------------------------------------------------------
section "Build"

if $BUILD; then
    echo "Building emd-agent..."
    (
        cd "$REPO_ROOT"
        go build -o "$COMPONENT_DIR/build/emd-agent" \
            ./components/emd/cmd/emd-agent/
    )
    ok "emd-agent built: $COMPONENT_DIR/build/emd-agent"
else
    if [[ ! -x "$COMPONENT_DIR/build/emd-agent" ]]; then
        echo "ERROR: --no-build specified but binary not found at $COMPONENT_DIR/build/emd-agent"
        exit 1
    fi
    ok "emd-agent binary exists (skipped build)"
fi

AGENT_BIN="$COMPONENT_DIR/build/emd-agent"

# ---------------------------------------------------------------------------
# [2] Write a patched config that overrides output paths to /tmp
#     so the real camera config works without touching its clip_root.
# ---------------------------------------------------------------------------
PATCHED_CONFIG="/tmp/emd_local_test/agent.toml"
python3 - "$CONFIG" "$CLIP_ROOT" "$INFLIGHT_ROOT" "$EVENTLOG_ROOT" "$CAM_NAME" <<'PYEOF'
import sys, re

config_path, clip_root, inflight_root, event_log_root, cam_name = sys.argv[1:]

with open(config_path) as f:
    text = f.read()

def set_toml_key(text, key, value):
    pattern = rf'^(\s*{re.escape(key)}\s*=\s*).*$'
    replacement = rf'\g<1>"{value}"'
    new, n = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if n == 0:
        new = re.sub(r'(\[runtime\][^\[]*)', rf'\1{key} = "{value}"\n', new, flags=re.DOTALL)
    return new

# Override output paths and log level
for key, val in [
    ("clip_root",      clip_root),
    ("inflight_root",  inflight_root),
    ("event_log_root", event_log_root),
    ("log_level",      "debug"),
]:
    text = set_toml_key(text, key, val)

# Never fsync for speed
text = re.sub(r'^(\s*fsync_policy\s*=\s*).*$', r'\1"never"', text, flags=re.MULTILINE)

# Keep only the target camera — strip all other [cameras.*] blocks
lines = text.splitlines(keepends=True)
out = []
in_cam_block = False
keep_block = False
for line in lines:
    m = re.match(r'^\[cameras\.(\w+)\]', line)
    if m:
        in_cam_block = True
        keep_block = (m.group(1) == cam_name)
    elif re.match(r'^\[', line):
        in_cam_block = False
        keep_block = False

    if in_cam_block:
        if keep_block:
            out.append(line)
    else:
        out.append(line)

with open("/tmp/emd_local_test/agent.toml", "w") as f:
    f.writelines(out)

print(f"  Patched config: kept only camera [{cam_name}]")
PYEOF

# Show the runtime section of the patched config
echo ""
echo "  Runtime section of patched config:"
python3 -c "
import re, sys
with open('/tmp/emd_local_test/agent.toml') as f: t = f.read()
m = re.search(r'\[runtime\][^\[]*', t, re.DOTALL)
print(m.group(0) if m else '(not found)')
" | sed 's/^/    /'

# ---------------------------------------------------------------------------
# [3] Start emd-agent
# ---------------------------------------------------------------------------
section "emd-agent"

echo "Starting emd-agent (logging to $AGENT_LOG)..."

"$AGENT_BIN" \
    -config "$PATCHED_CONFIG" \
    -api    ":${AGENT_API_PORT}" \
    -metrics ":0" \
    -pprof   ":0" \
    2>&1 | tee "$AGENT_LOG" | sed 's/^/  [agent] /' &
AGENT_PID=$!

echo "Waiting for agent API to be ready..."
for i in $(seq 1 15); do
    if curl -sf "http://127.0.0.1:${AGENT_API_PORT}/health" >/dev/null 2>&1; then
        ok "agent API ready"
        break
    fi
    if ! kill -0 "$AGENT_PID" 2>/dev/null; then
        fail "agent exited before API became ready"
        echo ""
        echo "--- AGENT LOG ---"
        tail -40 "$AGENT_LOG" || true
        exit 1
    fi
    if [[ $i -eq 15 ]]; then
        fail "agent API did not become ready in 15s"
        echo ""
        echo "--- AGENT LOG (last 40 lines) ---"
        tail -40 "$AGENT_LOG" || true
        exit 1
    fi
    sleep 1
done

# ---------------------------------------------------------------------------
# [4] Wait for first motion event
# ---------------------------------------------------------------------------
section "Waiting for motion event (up to ${TEST_TIMEOUT}s)"

EVENT_FILE=""
DETECTED=false
ELAPSED=0

while [[ $ELAPSED -lt $TEST_TIMEOUT ]]; do
    EVENT_FILE=$(ls "$EVENTLOG_ROOT/${CAM_NAME}/"*.jsonl 2>/dev/null | head -1 || true)
    if [[ -n "$EVENT_FILE" ]] && [[ -s "$EVENT_FILE" ]]; then
        if grep -q '"type":"motion"' "$EVENT_FILE" 2>/dev/null; then
            DETECTED=true
            echo "  First motion event detected at ${ELAPSED}s"
            break
        fi
    fi
    if ! kill -0 "$AGENT_PID" 2>/dev/null; then
        fail "agent exited unexpectedly while waiting for events"
        break
    fi
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done

# ---------------------------------------------------------------------------
# [5] Validate results
# ---------------------------------------------------------------------------
section "Validation"

# 5a: Motion event detected
if $DETECTED; then
    ok "motion event detected in JSONL eventlog"
else
    fail "no motion event after ${TEST_TIMEOUT}s"
    echo ""
    echo "--- AGENT LOG (last 60 lines) ---"
    tail -60 "$AGENT_LOG" || true
fi

# 5b: Inspector signal fields present in eventlog records
REQUIRED_FIELDS="z_score intra_ratio bytes bpf_slow bpf_ewma bpf_var since_kf fsm_before fsm_after"
if [[ -n "$EVENT_FILE" ]] && [[ -s "$EVENT_FILE" ]]; then
    MISSING_FIELDS=""
    for field in $REQUIRED_FIELDS; do
        if ! grep -q "\"$field\"" "$EVENT_FILE" 2>/dev/null; then
            MISSING_FIELDS="$MISSING_FIELDS $field"
        fi
    done

    if [[ -z "$MISSING_FIELDS" ]]; then
        ok "all inspector signal fields present in eventlog records"
    else
        fail "missing fields in eventlog:$MISSING_FIELDS"
    fi

    echo ""
    echo "  Sample eventlog record:"
    grep '"type":"motion"' "$EVENT_FILE" | head -1 | python3 -m json.tool 2>/dev/null | sed 's/^/    /' || true
else
    fail "eventlog file not found or empty"
fi

# 5c: At least one clip written
CLIP_COUNT=$(find "$CLIP_ROOT" -name "*.mpegts" 2>/dev/null | wc -l | tr -d ' ')
if [[ "$CLIP_COUNT" -gt 0 ]]; then
    ok "clip files written: $CLIP_COUNT .mpegts file(s)"
    find "$CLIP_ROOT" -name "*.mpegts" | sed 's/^/    /'
else
    fail "no .mpegts clip files found in $CLIP_ROOT"
fi

# 5d: REST endpoint returns records
echo ""
echo "  Querying GET /api/cameras/${CAM_NAME}/events?limit=5 ..."
REST_RESPONSE=$(curl -sf \
    "http://127.0.0.1:${AGENT_API_PORT}/api/cameras/${CAM_NAME}/events?limit=5" \
    2>/dev/null || true)

if [[ -n "$REST_RESPONSE" ]]; then
    RECORD_COUNT=$(echo "$REST_RESPONSE" | grep -c '"event_id"' || true)
    ok "REST endpoint returned $RECORD_COUNT event record(s)"
    echo "$REST_RESPONSE" | head -1 | python3 -m json.tool 2>/dev/null | sed 's/^/    /' || \
        echo "$REST_RESPONSE" | head -200 | sed 's/^/    /'
else
    fail "REST endpoint returned empty response or error"
fi

# 5e: No unexpected ERROR lines in agent log
ERROR_COUNT=$(grep -cE '"level":"error"|level=error' "$AGENT_LOG" 2>/dev/null || true)
if [[ "$ERROR_COUNT" -gt 0 ]]; then
    fail "agent log contains $ERROR_COUNT error-level line(s)"
    grep -E '"level":"error"|level=error' "$AGENT_LOG" | head -10 | sed 's/^/    /'
else
    ok "no error-level log lines in agent output"
fi

# ---------------------------------------------------------------------------
# [6] Summary
# ---------------------------------------------------------------------------
section "Summary"

echo "  PASSED: $PASS"
echo "  FAILED: $FAIL"
echo ""
echo "  Full agent log: $AGENT_LOG"
echo "  Clip root:      $CLIP_ROOT"
echo "  Event log:      $EVENTLOG_ROOT"
echo ""

if [[ $FAIL -eq 0 ]]; then
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
