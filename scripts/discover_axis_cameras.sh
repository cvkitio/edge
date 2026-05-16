#!/bin/bash
# discover_axis_cameras.sh - Scan Axis cameras and discover all available streams
#
# Usage: ./discover_axis_cameras.sh <ip_list_file> <username> <password>
#
# This script:
# 1. Connects to each Axis camera
# 2. Queries RTSP DESCRIBE for available streams
# 3. Detects codec, resolution, framerate for each stream
# 4. Outputs JSON with all discovered feeds

set -e

if [ $# -lt 3 ]; then
    echo "Usage: $0 <ip_list_file> <username> <password>"
    echo ""
    echo "Example:"
    echo "  echo '10.45.81.1' > cameras.txt"
    echo "  echo '10.45.81.2' >> cameras.txt"
    echo "  $0 cameras.txt root 'password'"
    exit 1
fi

IP_FILE="$1"
USERNAME="$2"
PASSWORD="$3"

# URL encode password
PASSWORD_ENC=$(echo -n "$PASSWORD" | jq -sRr @uri)

echo "{"
echo "  \"cameras\": ["

FIRST=true

while IFS= read -r IP; do
    # Skip comments and empty lines
    [[ "$IP" =~ ^#.*$ ]] && continue
    [[ -z "$IP" ]] && continue

    echo "  Scanning $IP..." >&2

    # Try common Axis RTSP paths
    PATHS=(
        "/axis-media/media.amp"
        "/axis-media/media.amp?videocodec=h264"
        "/axis-media/media.amp?videocodec=h265"
        "/axis-media/media.amp?camera=1"
        "/axis-media/media.amp?camera=2"
        "/axis-media/media.amp?camera=3"
        "/axis-media/media.amp?camera=4"
    )

    for PATH in "${PATHS[@]}"; do
        URL="rtsp://${USERNAME}:${PASSWORD_ENC}@${IP}${PATH}"

        # Probe the stream (timeout 5s)
        PROBE_JSON=$(ffprobe -v quiet -print_format json -show_streams -show_format \
            -rtsp_transport tcp -timeout 5000000 "$URL" 2>/dev/null || echo "{}")

        # Check if we got valid video stream info
        CODEC=$(echo "$PROBE_JSON" | jq -r '.streams[]? | select(.codec_type=="video") | .codec_name' | head -1)

        if [ -n "$CODEC" ] && [ "$CODEC" != "null" ]; then
            WIDTH=$(echo "$PROBE_JSON" | jq -r '.streams[]? | select(.codec_type=="video") | .width' | head -1)
            HEIGHT=$(echo "$PROBE_JSON" | jq -r '.streams[]? | select(.codec_type=="video") | .height' | head -1)
            FPS_STR=$(echo "$PROBE_JSON" | jq -r '.streams[]? | select(.codec_type=="video") | .r_frame_rate' | head -1)

            # Calculate FPS from fraction
            if [ -n "$FPS_STR" ] && [ "$FPS_STR" != "null" ]; then
                FPS=$(echo "$FPS_STR" | awk -F/ '{if ($2) print $1/$2; else print $1}')
            else
                FPS="unknown"
            fi

            # Add comma if not first entry
            if [ "$FIRST" = false ]; then
                echo ","
            fi
            FIRST=false

            # Output JSON entry
            cat <<EOF
    {
      "ip": "$IP",
      "path": "$PATH",
      "url": "rtsp://${USERNAME}:PASSWORD@${IP}${PATH}",
      "codec": "$CODEC",
      "width": $WIDTH,
      "height": $HEIGHT,
      "fps": $FPS
    }
EOF

            echo "    Found: $IP$PATH - $CODEC ${WIDTH}x${HEIGHT} @ ${FPS}fps" >&2
        fi
    done

done < "$IP_FILE"

echo ""
echo "  ]"
echo "}"
