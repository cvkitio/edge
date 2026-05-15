# emd — Edge Motion Detector

A high-performance C11 daemon that ingests live video from IP cameras over RTSP, detects motion and scene changes using **encoded-domain heuristics** (no pixel decoding on the hot path), and writes self-contained MPEG-TS or fragmented MP4 clips to disk while publishing structured MQTT notifications.

Designed for **ARMv8 edge hardware** (Ampere Altra, Raspberry Pi 4/5, Rockchip RK3588) and x86-64 servers. One Ampere core comfortably handles 30+ simultaneous 1080p30 H.264 streams at well under 60% CPU.

---

## Features

- **RTSP ingest** — RTP/AVP over TCP (interleaved) or UDP; H.264 (AVC) and H.265 (HEVC).
- **Encoded-domain motion detection** — inspects NAL headers, slice types, and per-frame byte rates; never decodes pixels in the common case.
- **Gradual scene change detection** — dual-EWMA baseline catches dawn/dusk, lighting changes, and lens contamination.
- **Pre-roll / post-roll clips** — atomic write to MPEG-TS (`.ts`) or fragmented MP4 (`.mp4`) with configurable pre/post windows and event coalescing.
- **MQTT notifications** — structured JSON events and clip payloads published after successful atomic rename.
- **Prometheus metrics** — per-camera counters and histograms on a configurable listen address.
- **Lock-free hot path** — C11 `_Atomic` throughout; no `pthread_mutex_lock` on any camera worker thread.
- **Static-PIE binary** — ~3–5 MB; runs in a `scratch` container.

---

## Build

**Prerequisites:** CMake ≥ 3.22, Ninja (or Make), GCC 12+ or Clang 16+, POSIX threads.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The resulting binary is at `build/emd`.

**Debug build (no optimisation, full debug info):**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

**Cross-compile for aarch64** (with a suitable toolchain):

```sh
cmake -S . -B build-arm64 \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 --parallel
```

---

## Unit Tests

```sh
cd build
ctest --output-on-failure
```

Or, to build and run in one step using the helper script:

```sh
bash scripts/run_tests.sh
```

This script also runs the full end-to-end harness after the unit suite passes.

---

## End-to-End Tests

The E2E test stands up a synthetic RTSP server that streams a pre-generated H.264 fixture, runs the `emd` binary against it, and validates that clip files are written to disk within 90 seconds.

**Prerequisites:** Python 3.9+, ffmpeg (for fixture generation), ffprobe (optional, for clip validation).

```sh
bash scripts/e2e_test.sh
```

The script will:
1. Build `emd` in Release mode.
2. Generate synthetic H.264 fixture files under `tests/fixtures/streams/` (if absent).
3. Start the fake RTSP server and `emd`.
4. Assert that at least one `.ts` clip is produced and is valid.

You can also run the Python orchestrator directly for more control:

```sh
python3 tests/integration/e2e_test.py \
    --binary build/emd \
    --fixtures-dir tests/fixtures/streams \
    --scenario motion \
    --container mpegts
```

---

## Configuration

`emd` reads a TOML 1.0 config file. The default path is `/etc/emd/emd.toml`; override with `-c`:

```sh
build/emd -c /path/to/emd.toml
```

### Minimal config example

```toml
[runtime]
log_level        = "info"
metrics_listen   = "0.0.0.0:9464"
clip_root        = "/var/lib/emd/clips"
inflight_root    = "/var/lib/emd/inflight"

[recording]
container        = "mpegts"   # mpegts | fmp4
muxer            = "intree"   # intree | libav
fsync_policy     = "on_close" # on_close | always | never

[mqtt]
url              = "mqtt://broker.local:1883"
client_id_prefix = "emd"

[disk]
max_bytes_per_camera = 20_000_000_000  # 20 GB
retention_days       = 14

[cameras.driveway]
url               = "rtsp://user:pass@10.0.1.51:554/Streaming/Channels/101"
transport         = "tcp"       # tcp | udp
codec_hint        = "h264"      # auto | h264 | h265
buffer_seconds    = 12
pre_roll_seconds  = 6
post_roll_seconds = 8
clip_max_seconds  = 120
motion_z_high     = 3.0
intra_ratio_high  = 2.5

[cameras.back_garden]
url               = "rtsp://10.0.1.52:554/h264Preview_01_main"
buffer_seconds    = 10
gradual_enabled   = true
```

Config is hot-reloaded on `SIGHUP` or via the `emd/{instance}/cmd/reload` MQTT command. Invalid config on reload leaves the running config untouched.

---

## Architecture Overview

The supervisor spawns one **camera worker thread** per camera; each worker owns its RTSP/RTP state machine, NAL depacketizer, encoded-domain inspector, and a per-camera arena-backed ring buffer (lock-free SPSC). On motion detection the worker pushes a lightweight event struct into a process-wide **lock-free MPMC event bus**; a pool of **recorder threads** drains the bus, snapshots the ring buffer, and writes a clip to disk using an atomic part→fsync→rename sequence. A separate **notifier thread** publishes MQTT messages only after the rename succeeds. No camera worker ever blocks on disk I/O, MQTT I/O, or another camera's lock; backpressure is absorbed by dropping oldest pre-roll content with a Prometheus counter, never by stalling ingest.

---

## Module Map

| Module | Header | Responsibility |
|---|---|---|
| `emd_config` | `emd/config.h` | TOML config parse, validate, hotreload |
| `emd_log` | `emd/log.h` | Structured JSON logs, per-subsystem levels |
| `emd_metrics` | `emd/metrics.h` | Prometheus text exposition, lock-free counters |
| `emd_net` | `emd/net.h` | TCP/UDP sockets, epoll wrapper, timeouts |
| `emd_rtsp` | `emd/rtsp.h` | RTSP client state machine (DESCRIBE/SETUP/PLAY/TEARDOWN) |
| `emd_rtp` | `emd/rtp.h` | RTP header parse, jitter buffer, loss accounting |
| `emd_h264_depay` | `emd/h264_depay.h` | RFC 6184 depacketizer (STAP-A, FU-A, single NAL) |
| `emd_h265_depay` | `emd/h265_depay.h` | RFC 7798 depacketizer (AP, FU, single NAL) |
| `emd_h264_parse` | `emd/h264_parse.h` | NAL/slice header parse, SPS/PPS cache, Golomb decode |
| `emd_h265_parse` | `emd/h265_parse.h` | VPS/SPS/PPS cache, slice header, NAL type table |
| `emd_inspector` | `emd/inspector.h` | Encoded-domain motion heuristics |
| `emd_ringbuf` | `emd/ringbuf.h` | Per-camera bitstream ring buffer with snapshot API |
| `emd_event` | `emd/event.h` | Event types, debounce/hysteresis state machine, MPMC bus |
| `emd_recorder` | `emd/recorder.h` | Muxer dispatch (MPEG-TS / fMP4), atomic rename, fsync policy |
| `emd_mqtt` | `emd/mqtt.h` | MQTT client front-end, reconnect, LWT, QoS |
| `emd_supervisor` | `emd/supervisor.h` | Lifecycle, signals, sd_notify, hotreload orchestration |

---

## Third-Party Dependencies

All core dependencies are permissive-license only.

| Library | License | Location | Role |
|---|---|---|---|
| `tomlc99` | MIT | `third_party/tomlc99/` | TOML 1.0 config parser |
| `mqtt-c` (LiamBindle) | MIT | `third_party/mqtt-c/` | Embedded MQTT 3.1.1 client |
| `cmocka` | Apache 2.0 | `third_party/cmocka/` | Unit test and mocking framework (test only) |

No GPL components. No runtime dependency on FFmpeg, GStreamer, OpenCV, or live555. The `ffmpeg` CLI is used **in test scripts only** to generate synthetic fixtures; it is never exec'd by the production binary.

---

## License

emd is released under the **MIT License**. All vendored dependencies are MIT, Apache 2.0, or BSD — no copyleft (GPL/LGPL) code is statically linked into the production binary. Optional LGPL plugins (`emd_mux_libav`, `emd_decoder_libav`) are loaded dynamically and are entirely optional.
