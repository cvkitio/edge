#!/usr/bin/env bash
# scripts/generate_fixtures.sh
#
# Generates synthetic H.264 test fixtures using the ffmpeg CLI.
# ffmpeg CLI use is intentional and confined to test tooling only — it is
# never exec'd by the production emd binary (see spec §5.3 and CLAUDE.md).
#
# Output directory: tests/fixtures/streams/
# Generated files:
#   static_1080p30.h264   — 30 s of static noise, no motion, IDR every 10 s
#   motion_1080p30.h264   — 60 s with three motion bursts at t=10,30,50 s
#   gradual_1080p30.h264  — 90 s slow brightness increase (simulated dawn)
#   emd_e2e.toml          — sample config for the E2E test harness

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
FIXTURES_DIR="$ROOT/tests/fixtures/streams"

# ---------------------------------------------------------------------------
# 1. Check ffmpeg
# ---------------------------------------------------------------------------
if ! command -v ffmpeg &>/dev/null; then
    echo "ERROR: ffmpeg not found in PATH." >&2
    echo "       Install ffmpeg to generate test fixtures (test-only use)." >&2
    exit 1
fi

FFMPEG_VER=$(ffmpeg -version 2>&1 | head -1)
echo "Using: $FFMPEG_VER"

# ---------------------------------------------------------------------------
# 2. Create output directory
# ---------------------------------------------------------------------------
mkdir -p "$FIXTURES_DIR"
echo "Output directory: $FIXTURES_DIR"

# ---------------------------------------------------------------------------
# Helper: skip generation if file already exists and is non-empty
# ---------------------------------------------------------------------------
need_generate() {
    local path="$1"
    if [[ -f "$path" && -s "$path" ]]; then
        echo "  SKIP  $path (already exists)"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# 3a. static_1080p30.h264
#     30 seconds of static noise, no significant motion.
#     IDR (keyframe) every 10 s (300 frames at 30 fps → keyint=300).
#     Target bitrate ~500 kbps.
# ---------------------------------------------------------------------------
STATIC="$FIXTURES_DIR/static_1080p30.h264"
if need_generate "$STATIC"; then
    echo "  GEN   static_1080p30.h264 ..."
    ffmpeg -y \
        -f lavfi \
        -i "testsrc2=size=1920x1080:rate=30" \
        -t 30 \
        -c:v libx264 \
        -x264-params "keyint=300:min-keyint=300:scenecut=0" \
        -b:v 500k \
        -vf "noise=alls=0:allf=t" \
        -f h264 \
        "$STATIC" \
        2>/dev/null
    echo "  OK    static_1080p30.h264"
fi

# ---------------------------------------------------------------------------
# 3b. motion_1080p30.h264
#     60 seconds with three clear motion bursts at t=10 s, t=30 s, t=50 s.
#     Moving coloured boxes force high intra-refresh and bitrate spikes that
#     the emd inspector will detect.
# ---------------------------------------------------------------------------
MOTION="$FIXTURES_DIR/motion_1080p30.h264"
if need_generate "$MOTION"; then
    echo "  GEN   motion_1080p30.h264 ..."
    ffmpeg -y \
        -f lavfi \
        -i "testsrc2=size=1920x1080:rate=30,\
drawbox=enable='between(t,10,12)':x=100:y=100:w=400:h=400:color=red:t=fill,\
drawbox=enable='between(t,30,32)':x=500:y=200:w=400:h=400:color=blue:t=fill,\
drawbox=enable='between(t,50,52)':x=200:y=300:w=400:h=400:color=green:t=fill" \
        -t 60 \
        -c:v libx264 \
        -x264-params "keyint=300:min-keyint=300:scenecut=0" \
        -b:v 2M \
        -f h264 \
        "$MOTION" \
        2>/dev/null
    echo "  OK    motion_1080p30.h264"
fi

# ---------------------------------------------------------------------------
# 3c. gradual_1080p30.h264
#     90 seconds of very slow brightness increase simulating dawn.
#     The emd gradual scene change detector should fire.
# ---------------------------------------------------------------------------
GRADUAL="$FIXTURES_DIR/gradual_1080p30.h264"
if need_generate "$GRADUAL"; then
    echo "  GEN   gradual_1080p30.h264 ..."
    ffmpeg -y \
        -f lavfi \
        -i "testsrc2=size=1920x1080:rate=30,\
eq=brightness='0.01*t/90':contrast='1+0.5*t/90'" \
        -t 90 \
        -c:v libx264 \
        -b:v 1M \
        -f h264 \
        "$GRADUAL" \
        2>/dev/null
    echo "  OK    gradual_1080p30.h264"
fi

# ---------------------------------------------------------------------------
# 4. Sample E2E config file
# ---------------------------------------------------------------------------
E2E_TOML="$ROOT/tests/fixtures/emd_e2e.toml"
if [[ ! -f "$E2E_TOML" ]]; then
    cat >"$E2E_TOML" <<'TOML'
# emd_e2e.toml — sample config used by the E2E test harness.
# The fake RTSP server listens on 127.0.0.1:8554 and MQTT is a dummy URL
# (MQTT failure does not block clip writing — see spec §4.4).

[runtime]
log_level       = "debug"
metrics_listen  = "127.0.0.1:9464"
clip_root       = "/tmp/emd_e2e_test/clips"
inflight_root   = "/tmp/emd_e2e_test/inflight"

[recording]
container       = "mpegts"
muxer           = "intree"
fsync_policy    = "never"

[mqtt]
url             = "mqtt://127.0.0.1:11883"
client_id_prefix = "emd-e2e"

[cameras.cam0]
url               = "rtsp://127.0.0.1:8554/stream"
transport         = "tcp"
codec_hint        = "h264"
buffer_seconds    = 12
pre_roll_seconds  = 4
post_roll_seconds = 6
clip_max_seconds  = 60
motion_z_high     = 2.5
intra_ratio_high  = 2.0
TOML
    echo "  GEN   tests/fixtures/emd_e2e.toml"
fi

# ---------------------------------------------------------------------------
# 5. Print sizes
# ---------------------------------------------------------------------------
echo ""
echo "Generated fixture sizes:"
for f in "$STATIC" "$MOTION" "$GRADUAL"; do
    if [[ -f "$f" ]]; then
        SIZE=$(du -sh "$f" 2>/dev/null | cut -f1)
        printf "  %-40s %s\n" "$(basename "$f")" "$SIZE"
    fi
done
echo ""
echo "Fixture generation complete."
