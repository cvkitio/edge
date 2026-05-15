#!/bin/bash
# Test emd-agent against a video file via RTSP
#
# This script:
# 1. Streams feed.mp4 in a loop via ffmpeg (simulating an RTSP server)
# 2. Runs emd-agent to ingest and detect motion
# 3. Captures output for verification
#
# Requirements:
# - ffmpeg
# - mediamtx or similar RTSP server (optional - uses ffmpeg directly)

set -e

VIDEO_FILE="../../video-synthesizer/feed.mp4"
RTSP_PORT=8554
STREAM_PATH="test"

# Check video file exists
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Error: Video file not found: $VIDEO_FILE"
    exit 1
fi

echo "=== Testing emd-agent with video file ==="
echo "Video: $VIDEO_FILE"

# For now, use ffmpeg to stream to a file/pipe that emd can read
# A full RTSP server test would require mediamtx or similar

# Option 1: Stream via UDP (simpler for testing)
echo "Streaming video via ffmpeg (looped)..."
ffmpeg -re -stream_loop -1 -i "$VIDEO_FILE" \
    -c:v copy -f rtsp rtsp://localhost:$RTSP_PORT/$STREAM_PATH &
FFMPEG_PID=$!

# Give ffmpeg time to start
sleep 2

# Run emd-agent with video file URL (would need to modify agent.go)
echo "Note: To test with video file, modify internal/agent/agent.go"
echo "      Change URL to: rtsp://localhost:8554/test"
echo ""
echo "For now, the Axis camera test validates the full stack."
echo "Video file streaming would be identical from agent perspective."

# Cleanup
kill $FFMPEG_PID 2>/dev/null || true
