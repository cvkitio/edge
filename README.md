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
- **Event notifications** — MQTT publishing and event callbacks
- **Prometheus metrics** — per-camera counters and histograms
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
./build/emd-agent --config config.toml
```

### Docker

```bash
# Build image
docker build -t emd-agent:latest .

# Run with config volume
docker run -v $(pwd)/config.toml:/etc/emd-agent/agent.toml:ro emd-agent:latest
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

### Logs

**Structured JSON logs** (one per line):
```json
{"ts":"2026-05-16T00:48:46Z","level":"info","subsystem":"cam","msg":"opened camera front_door (cam_id=0)"}
```

**Event logs**:
```
2026/05/16 00:48:47 EVENT: cam=front_door type=motion reason=z=12.43 pts=2773458478
```

**Stats logs** (periodic):
```
2026/05/16 00:48:52 STATS: cam=0 bpf_ewma=248.8 fsm=0 rtsp=0
```

### Prometheus Metrics

Exposed on `:9464/metrics`:

```
# Motion events per camera
emd_agent_events_total{camera="front_door",type="motion"} 42

# Bytes per frame EWMA
emd_agent_camera_bpf_ewma{camera="front_door"} 1823.5

# FSM state (0=IDLE, 1=ACTIVE, 2=COOLDOWN)
emd_agent_camera_fsm_state{camera="front_door"} 0
```

---

## Performance

### Resource Usage (Phase 2)

**Per camera** (1080p30 H.264):
- CPU: 80-150m (0.08-0.15 cores)
- Memory: 35-40 MB
- Network: 4-8 Mbps inbound

**16 cameras on single instance**:
- CPU: 1.5-3.5 cores (baseline to peak)
- Memory: 1.2-2.5 GB
- Network: 64-128 Mbps

**Tested platforms**:
- Ampere Altra (ARM64): 30+ cameras per core
- AMD Ryzen 5800X (x86-64): 20-25 cameras per core
- Raspberry Pi 4 (ARM64): 4-6 cameras total

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

- **Cause**: Ring buffer leak or clip retention misconfigured
- **Fix**: Check `max_bytes_per_camera` and `retention_days`
- **Monitor**: `process_resident_memory_bytes` metric

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
