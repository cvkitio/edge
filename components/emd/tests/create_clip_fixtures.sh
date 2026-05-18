#!/usr/bin/env bash
#
# Create reference fixtures for clip validation
# Decodes a clip and saves frame metadata for comparison
#

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <clip.mpegts> [output_fixture.json]"
    exit 1
fi

clip="$1"
fixture="${2:-$(basename "$clip" .mpegts)_fixture.json}"

echo "Creating fixture from: $clip"
echo "Output: $fixture"

# Extract detailed frame information
ffprobe -v error \
    -select_streams v:0 \
    -show_entries frame=pkt_pts_time,pkt_size,key_frame,pict_type \
    -of json \
    "$clip" > "$fixture"

# Validate all frames decode
if ffmpeg -v error -i "$clip" -f null - 2>&1; then
    echo "✓ All frames validated"
    frame_count=$(jq '.frames | length' "$fixture")
    echo "✓ Fixture created: $frame_count frames"
else
    echo "✗ Decode errors detected"
    exit 1
fi
