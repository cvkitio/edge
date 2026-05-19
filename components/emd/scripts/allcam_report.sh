#!/usr/bin/env bash
# scripts/allcam_report.sh — Multi-camera false-positive audit
#
# Runs all 14 cameras from k8s/agent.toml simultaneously for a configurable
# duration, then validates every recorded clip with the video-understand
# Gemini service to classify true-positive vs false-positive motion events.
#
# Outputs a per-camera report table:
#   Camera | Events | Clips | Gemini TP | Gemini FP | TP% | Avg z | Avg bytes
#
# Prerequisites:
#   - GOOGLE_API_KEY  must be set (for video-understand Gemini backend)
#   - VU_API_KEY      must be set (for video-understand API auth)
#   - emd-agent binary built: components/emd/build/emd-agent
#   - video-understand binary built: ../../video-understand/bin/video-understand
#
# Usage:
#   bash components/emd/scripts/allcam_report.sh [--duration 180] [--no-build]
#
# Environment overrides:
#   RUN_DURATION  seconds to run cameras (default: 180)
#   VU_SERVER     video-understand server URL (default: http://127.0.0.1:18080)
#   VU_FEED       feed ID to use for validation (default: test)
#   AGENT_API_PORT emd-agent REST API port (default: 8082)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPONENT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$COMPONENT_DIR/../.." && pwd)"
VU_DIR="$(cd "$REPO_ROOT/../video-understand" && pwd)"

RUN_DURATION="${RUN_DURATION:-180}"
VU_SERVER="${VU_SERVER:-http://127.0.0.1:18080}"
VU_FEED="${VU_FEED:-test}"
AGENT_API_PORT="${AGENT_API_PORT:-8082}"
VU_PORT=18080

CONFIG="$COMPONENT_DIR/k8s/agent.toml"
WORK_DIR="/tmp/emd_allcam_report"
CLIP_ROOT="$WORK_DIR/clips"
EVENTLOG_ROOT="$WORK_DIR/eventlog"
INFLIGHT_ROOT="$WORK_DIR/inflight"
AGENT_LOG="$WORK_DIR/agent.log"
VU_LOG="$WORK_DIR/vu.log"
REPORT="$WORK_DIR/report.json"

BUILD=true
for arg in "$@"; do
    case "$arg" in
        --no-build)   BUILD=false ;;
        --duration)   shift; RUN_DURATION="${1:-$RUN_DURATION}" ;;
    esac
done

AGENT_PID=""
VU_PID=""

cleanup() {
    echo ""
    echo "--- CLEANUP ---"
    [[ -n "$AGENT_PID" ]] && kill "$AGENT_PID" 2>/dev/null || true
    [[ -n "$VU_PID" ]]    && kill "$VU_PID"    2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

section() { echo ""; echo "=== $* ==="; }

# ---------------------------------------------------------------------------
# Prerequisites check
# ---------------------------------------------------------------------------
section "Prerequisites"

if [[ -z "${GOOGLE_API_KEY:-}" ]]; then
    echo "ERROR: GOOGLE_API_KEY must be set"
    exit 1
fi
if [[ -z "${VU_API_KEY:-}" ]]; then
    echo "ERROR: VU_API_KEY must be set"
    exit 1
fi
echo "  GOOGLE_API_KEY: set"
echo "  VU_API_KEY: set"

rm -rf "$WORK_DIR"
mkdir -p "$CLIP_ROOT" "$EVENTLOG_ROOT" "$INFLIGHT_ROOT"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
section "Build"

if $BUILD; then
    echo "  Building emd-agent..."
    (cd "$REPO_ROOT" && go build -o "$COMPONENT_DIR/build/emd-agent" ./components/emd/cmd/emd-agent/)
    echo "  emd-agent OK"
    echo "  Building video-understand..."
    (cd "$VU_DIR" && go build -o bin/video-understand ./cmd/server/ && go build -o bin/check-feed ./cmd/check-feed/)
    echo "  video-understand OK"
fi

AGENT_BIN="$COMPONENT_DIR/build/emd-agent"
VU_BIN="$VU_DIR/bin/video-understand"
CHECK_BIN="$VU_DIR/bin/check-feed"

# ---------------------------------------------------------------------------
# Patch config: all cameras, override paths + debug logging
# ---------------------------------------------------------------------------
PATCHED_CONFIG="$WORK_DIR/agent.toml"

python3 - "$CONFIG" "$CLIP_ROOT" "$INFLIGHT_ROOT" "$EVENTLOG_ROOT" <<'PYEOF'
import sys, re

config_path, clip_root, inflight_root, event_log_root = sys.argv[1:]

with open(config_path) as f:
    text = f.read()

def set_toml_key(text, key, value):
    pattern = rf'^(\s*{re.escape(key)}\s*=\s*).*$'
    replacement = rf'\g<1>"{value}"'
    new, n = re.subn(pattern, replacement, text, flags=re.MULTILINE)
    if n == 0:
        new = re.sub(r'(\[runtime\][^\[]*)', rf'\1{key} = "{value}"\n', new, flags=re.DOTALL)
    return new

for key, val in [
    ("clip_root",      clip_root),
    ("inflight_root",  inflight_root),
    ("event_log_root", event_log_root),
    ("log_level",      "info"),   # info for multi-cam run (debug is too noisy)
]:
    text = set_toml_key(text, key, val)

text = re.sub(r'^(\s*fsync_policy\s*=\s*).*$', r'\1"never"', text, flags=re.MULTILINE)

with open("/tmp/emd_allcam_report/agent.toml", "w") as f:
    f.write(text)
print("  Config patched (all 14 cameras, info logging)")
PYEOF

# ---------------------------------------------------------------------------
# Start video-understand server
# ---------------------------------------------------------------------------
section "video-understand server"

# Write patched VU server config on a non-conflicting port
cat > "$WORK_DIR/vu-server.yaml" <<YAML
listen_addr: ":${VU_PORT}"
feeds_file: "${VU_DIR}/configs/feeds.yaml"
max_upload_mb: 200
temp_dir: "${WORK_DIR}/vu-tmp"
YAML
mkdir -p "$WORK_DIR/vu-tmp"

GOOGLE_API_KEY="$GOOGLE_API_KEY" VU_API_KEY="$VU_API_KEY" VU_LISTEN_ADDR=":${VU_PORT}" \
"$VU_BIN" \
    -config "$WORK_DIR/vu-server.yaml" \
    -feeds  "$VU_DIR/configs/feeds.yaml" \
    2>"$VU_LOG" &
VU_PID=$!

for i in $(seq 1 10); do
    if curl -sf "http://127.0.0.1:${VU_PORT}/v1/health" >/dev/null 2>&1; then
        echo "  video-understand ready on :${VU_PORT}"
        break
    fi
    if [[ $i -eq 10 ]]; then
        echo "  video-understand did not start. Log:"
        tail -20 "$VU_LOG" || true
        exit 1
    fi
    sleep 1
done

# ---------------------------------------------------------------------------
# Start emd-agent (all 14 cameras)
# ---------------------------------------------------------------------------
section "emd-agent (all 14 cameras, ${RUN_DURATION}s)"

"$AGENT_BIN" \
    -config "$PATCHED_CONFIG" \
    -api    ":${AGENT_API_PORT}" \
    -metrics ":0" \
    -pprof   ":0" \
    >"$AGENT_LOG" 2>&1 &
AGENT_PID=$!

echo "  Waiting for API..."
for i in $(seq 1 15); do
    if curl -sf "http://127.0.0.1:${AGENT_API_PORT}/health" >/dev/null 2>&1; then
        echo "  emd-agent ready (pid $AGENT_PID)"
        break
    fi
    if ! kill -0 "$AGENT_PID" 2>/dev/null; then
        echo "  emd-agent exited early:"
        tail -30 "$AGENT_LOG" || true
        exit 1
    fi
    if [[ $i -eq 15 ]]; then
        echo "  emd-agent API not ready in 15s"
        tail -20 "$AGENT_LOG"
        exit 1
    fi
    sleep 1
done

echo "  Running for ${RUN_DURATION}s..."
echo "  Progress: (a dot every 10s)"
printf "  "
for i in $(seq 1 $((RUN_DURATION / 10))); do
    sleep 10
    printf "."
    # Print running clip count every 30s
    if [[ $((i % 3)) -eq 0 ]]; then
        COUNT=$(find "$CLIP_ROOT" -name "*.mpegts" 2>/dev/null | wc -l | tr -d ' ')
        printf "[%s clips]" "$COUNT"
    fi
done
echo ""

echo "  Stopping emd-agent..."
kill "$AGENT_PID" 2>/dev/null || true
wait "$AGENT_PID" 2>/dev/null || true
AGENT_PID=""
echo "  Stopped."


# ---------------------------------------------------------------------------
# Collect results + Gemini validation + report — all in Python (bash3-safe)
# ---------------------------------------------------------------------------
section "Collecting results + Gemini validation"

python3 - "$EVENTLOG_ROOT" "$CLIP_ROOT" "$WORK_DIR" "$VU_SERVER" "$VU_FEED" "$VU_API_KEY" << 'PYEOF'
import json, os, sys, urllib.request, urllib.parse, urllib.error

eventlog_root, clip_root, work_dir, vu_server, vu_feed, vu_api_key = sys.argv[1:]

# ---- collect eventlog data ----
cameras = sorted([
    d for d in os.listdir(eventlog_root)
    if os.path.isdir(os.path.join(eventlog_root, d))
])

cam_events = {}   # cam -> list of event dicts
for cam in cameras:
    events = []
    cam_dir = os.path.join(eventlog_root, cam)
    for fn in sorted(os.listdir(cam_dir)):
        if not fn.endswith(".jsonl"):
            continue
        with open(os.path.join(cam_dir, fn)) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        events.append(json.loads(line))
                    except Exception:
                        pass
    cam_events[cam] = events
    print(f"  {cam}: {len(events)} events")

# ---- collect clip paths ----
cam_clips = {}   # cam -> list of file paths
total_clips = 0
for cam in cameras:
    d = os.path.join(clip_root, cam)
    if os.path.isdir(d):
        clips = sorted([os.path.join(d, f) for f in os.listdir(d) if f.endswith(".mpegts")])
    else:
        clips = []
    cam_clips[cam] = clips
    total_clips += len(clips)

total_events = sum(len(v) for v in cam_events.values())
print(f"\n  Total: {total_events} events, {total_clips} clips across {len(cameras)} cameras")

# ---- Gemini validation via video-understand ----
print(f"\n=== Gemini validation ({total_clips} clips) ===")

cam_tp = {c: 0 for c in cameras}
cam_fp = {c: 0 for c in cameras}
cam_err = {c: 0 for c in cameras}
cam_desc = {c: [] for c in cameras}   # descriptions for TP clips

validated = 0
for cam in cameras:
    for clip_path in cam_clips[cam]:
        validated += 1
        clip_name = os.path.basename(clip_path)
        print(f"  [{validated}/{total_clips}] {cam}/{clip_name} ... ", end="", flush=True)

        # Build multipart form using urllib (no third-party deps)
        boundary = "---EMDBoundary7MA4YWxkTrZu0gW"
        with open(clip_path, "rb") as fh:
            file_data = fh.read()

        body = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="feed_id"\r\n\r\n'
            f"{vu_feed}\r\n"
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="video"; filename="{clip_name}"\r\n'
            f"Content-Type: video/mp2t\r\n\r\n"
        ).encode() + file_data + f"\r\n--{boundary}--\r\n".encode()

        req = urllib.request.Request(
            f"{vu_server}/v1/analyze",
            data=body,
            headers={
                "X-API-Key": vu_api_key,
                "Content-Type": f"multipart/form-data; boundary={boundary}",
            },
        )

        try:
            with urllib.request.urlopen(req, timeout=180) as resp:
                result = json.loads(resp.read())
        except urllib.error.HTTPError as e:
            body_text = e.read().decode(errors="replace")[:200]
            print(f"ERROR HTTP {e.code}: {body_text}")
            cam_err[cam] += 1
            continue
        except Exception as exc:
            print(f"ERROR: {exc}")
            cam_err[cam] += 1
            continue

        if "error" in result:
            print(f"ERROR: {result['error']}")
            cam_err[cam] += 1
            continue

        detections = result.get("detections", [])
        found = any(d["detected"] for d in detections)
        desc = next((d.get("description", "") for d in detections if d["detected"]), "")

        if found:
            cam_tp[cam] += 1
            cam_desc[cam].append(desc[:80] if desc else "detected")
            print(f"TP  [{desc[:60]}]")
        else:
            cam_fp[cam] += 1
            print("FP")

# ---- write JSON results ----
report = {}
for cam in cameras:
    events = cam_events[cam]
    z_vals = [e["z_score"] for e in events if e.get("z_score", 0) > 0]
    b_vals = [e["bytes"]   for e in events if e.get("bytes", 0) > 0]
    s_vals = [e["bpf_slow"] for e in events if e.get("bpf_slow", 0) > 0]
    report[cam] = {
        "events":    len(events),
        "clips":     len(cam_clips[cam]),
        "tp":        cam_tp[cam],
        "fp":        cam_fp[cam],
        "err":       cam_err[cam],
        "avg_z":     sum(z_vals) / len(z_vals) if z_vals else 0,
        "avg_bytes": sum(b_vals) / len(b_vals) if b_vals else 0,
        "avg_bpf_slow": sum(s_vals) / len(s_vals) if s_vals else 0,
        "tp_descriptions": cam_desc[cam],
    }

with open(os.path.join(work_dir, "report.json"), "w") as f:
    json.dump(report, f, indent=2)

# ---- print table ----
print()
print("=== Per-Camera Report ===")
print()
hdr = f"{'Camera':<14} {'Events':>6} {'Clips':>5}  {'TP':>4} {'FP':>4} {'Err':>4} {'TP%':>6}  {'Avg z':>7} {'Avg bytes':>10} {'Avg bpf_slow':>12}"
print(hdr)
print("-" * len(hdr))

total_ev = total_clips_r = total_tp = total_fp = total_err = 0

for cam in cameras:
    r = report[cam]
    n_ev    = r["events"]
    n_clips = r["clips"]
    n_tp    = r["tp"]
    n_fp    = r["fp"]
    n_err   = r["err"]
    avg_z   = r["avg_z"]
    avg_b   = r["avg_bytes"]
    avg_s   = r["avg_bpf_slow"]

    validated = n_tp + n_fp
    tp_pct = (n_tp / validated * 100) if validated > 0 else 0
    flag = " !" if (validated > 0 and tp_pct < 50) else ("  " if validated > 0 else " -")

    print(f"{cam:<14} {n_ev:>6} {n_clips:>5}  {n_tp:>4} {n_fp:>4} {n_err:>4} {tp_pct:>5.0f}%{flag}  {avg_z:>7.2f} {avg_b:>10,.0f} {avg_s:>12,.1f}")

    total_ev       += n_ev
    total_clips_r  += n_clips
    total_tp       += n_tp
    total_fp       += n_fp
    total_err      += n_err

print("-" * len(hdr))
total_val = total_tp + total_fp
total_tp_pct = (total_tp / total_val * 100) if total_val > 0 else 0
print(f"{'TOTAL':<14} {total_ev:>6} {total_clips_r:>5}  {total_tp:>4} {total_fp:>4} {total_err:>4} {total_tp_pct:>5.0f}%")
print()
print("  TP  = Gemini confirmed person/vehicle/animal present")
print("  FP  = Gemini found nothing (false positive)")
print("  !   = camera with < 50% true-positive rate")
print("  -   = no clips to validate in this window")
PYEOF

echo ""
echo "  Run duration:  ${RUN_DURATION}s"
echo "  Full agent log: $AGENT_LOG"
echo "  Clip root:      $CLIP_ROOT"
echo "  Event log:      $EVENTLOG_ROOT"
echo "  Report JSON:    $WORK_DIR/report.json"
