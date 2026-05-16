# emd — Edge Motion Detector

A high-performance video motion detection system for IP cameras that analyzes **encoded video streams** without pixel decoding. Detects motion using statistical analysis of H.264/H.265 frame characteristics.

Available as both a **standalone C daemon** (Phase 1) and a **Go agent with C library** (Phase 2) for flexible deployment.

**Key Innovation**: Motion detection via **z-score analysis** of frame-level statistics — no expensive pixel decoding required.

---

## Features

- **RTSP ingest** — RTP/AVP over TCP (interleaved) or UDP; H.264 (AVC) and H.265 (HEVC)
- **Encoded-domain motion detection** — z-score analysis of bytes-per-frame and I-frame ratios; never decodes pixels
- **Multi-camera support** — Phase 2 Go agent handles dozens of cameras per instance
- **Pre-roll / post-roll clips** — atomic write to MPEG-TS (`.ts`) or fragmented MP4 (`.mp4`)
- **Runtime configuration API** — REST API for tuning motion detection parameters without restart
- **Kubernetes health checks** — `/healthz` and `/readyz` endpoints for liveness and readiness probes
- **Event notifications** — MQTT publishing and event callbacks
- **Prometheus metrics** — per-camera counters, histograms, and system metrics
- **Disk management** — automatic clip retention and quota enforcement
- **Lock-free hot path** — C11 `_Atomic` throughout; no mutexes on camera worker threads
- **Container-ready** — Docker images for x86-64 and ARM64
- **Kubernetes-ready** — Helm charts and manifests for production deployment

---

## Architecture

### Phase 2 (Current): Go Agent + C Library

```
┌─────────────────────────────────────┐
│         Go Agent (emd-agent)        │
│                                     │
│  ┌──────────────────────────────┐  │
│  │  Supervisor (agent.Run)      │  │
│  │  - Config loading (TOML)     │  │
│  │  - Multi-camera coordination │  │
│  │  - Event aggregation         │  │
│  └──────┬───────────────────────┘  │
│         │ cgo                       │
│  ┌──────▼───────────────────────┐  │
│  │   libemd (C library)         │  │
│  │   - RTSP/RTP ingestion       │  │
│  │   - H.264/H.265 parsing      │  │
│  │   - Motion detection         │  │
│  │   - Ring buffer              │  │
│  └──────────────────────────────┘  │
└─────────────────────────────────────┘
         │
         ▼
   [IP Cameras via RTSP]
```

**Phase 2 advantages**:
- Easy multi-camera configuration via TOML
- Go standard library for HTTP/metrics/config
- Clean C ABI for performance-critical video processing
- Flexible deployment (binary, container, K8s)

### Phase 1: Standalone C Daemon

Single-process C daemon with integrated supervisor. See `docs/PHASE1.md` for details.

---

## Motion Detection: Z-Score Analysis

### How It Works

Instead of decoding video frames to pixels (expensive), emd analyzes the **encoded bitstream statistics**:

1. **Bytes Per Frame (BPF)** — Track the size of each compressed video frame
2. **I-frame Ratio** — Monitor the ratio of I-frames (keyframes) to P-frames
3. **Exponential Moving Average (EWMA)** — Calculate running baseline: `EWMA = α × current + (1-α) × previous`
4. **Standard Deviation (σ)** — Track variance using Welford's online algorithm
5. **Z-Score** — Measure how many standard deviations from baseline:

```
z = (current_value - EWMA) / σ
```

### Why Z-Scores?

**Motion = sudden change in compression characteristics**

- **Motion events** → More inter-frame differences → Larger P-frames → Higher BPF
- **Static scene** → Minimal differences → Small P-frames → Stable low BPF
- **Scene change** → New keyframe required → I-frame ratio spikes

**Z-score advantages**:
- **Scene-adaptive** — Auto-adjusts to each camera's baseline (indoor vs outdoor, resolution, bitrate)
- **Statistically robust** — `z > 3.0` means 99.7% confidence of significant change
- **No calibration** — Works out-of-box for any camera/scene
- **Efficient** — Simple arithmetic, no matrix operations or ML inference

### Example

```
Typical idle scene:
  BPF: 1200 ± 200 bytes  (baseline ± noise)
  z-score: -0.5 to +0.5   (within normal variance)

Motion event (person walking):
  BPF: 4500 bytes         (sudden spike)
  z-score: 16.5           (16.5σ above baseline!)
  → EVENT TRIGGERED

Gradual dawn/dusk:
  BPF slowly drifts from 1200 → 1800 over 30 minutes
  EWMA adapts, z-score stays < 2.0
  → No false alarm
```

### Configuration

```toml
[cameras.example]
motion_z_high     = 3.0     # z-score threshold (3.0 = 99.7% confidence)
on_threshold      = 2       # consecutive frames above threshold to trigger
off_threshold     = 15      # frames below threshold before ending event
gradual_enabled   = true    # enable dual-EWMA for scene change suppression
```

**Tuning**:
- **Lower threshold (2.0-2.5)**: More sensitive, catches subtle motion, more false positives
- **Higher threshold (4.0-5.0)**: Only significant motion, fewer false positives
- **Default (3.0)**: Balanced for most scenarios (99.7% confidence interval)

---

## Quick Start

### Phase 2: Go Agent (Recommended)

**Prerequisites:** Go 1.22+, CMake ≥ 3.22, GCC 12+ or Clang 16+

```bash
# Build C library
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build --parallel

# Build Go agent
CGO_ENABLED=1 go build -o build/emd-agent ./cmd/emd-agent

# Create config (see k8s/agent.toml.example)
cp k8s/agent.toml.example config.toml
# Edit config.toml with your camera URLs

# Run
./build/emd-agent \
  --config config.toml \
  --api :8080 \
  --metrics :9464 \
  --pprof localhost:6060
```

### Docker

```bash
# Build image
docker build -t emd-agent:latest .

# Run with config volume
docker run \
  -p 8080:8080 \
  -p 9464:9464 \
  -v $(pwd)/config.toml:/etc/emd-agent/agent.toml:ro \
  -v emd-clips:/var/lib/emd-agent/clips \
  emd-agent:latest

# Access API
curl http://localhost:8080/api/cameras

# Access metrics
curl http://localhost:9464/metrics
```

### Kubernetes

```bash
# Copy and edit config
cp k8s/agent.toml.example k8s/agent.toml
# Edit k8s/agent.toml with your cameras

# Deploy
kubectl create namespace emd
kubectl create configmap emd-agent-config --from-file=agent.toml=k8s/agent.toml -n emd
kubectl apply -f k8s/deployment.yaml
```

---

## Configuration

### Multi-Camera TOML Config

```toml
[agent]
mode              = "single"
instance_id       = "cluster-01"
data_dir          = "/var/lib/emd-agent"

[runtime]
log_level         = "info"
clip_root         = "/var/lib/emd-agent/clips"
inflight_root     = "/var/lib/emd-agent/inflight"

[recording]
container         = "mpegts"      # mpegts | fmp4
fsync_policy      = "on_close"    # on_close | always | never

[disk]
max_bytes_per_camera = 2_000_000_000  # 2 GB per camera
retention_days       = 7

# Camera definitions
[cameras.front_door]
url               = "rtsp://username:password@192.168.1.101/stream"
transport         = "tcp"          # tcp | udp
codec_hint        = "h264"         # auto | h264 | h265
buffer_seconds    = 20             # ring buffer size
pre_roll_seconds  = 6              # capture before motion
post_roll_seconds = 10             # capture after motion
clip_max_seconds  = 120            # max clip length
motion_z_high     = 3.0            # z-score threshold
intra_ratio_high  = 2.5            # I-frame ratio threshold
on_threshold      = 2              # frames to trigger
off_threshold     = 15             # frames to end event
gradual_enabled   = false          # dual-EWMA for gradual changes
max_bitrate_bps   = 8000000        # 8 Mbps max

[cameras.back_yard]
url               = "rtsp://username:password@192.168.1.102/stream"
# ... same parameters ...
```

### RTSP URL Formats

**Axis cameras:**
```
rtsp://user:pass@IP/axis-media/media.amp?videocodec=h264
```

**Hikvision:**
```
rtsp://user:pass@IP:554/Streaming/Channels/101
```

**Dahua:**
```
rtsp://user:pass@IP:554/cam/realmonitor?channel=1&subtype=0
```

**Generic:**
```
rtsp://user:pass@IP:554/stream
```

---

## Runtime Configuration API

The agent exposes a REST API (default port 8080) for runtime configuration changes without restart.

### API Endpoints

**Health check:**
```bash
GET /health
```

**List cameras:**
```bash
GET /api/cameras
```

**Get motion detection config:**
```bash
GET /api/cameras/{name}/config
```

**Update motion detection config:**
```bash
PUT /api/cameras/{name}/config
Content-Type: application/json

{
  "motion_z_high": 5.0,
  "on_threshold": 3
}
```

### Common Use Cases

**Reduce false positives** (static scenes):
```bash
curl -X PUT http://localhost:8080/api/cameras/front_door/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 6.0,
    "on_threshold": 3,
    "off_threshold": 60
  }'
```

**Increase sensitivity** (high-activity areas):
```bash
curl -X PUT http://localhost:8080/api/cameras/parking_lot/config \
  -H "Content-Type: application/json" \
  -d '{
    "motion_z_high": 2.5,
    "on_threshold": 2
  }'
```

**Verify configuration change:**
```bash
curl http://localhost:8080/api/cameras/front_door/config | jq
```

### Tunable Parameters

| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| `motion_z_high` | float | 0-100 | 3.0 | Z-score threshold - higher = less sensitive |
| `intra_ratio_high` | float | 0-100 | 2.5 | Intra-macroblock ratio threshold |
| `on_threshold` | uint8 | 1-255 | 2 | Consecutive frames to trigger event |
| `off_threshold` | uint8 | 1-255 | 45 | Consecutive frames to return to idle |
| `bpf_floor` | float | > 0 | 100.0 | Minimum bytes-per-frame (prevents div/0) |
| `configured_periodic_kf` | bool | - | false | Camera sends periodic keyframes |
| `gradual_enabled` | bool | - | false | Enable gradual scene change detection |
| `gradual_threshold` | float | 0-1 | 0.15 | Gradual change threshold |
| `gradual_window_frames` | uint32 | > 0 | 900 | Gradual detection window size |

**Note**: The API supports **partial updates** — only send fields you want to change.

---

## Build Options

### C Library Only

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
cmake --build build --parallel
# Output: build/libemd.a, build/libemd.so
```

### Go Agent

```bash
CGO_ENABLED=1 go build -o build/emd-agent ./cmd/emd-agent
```

### Phase 1 Standalone Binary

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# Output: build/emd (static-PIE, ~5 MB)
```

### Cross-Compile for ARM64

```bash
# C library
cmake -S . -B build-arm64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF
cmake --build build-arm64 --parallel

# Go agent
GOOS=linux GOARCH=arm64 CGO_ENABLED=1 \
    CC=aarch64-linux-gnu-gcc \
    go build -o build/emd-agent-arm64 ./cmd/emd-agent
```

---

## Testing

### Unit Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
cd build && ctest --output-on-failure
```

### End-to-End Test

```bash
bash scripts/e2e_test.sh
```

Tests video file ingestion and motion detection against pre-recorded fixtures.

### Live Camera Test

```bash
# Edit configs/axis_p4708.toml with your camera URL
./build/emd-agent --config configs/axis_p4708.toml
```

---

## Monitoring

### Health Check Endpoints

For Kubernetes liveness and readiness probes (default port 9464):

**Liveness probe** (always returns 200 if process running):
```bash
GET /healthz
```

**Readiness probe** (returns 200 if cameras connected):
```bash
GET /readyz
```

**Camera status**:
```bash
GET /health/cameras
```

Example Kubernetes config:
```yaml
livenessProbe:
  httpGet:
    path: /healthz
    port: 9464
  initialDelaySeconds: 10
  periodSeconds: 30

readinessProbe:
  httpGet:
    path: /readyz
    port: 9464
  initialDelaySeconds: 5
  periodSeconds: 10
```

### Logs

**Structured JSON logs** (one per line):
```json
{"ts":"2026-05-16T00:48:46Z","level":"info","subsystem":"cam","msg":"opened camera front_door (cam_id=0)"}
```

**Event logs**:
```
2026/05/16 00:48:47 EVENT: cam=front_door type=motion reason=z=12.43 pts=2773458478
```

**Clip creation logs**:
```
2026/05/16 00:48:50 CLIP: created front_door_20260516_004850_a3f2b1c8.ts size=15MB duration=16.2s z=12.43 (recorded in 0.3s)
```

**Stats logs** (periodic):
```
2026/05/16 00:48:52 STATS: cam=0 bpf_ewma=248.8 fsm=0 rtsp=0
```

**Memory stats** (every 30s):
```
2026/05/16 00:49:00 MEMORY: alloc=42MB sys=68MB heapAlloc=42MB heapSys=64MB numGC=12 goroutines=35
```

### Prometheus Metrics

Exposed on `:9464/metrics`:

**Camera metrics:**
```prometheus
# Camera connection status (1=connected, 0=disconnected)
emd_camera_status{camera="front_door"} 1

# Number of cameras connected
emd_cameras_connected 14

# Total configured cameras
emd_cameras_total 14
```

**Event metrics:**
```prometheus
# Motion events per camera
emd_events_total{camera="front_door",type="motion"} 42

# Total motion events across all cameras
emd_motion_events_total 156
```

**Recording metrics:**
```prometheus
# Clips created per camera
emd_clips_created_total{camera="front_door"} 12

# Bytes written per camera
emd_clip_bytes_total{camera="front_door"} 185400320

# Clip duration histogram
emd_clip_duration_seconds_bucket{camera="front_door",le="15"} 8
emd_clip_duration_seconds_bucket{camera="front_door",le="30"} 12

# Recording errors
emd_recording_errors_total{camera="front_door",error="write_failed"} 0
```

**System metrics:**
```prometheus
# Go runtime metrics
emd_goroutines 35
emd_memory_alloc_bytes 44040192
emd_memory_sys_bytes 71303168
emd_memory_heap_bytes 44040192

# GC pause times
emd_gc_duration_seconds_bucket{le="0.001"} 145
```

**Disk metrics:**
```prometheus
# Disk usage per camera
emd_disk_usage_bytes{camera="front_door"} 1850400000

# Clip count per camera
emd_disk_clips_total{camera="front_door"} 124

# Cleanup runs
emd_disk_cleanup_runs_total 48

# Clips deleted by cleanup
emd_disk_clips_deleted_total 12
```

---

## Performance

### Resource Usage (Phase 2)

**Per camera** (1080p30 H.264):
- CPU: 80-150m (0.08-0.15 cores) — optimized hot path with zero-copy NAL handling
- Memory: 35-40 MB — fixed-size ring buffers, no per-frame allocations
- Disk: ~200 MB/hour (default 2 GB quota per camera with 7-day retention)
- Network: 4-8 Mbps inbound

**16 cameras on single instance**:
- CPU: 1.5-3.5 cores (baseline to peak)
- Memory: 1.2-2.5 GB stable (no leaks, verified with pprof)
- Network: 64-128 Mbps
- Disk: ~50 GB with default retention (2 GB × 16 cameras + overhead)

**Tested platforms**:
- Ampere Altra (ARM64): 30+ cameras per core
- AMD Ryzen 5800X (x86-64): 20-25 cameras per core
- Raspberry Pi 4 (ARM64): 4-6 cameras total

**Recent optimizations**:
- **Zero-copy NAL processing**: Direct RTP payload to ring buffer (no intermediate malloc)
- **SPS/PPS injection**: Automatic parameter set injection for clip playback compatibility
- **Disk management**: Automatic cleanup based on age and quota (runs every 5 minutes)

---

## Deployment

### Docker Compose

```yaml
version: '3.8'
services:
  emd-agent:
    image: emd-agent:latest
    volumes:
      - ./agent.toml:/etc/emd-agent/agent.toml:ro
      - emd-data:/var/lib/emd-agent
    restart: unless-stopped
volumes:
  emd-data:
```

### Kubernetes

See `k8s/deployment.yaml` for full manifest. Key resources:

```yaml
resources:
  requests:
    cpu: "4000m"      # 4 cores for ~16 cameras
    memory: "4Gi"     # 4 GB
  limits:
    cpu: "8000m"      # 8 cores burst
    memory: "8Gi"     # 8 GB
```

**Scaling**: For 30+ cameras, use sharded deployment (4 pods × 8 cameras each).

---

## Troubleshooting

### High CPU Usage

- **Cause**: All cameras in motion simultaneously
- **Fix**: Increase CPU limits or shard across pods
- **Check**: `docker stats` or `kubectl top pods`

### No Motion Events

- **Cause**: Threshold too high or camera static
- **Fix**: Lower `motion_z_high` to 2.5 or 2.0
- **Check**: Watch `STATS` logs for `bpf_ewma` values

### Camera Connection Failed

- **Cause**: Network unreachable, wrong credentials, wrong URL
- **Fix**: Test with `ffprobe` or VLC player first
- **Check**: Logs for `"connect failed"` or `"rtsp error"`

### Memory Growth

- **Cause**: Ring buffer leak (should be fixed in recent builds)
- **Fix**: Check memory profiling at `http://localhost:6060/debug/pprof/heap`
- **Monitor**: `emd_memory_alloc_bytes` and `emd_memory_heap_bytes` metrics
- **Expected**: Stable 35-40 MB per camera after initial allocation

### Disk Full

- **Cause**: Clips accumulating faster than cleanup
- **Fix**: Lower `max_bytes_per_camera` or `retention_days` in config
- **Check**: `emd_disk_usage_bytes` and `emd_disk_clips_total` metrics
- **Manual cleanup**: `rm -rf /var/lib/emd-agent/clips/camera_name/*`
- **Automatic**: Cleanup runs every 5 minutes

### Clips Won't Play

- **Cause**: Missing SPS/PPS parameter sets
- **Fix**: Recent builds automatically inject SPS/PPS into clips
- **Test**: `ffprobe /path/to/clip.ts` should show video stream
- **Workaround**: Ensure camera sends parameter sets periodically

---

## Project Structure

```
edge/
├── cmd/emd-agent/          # Phase 2 Go agent entry point
├── internal/
│   ├── agent/              # Go supervisor and config
│   └── libemd/             # cgo bindings to C library
├── src/                    # C library implementation
├── include/emd/            # C public headers
├── tests/                  # Unit tests (cmocka)
├── scripts/                # Build and test automation
├── k8s/                    # Kubernetes manifests
├── Dockerfile              # Multi-stage container build
└── CLAUDE.md               # Codebase guide for contributors
```

---

## Module Map (C Library)

| Module | Responsibility |
|--------|----------------|
| `emd_rtsp` | RTSP state machine (DESCRIBE/SETUP/PLAY/TEARDOWN) |
| `emd_rtp` | RTP header parse, jitter handling |
| `emd_h264_depay` | RFC 6184 depacketizer (STAP-A, FU-A) |
| `emd_h265_depay` | RFC 7798 depacketizer (AP, FU) |
| `emd_h264_parse` | NAL/slice header parse, SPS/PPS cache |
| `emd_inspector` | **Z-score motion detection** |
| `emd_ringbuf` | Lock-free per-camera ring buffer |
| `emd_event` | Event bus and FSM (IDLE/ACTIVE/COOLDOWN) |
| `emd_recorder` | Clip muxing (MPEG-TS / fMP4) |
| `emd_cam` | Phase 2 ABI (camera handle, callbacks) |

---

## License

**MIT License** — See [LICENSE](LICENSE)

All vendored dependencies are MIT or Apache 2.0. No GPL/LGPL code statically linked.

---

## Contributing

See [CLAUDE.md](CLAUDE.md) for codebase architecture and coding standards.

**Key rules**:
- No `pthread_mutex_lock` on hot path
- Use C11 `_Atomic` with explicit memory orders
- Zero-copy from RTP into ring buffer
- All public functions in `include/emd/*.h`

---

## References

- [RFC 6184](https://datatracker.ietf.org/doc/html/rfc6184) — RTP Payload Format for H.264 Video
- [RFC 7798](https://datatracker.ietf.org/doc/html/rfc7798) — RTP Payload Format for H.265/HEVC Video
- [RFC 2326](https://datatracker.ietf.org/doc/html/rfc2326) — Real Time Streaming Protocol (RTSP)
- [ITU-T H.264](https://www.itu.int/rec/T-REC-H.264) — Advanced Video Coding specification

---

**Questions?** Open an issue at [github.com/cvkitio/edge](https://github.com/cvkitio/edge)
