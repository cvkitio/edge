#!/bin/bash
# API Test Script for emd-agent Runtime Configuration API
#
# This script tests all API endpoints and provides usage examples.

set -e

API_URL="${API_URL:-http://localhost:8080}"

echo "==================================="
echo "emd-agent API Test Suite"
echo "==================================="
echo "API URL: $API_URL"
echo ""

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

test_endpoint() {
    local name="$1"
    local method="$2"
    local path="$3"
    local data="$4"

    echo -e "${BLUE}Test: ${name}${NC}"
    echo "→ ${method} ${path}"

    if [ -n "$data" ]; then
        echo "Data: $data"
        response=$(curl -s -X "$method" "$API_URL$path" \
            -H "Content-Type: application/json" \
            -d "$data")
    else
        response=$(curl -s -X "$method" "$API_URL$path")
    fi

    echo "$response" | jq '.' 2>/dev/null || echo "$response"
    echo ""
}

# Test 1: Health Check
test_endpoint "Health Check" "GET" "/health"

# Test 2: List Cameras
test_endpoint "List All Cameras" "GET" "/api/cameras"

# Test 3: Get Inspector Config
echo -e "${BLUE}Test: Get Inspector Configuration${NC}"
echo "→ GET /api/cameras/{name}/config"
echo "Replace {name} with your camera name from the list above"
echo "Example:"
echo "  curl http://localhost:8080/api/cameras/axis_82_2/config | jq"
echo ""

# Test 4: Update Inspector Config (Full)
echo -e "${BLUE}Test: Update Inspector Configuration (Full)${NC}"
echo "→ PUT /api/cameras/{name}/config"
echo ""
echo "Full config update example:"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 4.5,
    "intra_ratio_high": 3.0,
    "on_threshold": 3,
    "off_threshold": 50,
    "bpf_floor": 100.0,
    "gradual_enabled": false,
    "gradual_threshold": 0.15,
    "gradual_window_frames": 900
  }' | jq
EOF
echo ""

# Test 5: Update Inspector Config (Partial)
echo -e "${BLUE}Test: Update Inspector Configuration (Partial)${NC}"
echo "→ PUT /api/cameras/{name}/config"
echo ""
echo "Partial config update example (only changed fields):"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 5.0,
    "on_threshold": 3
  }' | jq
EOF
echo ""

# Test 6: Common Use Cases
echo "==================================="
echo "Common Use Cases"
echo "==================================="
echo ""

echo -e "${GREEN}1. Reduce False Positives (Make Less Sensitive)${NC}"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{"motion_z_high": 6.0, "on_threshold": 3}'
EOF
echo ""
echo ""

echo -e "${GREEN}2. Increase Sensitivity (Detect More Motion)${NC}"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{"motion_z_high": 2.5, "on_threshold": 2}'
EOF
echo ""
echo ""

echo -e "${GREEN}3. Enable Gradual Scene Change Detection${NC}"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{"gradual_enabled": true, "gradual_threshold": 0.15}'
EOF
echo ""
echo ""

echo -e "${GREEN}4. Adjust Debouncing (Reduce Flickering Events)${NC}"
cat <<'EOF'
curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config \
  -H "Content-Type: application/json" \
  -d '{"on_threshold": 4, "off_threshold": 60}'
EOF
echo ""
echo ""

# Test 7: Parameter Descriptions
echo "==================================="
echo "Parameter Descriptions"
echo "==================================="
echo ""
cat <<'EOF'
motion_z_high (float, 0-100)
  Z-score threshold for motion detection
  Lower = more sensitive, Higher = less sensitive
  Default: 3.0
  Recommended range: 2.0 - 10.0

intra_ratio_high (float, 0-100)
  Intra-macroblock ratio threshold
  Detects motion based on compression changes
  Default: 2.5
  Recommended range: 2.0 - 5.0

on_threshold (uint8, 1-255)
  Consecutive frames above threshold to trigger
  Higher = reduces flickering/false positives
  Default: 2
  Recommended range: 2 - 5

off_threshold (uint8, 1-255)
  Consecutive frames below threshold to return to idle
  Higher = keeps event active longer
  Default: 45
  Recommended range: 30 - 90

bpf_floor (float)
  Minimum bytes-per-frame denominator (prevents div/0)
  Usually don't need to change this
  Default: 100.0

gradual_enabled (bool)
  Enable detection of gradual scene changes
  Useful for detecting slow lighting changes
  Default: false

gradual_threshold (float)
  Threshold for gradual change detection
  Default: 0.15
  Recommended range: 0.1 - 0.3

gradual_window_frames (uint32)
  Number of frames for gradual detection window
  Default: 900 (30 seconds at 30fps)
EOF
echo ""

# Test 8: Error Handling
echo "==================================="
echo "Error Handling Examples"
echo "==================================="
echo ""

echo -e "${RED}Invalid camera name:${NC}"
test_endpoint "Nonexistent Camera" "GET" "/api/cameras/nonexistent/config"

echo -e "${RED}Invalid JSON:${NC}"
echo "curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config -d 'invalid json'"
echo "(Returns 400 Bad Request with error message)"
echo ""

echo -e "${RED}Out of range parameter:${NC}"
echo "curl -X PUT http://localhost:8080/api/cameras/axis_82_2/config -d '{\"motion_z_high\": 150}'"
echo "(Returns 400 Bad Request - motion_z_high must be between 0 and 100)"
echo ""

echo "==================================="
echo "API Documentation Complete"
echo "==================================="
echo ""
echo "For production use:"
echo "  1. Add authentication (API key or JWT)"
echo "  2. Enable HTTPS"
echo "  3. Add rate limiting"
echo "  4. Monitor API access logs"
echo ""
