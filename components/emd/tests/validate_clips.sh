#!/usr/bin/env bash
#
# Validate MPEG-TS clips are properly formed and decodable
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIPS_DIR="${1:-/tmp/emd-clips-test4/clips}"

echo "=== Clip Validation Test ==="
echo "Clips directory: $CLIPS_DIR"
echo

# Find all .mpegts files
clips=()
while IFS= read -r -d '' clip; do
    clips+=("$clip")
done < <(find "$CLIPS_DIR" -name "*.mpegts" -print0)

if [ ${#clips[@]} -eq 0 ]; then
    echo "ERROR: No clips found in $CLIPS_DIR"
    exit 1
fi

echo "Found ${#clips[@]} clips to validate"
echo

passed=0
failed=0

for clip in "${clips[@]}"; do
    basename=$(basename "$clip")
    echo "Validating: $basename"
    
    # Test 1: ffprobe can detect codec
    if ! ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$clip" 2>&1 | grep -q "h264"; then
        echo "  ✗ FAIL: Codec not detected"
        ((failed++))
        continue
    fi
    echo "  ✓ Codec detected: H.264"
    
    # Test 2: Get frame count and validate
    frame_count=$(ffprobe -v error -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of default=nokey=1:noprint_wrappers=1 "$clip" 2>&1)
    if [ -z "$frame_count" ] || [ "$frame_count" -lt 1 ]; then
        echo "  ✗ FAIL: No frames found"
        ((failed++))
        continue
    fi
    echo "  ✓ Frame count: $frame_count"
    
    # Test 3: Decode all frames and check for errors
    if ! ffmpeg -v error -i "$clip" -f null - 2>&1 | grep -q "error"; then
        echo "  ✓ All frames decode successfully"
        ((passed++))
    else
        echo "  ✗ FAIL: Decode errors found"
        ((failed++))
    fi
    echo
done

echo "=== Results ==="
echo "Passed: $passed"
echo "Failed: $failed"
echo

if [ $failed -gt 0 ]; then
    exit 1
fi

echo "✓ All clips validated successfully"
