# Phase 2 Quick Start

## Build

```bash
# 1. Build C library (libemd)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# 2. Copy artifacts to third_party (for Go build)
cp build/libemd.a third_party/libemd/lib/

# 3. Build Go agent
CGO_ENABLED=1 go build -o build/emd-agent ./cmd/emd-agent
```

## Test

### Against Axis Camera
```bash
# Hardcoded URL in internal/agent/agent.go:
# rtsp://root:***REDACTED_PASSWORD***@10.45.81.7/axis-media/media.amp

./build/emd-agent
# Watch for EVENT and STATS lines
# Ctrl-C to stop
```

### Against Video File
To test with `../../video-synthesizer/feed.mp4`, you need:

1. **Option A: mediamtx**
   ```bash
   # Install mediamtx (https://github.com/bluenviron/mediamtx)
   # Edit mediamtx.yml:
   paths:
     test:
       source: file:///path/to/feed.mp4
   
   # Run mediamtx, then change agent URL to rtsp://localhost:8554/test
   ```

2. **Option B: ffmpeg-rtsp-server**
   ```bash
   # Use a simple RTSP server like:
   # https://github.com/aler9/rtsp-simple-server
   ```

## What Works

✅ Single camera ingestion  
✅ RTSP/RTP/H.264 depacketization  
✅ Motion detection (z-score calculation)  
✅ Events C → Go via callbacks  
✅ Stats sampling  
✅ Clean shutdown  

## What's TODO

📋 TOML config parsing  
📋 Multi-camera support  
📋 NATS/MQTT publishers  
📋 S3 uploader  
📋 Outbox (durable queue)  
📋 Gate rules  
📋 Recorder driver  
📋 Control plane  
📋 Metrics/traces  

See `PHASE2_IMPLEMENTATION.md` for full details.
