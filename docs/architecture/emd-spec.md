# Edge Motion Detector — Technical Specification

**Codename:** `emd` (Edge Motion Detector)
**Document status:** Draft v0.1
**Owner:** Andrew Sinclair
**Date:** 2026-05-16
**Audience:** Implementers, reviewers, QA

---

## 1. Goals and non‑goals

### 1.1 Goals

1. Ingest live video from N IP cameras over **RTSP** (RTP/AVP, UDP and TCP interleaved) using **H.264 (AVC)** and **H.265 (HEVC)** payloads.
2. Run on **edge hardware**, primarily **ARMv8** (Ampere‑class servers, also functional on RPi 4/5 and similar SBCs) and secondarily **x86_64**, with a single codebase and CMake build.
3. Maintain a **rolling pre‑roll buffer per camera**, sized either in **seconds** or **frames**, holding the raw encoded bitstream (NAL units + RTP timing metadata) — never the decoded frames.
4. Inspect the bitstream in real time to detect **scene change / motion** using **encoded‑domain heuristics** (IDR/recovery point arrival, intra/skip MB ratios in H.264, intra CU ratios in H.265, residual coefficient energy proxy, motion‑vector magnitude proxy) **without** decoding pixels in the common case.
5. Additionally detect **gradual scene change** via a slow‑moving exponential baseline of the same encoded‑domain statistics.
6. On an event, asynchronously (a) **flush the pre‑roll buffer + post‑roll window** to a self‑contained clip on disk — either **MPEG‑TS** (default, append‑friendly) or **fragmented MP4** (when client compatibility prefers it) — and (b) publish a structured **MQTT** notification, without ever blocking the ingest threads of any camera.
7. Be **developed test‑first (TDD)**, with a deterministic per‑module unit test suite, golden bitstream fixtures, and a full integration harness containing a **simulated RTSP camera** and a **simulated MQTT broker**.
8. Permissive‑OSS only (MIT/BSD/Apache/MPL, plus LGPL **dynamic link**). No GPL components. No shelling out to the `ffmpeg` CLI — any FFmpeg usage is **in‑process via libavcodec / libavformat** and isolated behind a plugin boundary.
9. Define a clean **decoded‑frame interface** so that future modules (hardware decoders, OpenCV background subtractors, DNN inference) can consume YUV/RGB frames without changing the worker or the bitstream path.

### 1.2 Non‑goals (v1)

- Pixel‑accurate motion detection, object detection, classification, or any ML inference shipped in v1. (An optional decode‑and‑verify path is defined as an extension point, but no DNN is shipped.)
- Two‑way audio, PTZ control, ONVIF discovery (later).
- Cloud upload of clips (later — local disk only, plus the MQTT notification is the integration seam for any uploader).
- **Required** hardware‑accelerated decode (NVDEC, VAAPI, V4L2 M2M). v1 does not require any HW decoder — the hot path never decodes. However the decoder backend is a **first‑class plugin interface** (§13) so NVDEC / VAAPI / V4L2 M2M backends can be added without touching the core, and an early stub for VAAPI / V4L2 is on the v1.1 roadmap.

---

## 2. Target platforms

| Tier | Architecture | Reference SKU                   | Notes |
|------|--------------|---------------------------------|-------|
| T1   | aarch64 (ARMv8.2+) | Ampere Altra / AWS Graviton    | Primary perf/scale target. NEON used for slow‑path SAD/checksum kernels. |
| T1   | aarch64 (ARMv8)   | Raspberry Pi 4/5, Rockchip RK3588 | Same binary; smaller channel count, NEON also present. |
| T2   | x86_64 (Haswell+) | Intel NUC / generic mini‑PC     | Same binary; SSE4.2/AVX2 used in slow‑path kernels via runtime dispatch. |

Operating system: Linux ≥ 5.10, glibc or musl. Boot environment: systemd unit. Container: distroless or `scratch` with static‑PIE binary.

---

## 3. High‑level architecture

```
                              ┌──────────────────────┐
                              │     Supervisor       │
                              │  (main thread, sd-   │
                              │  notify, hotreload)  │
                              └──────────┬───────────┘
                                         │ spawn / join
        ┌────────────────────────────────┼────────────────────────────────┐
        │                                │                                │
 ┌──────▼──────┐                  ┌──────▼──────┐                  ┌──────▼──────┐
 │ Cam Worker  │  …per camera…   │ Cam Worker  │                  │ Cam Worker  │
 │  (1 thread) │                  │             │                  │             │
 └──────┬──────┘                  └─────────────┘                  └─────────────┘
        │ RTSP/RTP read → depacketize → NAL → inspector → ring buffer
        │
        │  on Event ──► enqueue Event{cam_id, ts_pre, ts_post, reason}
        ▼
 ┌──────────────────────────────────────────────────────────────────────────────┐
 │                       Event Bus (lock‑free MPMC queue)                       │
 └────────────┬─────────────────────────────────────────┬──────────────────────┘
              │                                         │
       ┌──────▼──────┐                          ┌───────▼──────┐
       │  Recorder   │  N writer threads        │   Notifier   │  1 thread
       │  pool       │  (CPU count tunable)     │  (MQTT loop) │
       └─────────────┘                          └──────────────┘
              │                                         │
              ▼                                         ▼
     disk (MPEG-TS or fMP4)                         MQTT broker
```

Key invariants:

- **One reader thread per camera.** It owns the socket, the RTSP/RTP state machine, the depacketizer, the NAL inspector, and the ring buffer. It never blocks on disk I/O, never blocks on the MQTT socket, and never takes a contended lock with another camera worker.
- **Events leave the worker via an SPSC handoff** into a process‑wide MPMC event queue. The worker’s only blocking primitive on the hot path is the kernel read on its own RTSP socket.
- **Recorder and notifier are pools of worker threads** that consume from the event bus. They share read‑only access to the originating ring buffer via reference‑counted snapshots (see §6).

---

## 4. Process and threading model

### 4.1 Threads

| Thread / pool          | Count                          | Affinity hint                     | Role |
|------------------------|--------------------------------|-----------------------------------|------|
| Supervisor             | 1                              | none                              | Config, hotreload, supervision, exit |
| Camera worker          | `N_cameras`                    | pinned, distributed via libnuma / `sched_setaffinity` round‑robin across cores | RTSP read → inspect → buffer |
| Recorder pool          | `max(2, N_cameras / 4)`        | not pinned                        | Muxer (MPEG‑TS or fMP4) + atomic rename |
| Notifier               | 1                              | not pinned                        | MQTT keepalive + publish loop |
| Metrics                | 1                              | not pinned                        | Prometheus text endpoint |
| Watchdog               | 1                              | not pinned                        | per‑worker liveness, sd_notify WATCHDOG |

### 4.2 Concurrency primitives

- **Lock‑free SPSC ring** for the per‑camera bitstream buffer (single writer = worker, single reader = recorder at flush time, with a reference‑counted snapshot to avoid contention with writes during the post‑roll window).
- **Lock‑free MPMC queue** (`liblfds`‑style or hand‑rolled bounded queue, Apache 2.0 compatible) for the event bus.
- **No `pthread_mutex_lock` on the hot path.** Locks are permitted in supervisor/config code only.
- **Memory ordering:** C11 `_Atomic` with explicit `memory_order_acquire` / `memory_order_release`; relaxed only where justified in code comments.

### 4.3 Memory

- Per‑camera **arena allocator** sized at startup (`buffer_seconds × bitrate_ceiling`) to avoid heap fragmentation and runaway allocation. NAL units are stored as `(offset, length, flags, pts, dts)` records pointing into a contiguous backing byte ring.
- Zero‑copy from the depacketizer into the ring (the RTP payload pointer is copied directly; no per‑NAL malloc).
- `madvise(MADV_DONTFORK)` on backing rings; `mlock` optional via config for jitter‑sensitive deployments.

### 4.4 Backpressure & failure isolation

- If a camera worker’s ring fills (i.e., the recorder pool is wedged), the **worker drops oldest pre‑roll content first**, never blocks. A `frames_dropped_total{cam=…}` counter is incremented.
- If MQTT is down, the notifier queues up to `notifier.queue_max` events and drops oldest with a `notifications_dropped_total` counter; the recorder and the worker are unaffected.
- A crashing camera worker is restarted by the supervisor with exponential backoff; other cameras are unaffected.

---

## 5. Module decomposition

Each module is a separate compilation unit with a narrow public header, designed to be unit‑testable in isolation. All modules use plain C11; no C++ (keeps the ABI surface small and toolchain portable).

| Module              | Public header             | Responsibility                                                                 |
|---------------------|---------------------------|--------------------------------------------------------------------------------|
| `emd_config`        | `emd/config.h`            | Parse TOML config, validate, hotreload                                         |
| `emd_log`           | `emd/log.h`               | Structured JSON logs (line‑oriented), per‑subsystem levels                     |
| `emd_metrics`       | `emd/metrics.h`           | Prometheus text exposition, lock‑free counters/histograms                      |
| `emd_net`           | `emd/net.h`               | TCP/UDP sockets, non‑blocking, `epoll` wrapper, timeouts                       |
| `emd_rtsp`          | `emd/rtsp.h`              | RTSP client state machine (DESCRIBE/SETUP/PLAY/TEARDOWN, digest auth, RTCP)    |
| `emd_rtp`           | `emd/rtp.h`               | RTP header parse, jitter buffer, sequence reorder, loss accounting             |
| `emd_h264_depay`    | `emd/h264_depay.h`        | RFC 6184 depacketizer (STAP‑A, FU‑A, single NAL)                               |
| `emd_h265_depay`    | `emd/h265_depay.h`        | RFC 7798 depacketizer (AP, FU, single NAL)                                     |
| `emd_h264_parse`    | `emd/h264_parse.h`        | NAL/slice header parse, SPS/PPS cache, golomb decode                           |
| `emd_h265_parse`    | `emd/h265_parse.h`        | VPS/SPS/PPS cache, slice header, NAL type table                                |
| `emd_inspector`     | `emd/inspector.h`         | Encoded‑domain motion heuristics (see §7)                                      |
| `emd_ringbuf`       | `emd/ringbuf.h`           | Per‑camera bitstream ring buffer with snapshot API                              |
| `emd_event`         | `emd/event.h`             | Event types, debounce/hysteresis state machine, MPMC bus                       |
| `emd_recorder`      | `emd/recorder.h`          | Muxer dispatch (MPEG‑TS / fMP4 / plugin), atomic rename, fsync policy          |
| `emd_mqtt`          | `emd/mqtt.h`              | MQTT client front‑end (defaults to MQTT‑C backend; mosquitto via plugin), reconnect, LWT, QoS |
| `emd_supervisor`    | `emd/supervisor.h`        | Lifecycle, signals, sd_notify, hotreload orchestration                         |

### 5.1 Dependencies

All defaults are permissive (MIT/BSD/Apache). LGPL libraries (`libavcodec` / `libavformat`) are accepted **only via dynamic link** and only inside optional plugins, so a closed‑source distribution of `emd` remains legally clean.

#### 5.1.1 Always linked (core)

| Dep                | License     | Why we chose it                                                                                  |
|--------------------|-------------|--------------------------------------------------------------------------------------------------|
| `MQTT-C` (LiamBindle) | MIT      | Tiny (~3 kLOC), embedded‑friendly, byte‑accurate state machine, easy to integrate with our own epoll loop. Default MQTT client. |
| `tomlc99`          | MIT         | TOML 1.0 config parser.                                                                          |
| `cmocka` (test)    | Apache 2.0  | Unit test + mocking framework. Optional alt: Unity (MIT) for very small targets.                 |

No core dependency on FFmpeg, GStreamer, OpenCV or live555.

#### 5.1.2 Optional plugins (loaded at runtime by name)

| Plugin              | Link‑time dep                     | License posture                                  | Role |
|---------------------|-----------------------------------|--------------------------------------------------|------|
| `emd_mqtt_mosquitto`| `libmosquitto`                    | EPL/EDL (BSD‑compatible)                         | Drop‑in replacement for MQTT‑C when TLS / advanced ops matter |
| `emd_mux_libav`     | `libavformat` (LGPL, dyn‑linked)  | LGPL‑link OK                                     | MPEG‑TS muxer (default container) and fragmented MP4 muxer    |
| `emd_decoder_libav` | `libavcodec` (LGPL, dyn‑linked)   | LGPL‑link OK; LGPL FFmpeg build only — must build FFmpeg with `--disable-gpl --disable-nonfree` | In‑process H.264/H.265 decode for the verify path (§7.6); exposes motion vectors via `AV_CODEC_FLAG2_EXPORT_MVS` |
| `emd_bgsub_opencv`  | `opencv_core`, `opencv_bgsegm`    | Apache 2.0 (OpenCV ≥ 4.5)                        | Wraps `cv::bgsegm::BackgroundSubtractorCNT` for the verify path — chosen because CNT is the fastest BG subtractor in OpenCV on edge hardware (~2.3× faster than MOG2 on RPi3) |
| `emd_hwaccel_vaapi` | libva                             | MIT                                              | Future: x86 hardware decode for the verify path |
| `emd_hwaccel_v4l2`  | kernel headers                    | —                                                | Future: ARM SBC HW decode |

> Important: nothing in 5.1.2 is required for v1 acceptance. The core compiles and passes IT‑01…IT‑14 against the in‑tree MPEG‑TS muxer and the encoded‑domain inspector alone. The optional plugins exist so that customers who want them can opt in without rebuilding the core, and so we can iterate on the verify path independently.

#### 5.1.3 In‑tree (no third‑party link)

These are hand‑rolled in `src/` and unit‑tested directly. We carry the small amount of code rather than take a dependency:

- `emd_rtsp`, `emd_rtp`, `emd_h264_depay`, `emd_h265_depay` — see RFC 6184 / 7798.
- `emd_h264_parse`, `emd_h265_parse` — just enough syntax to extract the signals in §7.2. ~1.5–2 kLOC of C per codec.
- `emd_mux_mpegts` — minimal MPEG‑TS muxer covering AVC (`stream_type 0x1B`) and HEVC (`0x24`) PES/PSI tables. ~600 LOC. Sufficient to produce TS clips that play in VLC/ffplay/mpv. (The `emd_mux_libav` plugin can replace this for customers who want libavformat’s features.)
- `emd_mux_fmp4` — minimal fragmented MP4 muxer (`ftyp`, `moov`, `moof`+`mdat` per fragment). ~800 LOC.

### 5.2 Reference reading (no link‑time dep)

We deliberately read and learn from these projects but do not link them:

- **OpenCV `cap_ffmpeg_impl.hpp`** — canonical reference for driving libavformat to read RTSP and apply the `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream filters. Useful as a worked example for the optional `emd_decoder_libav` plugin.
- **FFmpeg `doc/examples/extract_mvs.c`** — canonical example of using `AV_CODEC_FLAG2_EXPORT_MVS` to walk per‑MB motion vectors out of a decoded frame. The verify path mirrors this.
- **OpenIPC `smolrtsp`** (MIT, C) — used by the integration test harness only (§17.5) as the engine of the simulated RTSP camera. Not linked into the production binary.
- **`h264bitstream` (aizvorski, LGPL)** — used as a *reference implementation* during fuzz‑corpus minimization and as a cross‑check oracle in unit tests; not statically linked.
- **`chemag/h264nal`, `h265nal`** (BSD) — additional cross‑check oracle for the in‑tree parser.

### 5.3 Library survey & rejections

| Considered                              | Verdict       | Reason                                                                              |
|-----------------------------------------|---------------|-------------------------------------------------------------------------------------|
| `kierank/libmpegts`                     | Rejected      | GPLv2+. Incompatible with permissive distribution.                                  |
| `Bento4`                                | Rejected      | Dual GPL / commercial. Incompatible with permissive open distribution.              |
| `live555`                               | Rejected      | LGPL with linking ambiguities for client use in commercial appliances; large API; not needed once we have smolrtsp for tests and our own RTSP client for production. |
| `GStreamer`                             | Rejected      | LGPL is fine, but the plugin/dep surface is far larger than we need and forces a process‑heavy architecture.    |
| `libavcodec` static link                | Rejected      | LGPL static linking imposes relinkability obligations that conflict with a static‑PIE shipping model. Dynamic link inside an optional `.so` plugin is fine.|
| `aizvorski/h264bitstream` as runtime dep| Rejected as dep, kept as reference | LGPL link adds little over our in‑tree parser; we keep it as an oracle in tests only. |
| `uvgRTP`                                | Rejected      | BSD‑2; great library, but optimised for senders / SRTP, not RTSP client.            |
| Shelling out to `ffmpeg` CLI            | Rejected (per requirements) | Forks per camera, hard to bound CPU, hard to instrument, latency floor too high. All FFmpeg usage is in‑process via libavcodec/libavformat. |

---

## 6. Per‑camera bitstream ring buffer

### 6.1 Sizing

Configured per camera; the first form wins:

```toml
[cameras.driveway]
buffer_seconds = 10        # mandatory unless buffer_frames is set
buffer_frames  = 300       # alternative; overrides seconds if both present
pre_roll_seconds  = 6
post_roll_seconds = 8
```

The backing arena is provisioned at startup as
`size_bytes = buffer_seconds × max_bitrate_bps / 8 × 1.25` (the 1.25 factor absorbs short bursts). `max_bitrate_bps` is taken from config or, if absent, conservatively defaults to 8 Mbit/s for 1080p H.264.

### 6.2 Record layout

Each entry in the index ring describes one NAL unit:

```c
typedef struct {
    uint64_t pts_90khz;     // from RTP timestamp, lifted to 64‑bit
    uint64_t mono_ns;       // CLOCK_MONOTONIC at receipt
    uint32_t offset;        // into backing byte ring
    uint32_t length;
    uint8_t  nal_type;      // codec‑specific
    uint8_t  flags;         // EMD_NAL_KEYFRAME, EMD_NAL_PARAMSET, ...
    uint16_t cam_id;
} emd_nal_record_t;
```

### 6.3 Snapshot semantics

`emd_ringbuf_snapshot(rb, from_pts, to_pts, &snap)` produces a refcounted, copy‑on‑write window. Concurrent writes from the worker continue to advance the head; the snapshot pins the tail until released. The recorder always emits an **independently decodable** clip: the snapshot is widened backwards to the most recent SPS/PPS (or VPS/SPS/PPS for HEVC) and IDR/CRA so that the resulting file plays standalone.

### 6.4 Pre/post‑roll mechanics

On `event_detected_at = T`:
1. Recorder requests `snapshot(T − pre_roll, T)` immediately.
2. Worker continues writing; the snapshot index records the head position at `T`.
3. The recorder schedules a deferred close at `T + post_roll`; at that moment it extends the snapshot to `(T − pre_roll, T + post_roll)` and finalizes the file.
4. If a second event fires inside the post‑roll, the recorder **extends** the same clip (no overlapping clips) and resets the close deadline to `T2 + post_roll`. Coalescence is bounded by `clip_max_seconds` to prevent runaway.

---

## 7. Bitstream inspection and motion detection (the hot path)

### 7.1 Parse depth

We do **not** decode the residual. We parse:

- NAL header bytes (1 for AVC, 2 for HEVC).
- SPS/PPS (+ VPS for HEVC), once per stream and on every parameter‑set refresh, to know picture size, MB/CTU layout, and `entropy_coding_mode_flag`.
- Slice header up to and including `slice_type`, `frame_num`/`PicOrderCntLsb`, and (for AVC) `mb_skip_run` / `mb_skip_flag` when parseable without CABAC decode.

### 7.2 Signals collected per frame

| Signal                  | H.264 source                                              | H.265 source                                           | Cost |
|-------------------------|-----------------------------------------------------------|--------------------------------------------------------|------|
| `kf`                    | NAL type 5 (IDR) or recovery point SEI                    | NAL types 19/20 (IDR) or 21 (CRA) or recovery SEI       | O(1) |
| `slice_type`            | slice header                                              | slice header                                           | O(1) |
| `bytes`                 | NAL length                                                | NAL length                                             | O(1) |
| `qp_delta_proxy`        | `pic_init_qp_minus26` + slice `slice_qp_delta`            | equivalent                                             | O(1) |
| `intra_ratio_proxy`     | for non‑IDR slices, ratio of bytes per macroblock vs running median (large = many intra MBs) | same, per CTU | O(1) per slice |
| `mv_energy_proxy`       | running EWMA of P‑slice byte rate / kf byte rate          | same                                                   | O(1) |
| `mb_skip_run_proxy`     | when CAVLC, parse leading `mb_skip_run` Golomb codes      | n/a (HEVC uses skip flag in CTU; not parsed)            | O(1) |

For CABAC streams we deliberately stop at the slice header — entropy decoding is too expensive for the hot path. The byte‑per‑MB proxy is intentionally crude but is sufficient for motion gating; the **decode‑on‑suspect** path (§7.5) handles ambiguous cases.

### 7.3 Per‑camera state

```c
typedef struct {
    double bpf_ewma;        // bytes per frame, fast (α=0.2)
    double bpf_slow;        // bytes per frame, slow (α=0.005) — baseline
    double bpf_var;         // welford variance, fast
    uint32_t since_kf;      // frames since last IDR/CRA
    uint8_t  consecutive_above; // for debounce
    uint8_t  consecutive_below;
} emd_inspector_state_t;
```

### 7.4 Detection rule (motion)

For each completed access unit:

```
z = (bytes_this_frame - bpf_slow) / max(sqrt(bpf_var), bpf_floor)

is_motion =
       (z > motion_z_high)                              // sudden burst
    OR (since_kf == 0 AND not configured_periodic_kf)   // unexpected IDR
    OR (intra_ratio_proxy > intra_ratio_high)
```

State machine (hysteresis):

```
IDLE  --(consecutive_above ≥ on_threshold)-->  ACTIVE
ACTIVE -(consecutive_below ≥ off_threshold)--> COOLDOWN
COOLDOWN --(post_roll elapsed)--> IDLE
```

Defaults (tunable per camera): `motion_z_high=3.0`, `intra_ratio_high=2.5`, `on_threshold=2 frames`, `off_threshold=int(fps × 1.5)`.

### 7.5 Gradual scene change

Two EWMAs of `bpf` at very different time constants. A sustained divergence between fast and very‑slow baselines (e.g. `|bpf_slow − bpf_vslow| / bpf_vslow > 0.4` for `gradual_window_seconds`) raises a `SCENE_CHANGE` event with `reason="gradual"`. This catches dawn/dusk, cloud cover, lights turning on, fog, lens contamination.

### 7.6 Decode‑on‑suspect (optional, off by default)

When `inspector.verify_decode = true`, a flagged access unit is enqueued to a low‑priority decode thread inside the `emd_decoder_libav` plugin. The verify pipeline has three stages, each independently switchable, in increasing cost order:

1. **Motion‑vector verify (preferred).** The decoder is opened with `AV_CODEC_FLAG2_EXPORT_MVS`. After a successful decode we walk `AV_FRAME_DATA_MOTION_VECTORS` side data and compute a single statistic per frame: `mv_energy = sum(|dx| + |dy|)` over all P/B MBs (CTUs for HEVC). This is *real* motion, not a byte‑rate proxy. It also discriminates camera shake (uniform global MV) from object motion (clustered local MV) via simple histogram dispersion. We keep only the scalar; the frame pixels can be discarded.
2. **Pixel verify with CNT background subtractor (optional).** When `inspector.bgsub = "cnt"`, the decoded luma plane is passed through `emd_bgsub_opencv` wrapping `cv::bgsegm::BackgroundSubtractorCNT`. CNT is the OpenCV BG subtractor with the best throughput on edge hardware (≈2.3× faster than MOG2 on a Raspberry Pi 3 in published benchmarks) and is a drop‑in replacement should we want MOG2/KNN later. Only the foreground pixel‑count ratio crosses the plugin boundary back into the inspector.
3. **Frame‑hash drift (cheapest).** When `inspector.bgsub = "phash"`, we compute a perceptual hash of a downsampled luma plane and compare against an EWMA reference. Catches very slow lens / scene drift even when MV energy is zero.

In all three cases the alert path remains non‑blocking: the verify thread can only **suppress** an already‑raised preliminary event (false‑positive reduction) or **confirm** it; it never gates initial event emission, which still fires from §7.4 on the encoded‑domain signal. Latency added to the verify confirmation: ≤ 2 frame intervals P99 on T1.

The `emd_decoder_libav` plugin must be linked against an **LGPL FFmpeg build** (`--disable-gpl --disable-nonfree`). CI verifies the FFmpeg `LICENSE.md` shipped in the binary artifact contains no GPL strings.

### 7.7 What we explicitly do not do on the hot path

- No CABAC decode.
- No inverse transform.
- No motion vector reconstruction beyond the byte‑rate proxy.
- No pixel‑domain processing.

This keeps per‑frame inspector cost in the low‑µs range on T1 hardware and lets a single Ampere core comfortably handle 30+ 1080p30 streams.

### 7.8 Decoded‑frame interface (forward seam)

To keep open the option of operating on decoded frames in future, the spec defines a stable C interface that any decoder backend implements:

```c
typedef enum { EMD_PIXFMT_YUV420P, EMD_PIXFMT_NV12, EMD_PIXFMT_RGB24 } emd_pixfmt_t;

typedef struct emd_frame {
    uint16_t  cam_id;
    uint64_t  pts_90khz;
    uint64_t  mono_ns;
    uint16_t  width, height;
    emd_pixfmt_t pixfmt;
    uint8_t  *plane[4];
    int       linesize[4];
    void    (*release)(struct emd_frame *self);   // refcount drop
    void     *backend_handle;                     // opaque to consumers
} emd_frame_t;

typedef struct emd_decoder_backend {
    int (*open)(void *cfg, void **state);
    int (*submit_nal)(void *state, const uint8_t *nal, size_t len, uint64_t pts);
    int (*next_frame)(void *state, emd_frame_t **out);   // EAGAIN if none
    int (*close)(void *state);
} emd_decoder_backend_t;
```

This interface is the single seam crossed by every potential future consumer of decoded pixels (HW decoders, OpenCV BG subtractor, DNN inference). Importantly, the worker never holds a frame past the inspector loop body: ownership transfers to the verify queue and is dropped by the verify thread, so a slow consumer cannot stall ingest.

---

## 8. RTSP & RTP details

### 8.1 RTSP client

- Methods: `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY`, `PAUSE`, `TEARDOWN`, `GET_PARAMETER` (keepalive).
- Auth: Basic + Digest (MD5, MD5‑sess). No SHA‑256 digest in v1 (rare on cameras).
- Transport selection: prefer `RTP/AVP/TCP` (interleaved) by default — survives middleboxes and avoids UDP loss tuning per device. UDP fall‑back when configured.
- Keepalive via `GET_PARAMETER` every `session_timeout / 2`.
- Reconnect with exponential backoff `[1, 2, 4, 8, 16, 30]` seconds, capped.

### 8.2 RTP

- Sequence number reorder window of 32; out‑of‑order packets accepted within window.
- Loss is detected and counted; a lost packet inside a fragmented NAL invalidates that NAL only (recorded as a synthetic “lost” event with bytes=0 so the inspector statistics don’t skew).
- RTCP SR/RR sent at standard intervals; SR’s NTP↔RTP mapping is used to lift the 90 kHz RTP timestamp to wall‑clock for clip naming and MQTT.

### 8.3 Depacketization

- AVC (RFC 6184): single NAL, STAP‑A, FU‑A. STAP‑B / MTAP not required (rarely used by cameras); rejected with a counter.
- HEVC (RFC 7798): single NAL, AP, FU. DON/DONL fields parsed when `sprop-max-don-diff > 0`.

---

## 9. Recording: container choice

Container is configurable per camera (`recording.container = "mpegts" | "fmp4"`). Default is `mpegts` because it is the cheapest format to write correctly from a stream of pre‑existing NAL units — append‑only, no fragment bookkeeping, no `moov` derivation, no late metadata to fix up. Both formats are produced by in‑tree muxers (`emd_mux_mpegts`, `emd_mux_fmp4`); the optional `emd_mux_libav` plugin (LGPL link, libavformat) offers a feature‑complete alternative for either format.

### 9.1 MPEG‑TS (default)

- `.ts` extension. 188‑byte packets, PAT/PMT every ~100 ms, PCR every ~40 ms.
- Stream types: `0x1B` (AVC) or `0x24` (HEVC). PES‑packetized access units with PTS/DTS lifted from RTP timestamps.
- Trivial to append to: a clip extension during an event coalescing window (§6.4) is a pure byte append; no fragment metadata to rewrite.
- Robust to truncation: a corrupt or partial `.ts` still plays from the next intact packet boundary, which matters for power‑loss / disk‑full edge cases (§ IT‑10).
- Trade‑off: ~5 % container overhead vs raw bitstream, slightly more than fMP4.

### 9.2 Fragmented MP4

- `.mp4` extension, `ftyp` + `moov` (init segment) followed by `moof`+`mdat` per fragment.
- Codec mapping: `avc1` for H.264, `hvc1` for H.265. SPS/PPS (+ VPS for HEVC) stored in `avcC` / `hvcC` boxes derived from the cached parameter sets valid at the snapshot tail — clip plays standalone even if the camera changed parameter sets later.
- Better browser compatibility (Media Source Extensions can consume fragments directly) and slightly lower overhead.
- Trade‑off: extension during coalescing requires writing a new `moof`+`mdat`; truncation mid‑fragment yields an unplayable tail.

### 9.3 Common machinery

- Atomic completion: write to `…/inflight/{cam}/{ts}.{nonce}.{ext}.part`, `fsync`, `rename` to `…/clips/{cam}/{YYYY}/{MM}/{DD}/{ts}-{reason}.{ext}`. Notifier publishes only after the rename succeeds.
- Disk hygiene: a janitor thread enforces `disk.max_bytes_per_camera` and `disk.retention_days`; oldest clips evicted first.
- A clip header (`emd_clip_header_t`) is written as a sidecar `.json` for machine consumers, even though the same fields appear in the MQTT `clip` payload (§10.4).

---

## 10. MQTT contract

### 10.1 Connection

- MQTT 3.1.1 minimum, 5.0 preferred. TLS (cert‑pinned optional).
- Persistent session (`clean_start=false`).
- LWT on `emd/{instance_id}/status` with payload `{"online":false,"reason":"lwt"}` retained.
- On connect, publish retained `{"online":true,...}` on the same topic.

### 10.2 Topics (default; configurable prefix)

| Topic                                                    | Direction | QoS | Retained | Payload |
|----------------------------------------------------------|-----------|-----|----------|---------|
| `emd/{instance}/status`                                  | pub       | 1   | yes      | health  |
| `emd/{instance}/cameras/{cam_id}/event`                  | pub       | 1   | no       | event   |
| `emd/{instance}/cameras/{cam_id}/clip`                   | pub       | 1   | no       | clip    |
| `emd/{instance}/cameras/{cam_id}/stats`                  | pub       | 0   | no       | stats   |
| `emd/{instance}/cmd/+`                                   | sub       | 1   | —        | command |

### 10.3 Event payload

```json
{
  "v": 1,
  "instance": "emd-edge-01",
  "cam_id": "driveway",
  "event_id": "01HXY3Q…",
  "type": "motion",
  "reason": "z=4.7,intra_ratio=3.1",
  "started_at": "2026-05-16T14:22:09.413Z",
  "started_pts_90khz": 4123456789,
  "fps_estimate": 14.97,
  "codec": "h264"
}
```

### 10.4 Clip payload (sent after the file is closed and renamed)

```json
{
  "v": 1,
  "event_id": "01HXY3Q…",
  "cam_id": "driveway",
  "container": "mpegts",
  "codec": "h264",
  "path": "/var/lib/emd/clips/driveway/2026/05/16/142209-motion.ts",
  "uri": "file:///var/lib/emd/clips/driveway/2026/05/16/142209-motion.ts",
  "size_bytes": 4831234,
  "duration_ms": 14000,
  "pre_roll_ms": 6000,
  "post_roll_ms": 8000,
  "sha256": "…"
}
```

### 10.5 Commands subscribed

- `cmd/snapshot` → force an event on a camera (testing).
- `cmd/reload`   → re‑read config.
- `cmd/healthz`  → publish a one‑shot status payload.

---

## 11. Configuration

`/etc/emd/emd.toml` (TOML 1.0). Hotreload on SIGHUP and on `cmd/reload`.

```toml
[runtime]
log_level        = "info"
metrics_listen   = "0.0.0.0:9464"
worker_affinity  = "round_robin"   # round_robin | none | numa
clip_root        = "/var/lib/emd/clips"
inflight_root    = "/var/lib/emd/inflight"

[recording]
container        = "mpegts"        # mpegts | fmp4
muxer            = "intree"        # intree | libav
fsync_policy     = "on_close"      # on_close | always | never

[plugins]
mqtt             = "mqttc"         # mqttc | mosquitto
decoder          = "off"           # off | libav | vaapi | v4l2
bgsub            = "off"           # off | cnt | mog2 | phash

[mqtt]
url              = "mqtts://broker.local:8883"
client_id_prefix = "emd"
qos              = 1
tls_ca_file      = "/etc/emd/ca.pem"

[disk]
max_bytes_per_camera = 20_000_000_000  # 20 GB
retention_days       = 14

[cameras.driveway]
url               = "rtsp://user:pass@10.0.1.51:554/Streaming/Channels/101"
transport         = "tcp"            # tcp | udp
codec_hint        = "auto"           # auto | h264 | h265
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

All values are validated at parse time (range checks, URL parse, file existence). Invalid config → fatal startup error with a precise `key=…` message; hotreload errors leave the previous config in place.

---

## 12. Observability

### 12.1 Metrics (Prometheus text on `metrics_listen`)

- `emd_rtsp_state{cam,state}` (gauge)
- `emd_nal_received_total{cam,nal_type}` (counter)
- `emd_frames_dropped_total{cam,reason}` (counter)
- `emd_inspector_bpf_ewma{cam}` (gauge)
- `emd_event_total{cam,type,reason}` (counter)
- `emd_recording_seconds{cam}` (histogram)
- `emd_recorder_queue_depth` (gauge)
- `emd_mqtt_publish_total{result}` (counter)
- `emd_mqtt_connected` (gauge)
- `emd_worker_loop_latency_seconds{cam}` (histogram)
- `emd_build_info{version,commit,codec_set}` (gauge=1)

### 12.2 Logs

Line‑oriented JSON to stderr. Required fields: `ts`, `level`, `cam_id` (where applicable), `event_id` (when in an event context), `subsystem`, `msg`. Sampling for hot‑path debug logs (`log_level=debug` capped at 10 lines/sec/camera).

### 12.3 Health

- `sd_notify(WATCHDOG=1)` from the watchdog every `WatchdogSec/2`.
- HTTP `/healthz` (liveness) and `/readyz` (every camera connected ≥ once) on the metrics port.

---

## 13. Extension points

All extensions are separately compiled shared objects loaded by name from config; the core never depends on these symbols at link time. Each extension declares a small C ABI struct in a versioned header so plugins can be swapped at runtime without recompiling `emd`.

| Extension slot       | Interface                          | Example backends                                                                 |
|----------------------|------------------------------------|----------------------------------------------------------------------------------|
| `emd_mqtt_*`         | `emd_mqtt_backend_t`               | `mqttc` (default, MIT), `mosquitto` (EPL/EDL)                                    |
| `emd_mux_*`          | `emd_mux_backend_t`                | `intree_mpegts`, `intree_fmp4`, `libav` (libavformat, LGPL dyn‑link)             |
| `emd_decoder_*`      | `emd_decoder_backend_t` (§7.8)     | `libav` (libavcodec, LGPL dyn‑link), `vaapi` (future), `v4l2_m2m` (future), `nvdec` (future) |
| `emd_verify_*`       | `emd_verify_backend_t`             | `mv` (libavcodec motion vectors), `cnt` (OpenCV BackgroundSubtractorCNT), `phash`|
| `emd_uploader_*`     | `emd_uploader_backend_t`           | s3, gcs, r2 — consume from `cameras/+/clip`                                      |
| `emd_inference_*`    | out‑of‑process gRPC over `clip` topic | external classifier; hard out‑of‑process boundary by design                   |

Plugins ship as `.so` files under `/usr/lib/emd/plugins/` and are validated against an SBOM at load time.

---

## 14. Build, packaging, supply chain

- Build system: **CMake ≥ 3.22**, Ninja generator. Out‑of‑tree builds only.
- Compilers: GCC 12+ and Clang 16+. Both must build clean with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror`.
- Runtime: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection=full` (x86) / `-mbranch-protection=standard` (arm64).
- LTO + PGO supported (`-flto=thin`, `-fprofile-generate/-use`).
- Output: **static‑PIE** binary plus a thin loader script. Container image is `scratch + emd` plus CA bundle (~3–5 MB).
- Dependencies are vendored under `third_party/` with pinned versions; reproducible builds via locked toolchain (e.g. `zig cc` cross or pinned Ubuntu container).
- SBOM (CycloneDX) emitted by CI.
- Cross‑compile matrix: native x86_64; aarch64 via dedicated toolchain image.

---

## 15. Security

- Drops privileges to `emd:emd` after binding metrics port.
- `seccomp-bpf` allowlist (no `ptrace`, no `clone3` for new namespaces beyond startup, no `bpf`).
- `CAP_NET_BIND_SERVICE` only if metrics port < 1024.
- Camera credentials read from `${EMD_CREDENTIALS}` file (`mode 0600`) or systemd `LoadCredential=`; never echoed to logs.
- MQTT broker auth via TLS client cert preferred; password fallback supported.
- Strict input validation on every parsed bitstream field. Bounds checks asserted in debug, returned as `EMD_ERR_BITSTREAM` in release. **No `gets`, no `strcpy`, no `sprintf`** — banned via grep in CI.

---

## 16. Performance budgets (T1 reference: Ampere Altra, 1 core, no SMT)

| Workload                                  | Budget                | Notes |
|-------------------------------------------|-----------------------|-------|
| Inspect one 1080p30 H.264 access unit     | ≤ 8 µs P99            | parsing only, no decode |
| Inspect one 4K H.265 access unit          | ≤ 25 µs P99           |       |
| End‑to‑end ingest→event (encoded‑domain)  | ≤ 1 frame interval P99|       |
| Memory per 1080p10 Mbit camera, 10 s buffer | ≤ 16 MiB RSS overhead | |
| Cold startup to first frame buffered      | ≤ 1.5 s per camera    | |
| 30 cameras @ 1080p30 H.264, 1 core        | < 60 % CPU            | sustained |

Benchmarks under `bench/` are part of CI; regressions > 10 % fail the build.

---

## 17. Test strategy (TDD)

The project is developed test‑first. Every public function lands with its unit tests in the same PR; the test must be authored first and demonstrated failing before the implementation. CI enforces a coverage floor (line ≥ 80 %, branch ≥ 70 %) on changed files; the bitstream parsers must hit 95 % line coverage given their security exposure.

### 17.1 Tooling

- **cmocka** for unit tests + mocking (Apache 2.0).
- **libFuzzer** (Clang) for fuzz targets; corpora persisted under `tests/fuzz/corpus/`.
- **ASan, UBSan, TSan, MSan**: CI runs each suite under each sanitizer.
- **Valgrind** memcheck run nightly on aarch64 under QEMU.
- **clang‑tidy** + **cppcheck** in CI; `-Werror=…` for selected lints.
- **gcovr** for coverage reports.
- **hyperfine** + custom JSON harness for perf regression tracking.

### 17.2 Unit tests (illustrative, non‑exhaustive)

- `test_h264_parse`: SPS/PPS round‑trip across 30 SPS fixtures (Hikvision, Dahua, Reolink, Axis, generic ffmpeg). Slice‑header parsing for every `slice_type` and `profile_idc` we support.
- `test_h264_depay`: STAP‑A multi‑NAL, FU‑A reassembly across packet loss, sequence wrap.
- `test_h265_parse`: VPS/SPS/PPS, NAL type table including CRA/BLA/RASL/RADL.
- `test_h265_depay`: AP, FU, DONL handling.
- `test_inspector_motion`: deterministic event raises on synthetic byte‑rate traces. Includes table‑driven hysteresis cases.
- `test_inspector_gradual`: dawn‑simulation trace produces exactly one `SCENE_CHANGE` event.
- `test_ringbuf`: SPSC writer + snapshot reader under concurrent stress (TSan).
- `test_event_bus`: MPMC queue invariants under TSan; drop policy.
- `test_recorder_mp4`: bit‑for‑bit golden fMP4 outputs for fixed seeds; clip plays in `ffprobe` and reports correct duration and codec.
- `test_mqtt`: reconnect, LWT, ordering of clip vs event publishes.
- `test_config`: every error message tested against an invalid TOML fixture.

### 17.3 Golden bitstream fixtures

Stored under `tests/fixtures/streams/`:

- `h264_1080p30_static.h264` — synthetic, no motion, periodic IDR (10 s).
- `h264_1080p30_motion.h264` — synthetic, three motion bursts at known offsets.
- `h264_1080p30_gradual.h264` — slow byte‑rate drift mimicking dawn.
- `h265_2160p30_*` — HEVC counterparts.
- `vendor_*.h264` — recorded clips from real cameras (with vendor’s permission), checked into Git LFS.

Each fixture has a sidecar `*.expected.json` describing the events the inspector must produce. The unit test asserts an exact match (event count, timestamps within ±1 frame, reasons).

### 17.4 Fuzz targets

- `fuzz_h264_nal` — input: arbitrary bytes interpreted as NAL units. Asserts: no UB, no OOB, no infinite loop.
- `fuzz_h265_nal`.
- `fuzz_rtsp_response` — input: raw RTSP response bytes. Asserts: parser either accepts or returns error; never crashes.
- `fuzz_rtp_packet`.
- `fuzz_mp4_writer` — input: NAL sequences; output must parse with `mp4dump`.

CI runs each target for 60 s on every PR and 30 min nightly with corpus minimization.

### 17.5 Integration test harness

The integration suite stands the whole binary up against simulated peers, in‑process:

- **`emd-fake-rtsp`** — a self‑contained RTSP server in `tests/integration/fake_rtsp/`, built on top of **OpenIPC `smolrtsp`** (MIT, C, designed for embedded use; we already have it on disk for this purpose). Test‑only dependency, not linked into the production binary. Capabilities:
  - Serves a chosen golden bitstream over TCP‑interleaved RTP at a configured RTP timestamp clock.
  - Supports `DESCRIBE/SETUP/PLAY/TEARDOWN`, digest auth, `GET_PARAMETER` keepalive.
  - Can inject faults: packet loss (drop every Nth), reorder window, mid‑session SPS change, parameter set loss, mid‑session FU‑A truncation, TCP RST after T seconds, slow‑loris (delay between bytes).
  - Deterministic clock: optional logical clock driven by the test harness so the test isn’t real‑time bound.
- **`emd-fake-mqtt`** — minimal MQTT 3.1.1/5 broker (single client, in‑process). Records every publish in arrival order with timestamps; exposes a query API to the test.
- Fault‑injection knobs are exposed as TOML to the test for reproducibility.

The integration suite is implemented in C using cmocka with helper utilities in Python (only for orchestration: bring up `emd` as a child process, point it at the fakes, assert on the captured publishes and on the clip files on disk).

### 17.6 Integration test scenarios (must pass for release)

| ID  | Scenario                                                                                       | Pass criteria |
|-----|------------------------------------------------------------------------------------------------|---------------|
| IT‑01 | Single H.264 camera, no motion, 60 s                                                         | Zero `motion` events; one retained `online=true` status; no clips written |
| IT‑02 | Single H.264 camera, motion burst at t=20 s                                                  | Exactly one `motion` event with `started_at` within ±1 frame; one clip on disk covering `[14, 28]` s (±200 ms) |
| IT‑03 | Single H.265 camera, motion burst                                                            | Same as IT‑02 with codec=h265 in payload |
| IT‑04 | Two motion bursts within post‑roll window                                                    | One clip (coalesced), one event; clip duration extended; second event suppressed by hysteresis |
| IT‑05 | Gradual scene change over 90 s                                                                | Exactly one `motion` event with `reason="gradual"` inside ±5 s of injection mid‑point |
| IT‑06 | 16 cameras in parallel, mixed codecs, mixed motion                                            | All expected events emitted; no event_id duplication; per‑camera latency P99 ≤ 1 frame |
| IT‑07 | MQTT broker offline for 30 s during an event                                                  | Event eventually published after reconnect; clip already on disk; no worker stalls; `mqtt_publish_total{result=retry}` increments |
| IT‑08 | RTSP server kills TCP mid‑NAL                                                                 | Worker reconnects with backoff; no crash; the in‑flight clip (if any) is either complete or absent — never partial |
| IT‑09 | Packet loss 2 % UDP                                                                           | No false events; `frames_dropped_total{reason="loss"}` reflects actual loss |
| IT‑10 | Disk full mid‑clip                                                                            | Current clip is rolled back (deleted from inflight); error event published on `status`; ingest continues |
| IT‑11 | Hotreload changes a camera URL                                                                | Old worker exits cleanly within 5 s; new worker connects to new URL; no event loss for unaffected cameras |
| IT‑12 | 24 h soak under sanitizers                                                                    | No leaks (ASan), no data races (TSan), no UB (UBSan), bounded RSS |
| IT‑13 | RTSP digest auth                                                                              | Successful PLAY against fake server requiring digest |
| IT‑14 | Mid‑session SPS/PPS change                                                                    | Clip taken after change contains the updated parameter sets in its `moov` and plays back in ffprobe |
| IT‑15 | Container = `mpegts`, motion burst                                                            | Output `.ts` plays in ffplay; ffprobe reports correct duration and codec; byte‑level append during coalescing leaves a valid TS |
| IT‑16 | Verify path enabled (`decoder=libav`, `bgsub=cnt`)                                            | Encoded‑domain false positive (forced) is suppressed by the verify thread; true positives still confirmed within 2 frame intervals |
| IT‑17 | `emd_decoder_libav` plugin absent at runtime                                                  | `emd` starts cleanly with the encoded‑domain path only; metrics surface `plugin_missing{name="emd_decoder_libav"}`; no crash |
| IT‑18 | License audit                                                                                 | The shipped binary plus loaded plugins contain zero GPL strings; CI step fails the build otherwise |

### 17.7 CI matrix

| Axis              | Values                                       |
|-------------------|----------------------------------------------|
| OS                | Ubuntu 22.04, Alpine 3.19 (musl)             |
| Arch              | x86_64 native, aarch64 native (self‑hosted) and aarch64 under QEMU user mode |
| Compiler          | GCC 13, Clang 17                             |
| Build type        | Debug, Release, RelWithDebInfo               |
| Sanitizers        | none, ASan+UBSan, TSan, MSan (Clang only)    |

Required green: every cell of (OS × Arch × Compiler × Release). Sanitizer cells must be green on `main` nightly.

### 17.8 Acceptance criteria (release gate)

Release `1.0.0` requires:

1. All unit, fuzz (60 s), and integration scenarios IT‑01…IT‑14 pass on every CI cell.
2. 24 h soak (IT‑12) clean under ASan and TSan on aarch64.
3. Performance budgets (§16) met on the Ampere reference.
4. SBOM emitted; no GPL or unknown‑license dependencies present.
5. `clang‑tidy` clean (selected checks), `cppcheck` no warnings at `--enable=warning,performance,portability`.
6. Documentation: this spec, a `README`, a one‑page `OPERATIONS.md`, and `man emd(8)` generated from sources.

---

## 18. Requirement‑to‑spec traceability

| Requirement (from brief)                                                              | Where addressed |
|---------------------------------------------------------------------------------------|-----------------|
| Very high performance C application on edge devices                                   | §2, §4, §16     |
| Primarily ARM, also x86                                                               | §2, §14         |
| Pulls video over RTSP                                                                  | §5, §8          |
| H.264 and H.265                                                                        | §5 (`emd_h26x_*`), §7, §8.3 |
| Pull from multiple devices without blocking any other                                  | §3, §4 (per‑camera worker, lock‑free) |
| Multi‑processor / multi‑core / multi‑thread                                            | §4.1, §4.2, §4.3 |
| Per‑feed buffer sized in seconds **or** frames                                         | §6.1            |
| Inspect the native video stream on each buffer                                         | §7              |
| Detect motion via H.264 macroblock changes / new keyframes without decoding all frames | §7.1, §7.2, §7.4 |
| Gradual scene change                                                                   | §7.5            |
| Non‑blocking write of buffer to disk on change                                         | §4.4, §6.3, §9  |
| Trigger MQTT notification                                                              | §10             |
| Clip contains pre‑change buffer and post‑change buffer                                 | §6.4, §9        |
| libavcodec OK; no shelling out to `ffmpeg` CLI                                         | §1.1 goal 8, §5.1.2, §5.3, §7.6 |
| Small / lightweight                                                                    | §5.1 (core has 3 deps), §5.3 (rejections), §14 (static‑PIE ~3–5 MB) |
| fMP4 acceptable, MPEG‑TS also acceptable / lower effort                                | §1.1 goal 6, §9.1, §9.2, IT‑15 |
| Keep door open for HW decoders / decoded‑frame buffer processing                       | §1.2 (non‑goal softened), §7.6, §7.8, §13 |
| Reuse OpenCV components where useful                                                    | §5.1.2 `emd_bgsub_opencv`, §5.2, §7.6 |
| TDD‑based development                                                                  | §17, §17.2      |
| Full integration suite with simulated RTSP and simulated MQTT                          | §17.5, §17.6    |

---

## 19. Open questions / future work

1. Hardware decoder backend (NVDEC/VAAPI) for the optional verify path.
2. ONVIF discovery and PTZ; would add a separate `emd_onvif` module.
3. Per‑zone motion masks (would require a partial decode path on suspect frames).
4. WebRTC egress for live preview by an operator UI.
5. End‑to‑end encryption of clips at rest.
6. Outbound clip uploader as an `emd_uploader_*` plugin (S3, GCS, R2).
