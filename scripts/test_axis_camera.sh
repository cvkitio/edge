#!/usr/bin/env bash
# Test emd against the live AXIS P4708-PLVE panoramic camera at 10.45.81.7
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
CONFIG="$ROOT/configs/axis_p4708.toml"
CLIP_DIR="/tmp/emd_axis/clips"
INFLIGHT_DIR="/tmp/emd_axis/inflight"

mkdir -p "$CLIP_DIR" "$INFLIGHT_DIR"

echo "AXIS P4708-PLVE live test"
echo "  Camera:   10.45.81.7"
echo "  Config:   $CONFIG"
echo "  Clips:    $CLIP_DIR"
echo ""
echo "Clips appear in $CLIP_DIR as .ts files."
echo "Press Ctrl+C to stop."
echo ""

exec "$ROOT/build/emd" -c "$CONFIG"
