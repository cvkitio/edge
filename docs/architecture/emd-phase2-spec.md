# Edge Motion Detector — Phase 2 Technical Specification

**Codename:** `emd-agent` (Go outer) wrapping `libemd` (Phase 1 C core)
**Document status:** Draft v0.1
**Owner:** Andrew Sinclair
**Date:** 2026-05-16
**Audience:** Implementers, reviewers, QA
**Prerequisite:** `edge-motion-detector-spec.md` v0.1 (Phase 1) — release `1.0.0`

---

## 1. Purpose and relationship to Phase 1

Phase 1 delivered a self-contained C binary (`emd`) that ingests RTSP, performs encoded-domain motion detection, records pre/post-roll clips, and publishes MQTT notifications. Phase 2 splits the same functional surface across two artifacts:

1. **`libemd`** — the C code from Phase 1, refactored from an executable into a shared/static library. The hot path (RTSP, RTP, depay, parse, inspector, ringbuf) and the muxers (MPEG-TS, fMP4) remain unchanged in behaviour. The supervisor, MQTT client, file lifecycle, metrics endpoint, and config parser leave the C tree.
2. **`emd-agent`** — a new Go binary that supervises one camera worker per feed via cgo into `libemd`, owns all I/O outside the bitstream pipeline (NATS/MQTT publish, S3 upload, control plane, outbox, retention, observability), and exposes the Phase 1 wire contract (MQTT topics + payloads in §10 of Phase 1) augmented with NATS-native subjects.

This is the architecture that came out of the design discussion: keep the well-tested, microsecond-budget C code where C is unambiguously the right tool, and move the boring 70% of an edge agent — the parts that decide whether the system is reliable in production — into Go, where the standard library, ecosystem, and concurrency model collapse the line count and the bug surface.

### 1.1 Goals (Phase 2)

1. Repackage the Phase 1 C core as `libemd` with a stable, versioned C ABI (§3) that hides the existing internals (`emd_ringbuf_t`, `emd_recorder_pool_t`, etc.) behind a small set of camera-scoped handles.
2. Implement `emd-agent` in Go (≥ 1.22) that:
   - Spawns and supervises one OS-locked goroutine per camera, each running `libemd`'s blocking ingest loop.
   - Replaces `emd_mqtt`, `emd_supervisor`, the metrics HTTP endpoint, and the recorder pool's event-driven scheduling with native Go.
   - Adds capabilities not in Phase 1: NATS JetStream publisher, S3-compatible uploader, persistent outbox, mTLS control plane, pre-inference (Tier 1) edge gate rules, OpenTelemetry traces, OTA support hooks.
3. Preserve every Phase 1 acceptance test (IT‑01 … IT‑18). The end-to-end behaviour as observed via MQTT and clips on disk must remain identical for the scenarios that Phase 1 already covers.
4. Add a Phase-2 integration suite (§13) covering the new capabilities.
5. Keep the same target platforms (§2 of Phase 1): aarch64 primary, x86_64 secondary, Linux ≥ 5.10, glibc or musl.

### 1.2 Non-goals (Phase 2)

- Replacing any Phase 1 hot-path C code in Go or Rust. The inspector, depayers, parsers, RTSP/RTP client, ringbuf, and muxers stay in C unchanged.
- Replacing the optional verify-path plugins (`emd_decoder_libav`, `emd_bgsub_opencv`). These remain dlopen'd from `libemd`.
- GPU inference at the edge. The agent uploads fragments and emits events; downstream GPU workers (separate service) perform inference.
- Multi-tenant isolation inside one agent process. One agent serves one tenant; the platform multi-tenants at the broker / storage layer.
- Two-way audio, PTZ, ONVIF discovery (still later — same as Phase 1 §1.2).

### 1.3 Why Go for the outer

The decision is recorded here so reviewers don't relitigate it in code review:

- The high-value engineering — the C core — is unchanged and not in Go's scope.
- Everything the agent layer needs has a first-party Go library: NATS (`nats.go`), AWS/MinIO (`aws-sdk-go-v2`), MQTT (`paho.mqtt.golang`), mTLS (stdlib `crypto/tls`), TOML (`BurntSushi/toml`), Prometheus (`client_golang`), OpenTelemetry (`go.opentelemetry.io/otel`), embedded KV for the outbox (`bbolt` or `sqlite` via `modernc.org/sqlite` to keep it pure-Go where possible).
- cgo's documented footguns (frequent crossings, panicking callbacks, leaking Go pointers into C) do not apply to this call pattern: the boundary is crossed once at camera startup (blocking native call) plus on rare events. The hot path stays inside C.
- Cross-compilation to aarch64 and musl is a one-line build invocation.
- Hiring and onboarding are easier than Rust.

Rust would be reconsidered if a future requirement places the agent on <128 MB-RAM hardware, or if local GPU inference moves into the agent process.

---

## 2. Architecture

### 2.1 Component diagram

```
                  ┌─────────────────────────────────────────────────┐
                  │                    emd-agent (Go)               │
                  │                                                 │
                  │   ┌─────────────────────────────────────────┐   │
                  │   │             Supervisor                  │   │
                  │   │   (config, lifecycle, hot-reload,       │   │
                  │   │    crash isolation, sd_notify)          │   │
                  │   └─────────┬───────────────────────────────┘   │
                  │             │ spawn / join                      │
                  │   ┌─────────▼──────────┐  one per camera        │
                  │   │ Camera Goroutine   │  LockOSThread          │
                  │   │  ┌──────────────┐  │                        │
                  │   │  │  cgo call →  │  │                        │
                  │   │  │ emd_cam_run()│  │                        │
                  │   │  └──────┬───────┘  │                        │
                  │   └─────────┼──────────┘                        │
                  │             │ event callback (C → Go)           │
                  │             ▼                                   │
                  │   ┌──────────────────────────────────────────┐  │
                  │   │            Event Bus (Go chan)           │  │
                  │   └──────────┬────────────┬────────────┬─────┘  │
                  │              │            │            │        │
                  │       ┌──────▼──────┐ ┌───▼────┐ ┌─────▼──────┐ │
                  │       │ Gate Rules  │ │Recorder│ │ Outbox     │ │
                  │       │ (Tier 1)    │ │ driver │ │ (persist)  │ │
                  │       └──────┬──────┘ └────┬───┘ └─────┬──────┘ │
                  │              │             │           │        │
                  │     ┌────────▼────┐ ┌──────▼──────┐ ┌──▼──────┐ │
                  │     │ Publisher   │ │ Uploader    │ │Retention│ │
                  │     │ NATS / MQTT │ │ S3 / R2     │ │ janitor │ │
                  │     └─────────────┘ └─────────────┘ └─────────┘ │
                  └─────────────────────────────────────────────────┘
                              │                       │
                  ┌───────────▼───┐         ┌─────────▼────────────┐
                  │   libemd.so   │         │  External services   │
                  │  (Phase 1 C)  │         │  NATS / MQTT / S3 /  │
                  │               │         │  Control plane       │
                  │  RTSP/RTP →   │         └──────────────────────┘
                  │  depay → parse│
                  │  → inspector  │
                  │  → ringbuf →  │
                  │  snapshot →   │
                  │  mux (TS/MP4) │
                  └───────────────┘
```

### 2.2 Process and threading model

- **One agent process per host.** Single binary, statically linked Go + statically-linked `libemd` where the toolchain allows; otherwise `libemd.so` next to the binary.
- **One OS thread per camera**, owned by a goroutine that calls `runtime.LockOSThread()` before invoking `emd_cam_run`. The C-side hot path therefore retains its Phase 1 affinity invariants (§4.1 of Phase 1).
- **Non-camera goroutines** (publisher, uploader, recorder driver, retention janitor, metrics, control plane, outbox flusher) live in the regular Go runtime, not pinned, and communicate via typed channels.
- **No `emd_event_bus_t` in Phase 2.** The Phase 1 MPMC bus is replaced by a Go `chan Event` that the C side writes to via a registered callback. The bus type stays in `libemd` only because internal helpers reference it; new code uses the callback.
- **No `emd_recorder_pool_t` in Phase 2.** Recording is now driven by Go: on an event, the recorder driver calls `emd_recorder_write_clip` (Phase 1 §recorder.h) directly from a Go goroutine on a worker pool sized by `GOMAXPROCS / 4`. This preserves the C-side muxer but moves scheduling and file lifecycle to Go.

### 2.3 Failure isolation

| Failure                          | Phase 1 behaviour                                | Phase 2 behaviour                                                          |
|----------------------------------|--------------------------------------------------|----------------------------------------------------------------------------|
| Single camera RTSP timeout       | Worker thread reconnects with backoff            | Identical (handled inside `emd_cam_run`)                                   |
| Single camera C-side crash       | Supervisor (C) restarts the pthread              | Go supervisor traps via `recover` on the cgo boundary if possible; otherwise the whole agent restarts under systemd (loses other cameras momentarily). See §2.4 for the trade-off and the alternative subprocess-per-shard mode. |
| MQTT broker down                 | Notifier queue holds up to `notifier.queue_max`  | Publisher writes to outbox first; channel back-pressure is bounded         |
| NATS broker down                 | n/a                                              | Same as MQTT — outbox absorbs                                              |
| S3 endpoint down                 | n/a                                              | Uploader retries with backoff; clips queued on local disk until accepted   |
| Agent process killed (OOM/panic) | Daemon restart loses in-flight events            | Outbox replays all unacknowledged events on restart                        |
| Disk full                        | Current clip rolled back; ingest continues       | Identical (delegated to `libemd`); agent additionally publishes `status` and stops accepting new recording requests until space recovered |

### 2.4 Crash containment trade-off

Cgo'd C code that crashes typically takes the whole Go process with it. Two operating modes are defined; the default is mode A:

- **Mode A — single-process.** All cameras in one agent process. Simplest. Relies on the C side's TDD discipline (Phase 1 §17), `-D_FORTIFY_SOURCE=3`, ASan-in-CI, and the bitstream parsers' 95% line-coverage floor to keep the crash rate near zero. A C-side crash restarts the whole agent under systemd's `Restart=always`. The outbox makes this lossless for events that were already enqueued; in-flight access units in the ring at the moment of crash are lost (same as Phase 1).
- **Mode B — sharded subprocess.** Agent supervises N child processes, each a `libemd`-loaded subprocess responsible for a shard of cameras. Crash of one shard does not affect others. IPC is via `socketpair(AF_UNIX, SOCK_SEQPACKET)`; events are length-prefixed FlatBuffers (§3.4). Mode B costs ~10 MB per shard and adds a serialization step on the event path. Enabled by `[agent] mode = "sharded"` and `[agent] cameras_per_shard = 8`.

Mode B is implemented for the v1.1 release. Mode A ships first.

---

## 3. The `libemd` C ABI

Phase 1 already exposes the underlying types we need (`emd_camera_cfg_t`, `emd_event_t`, `emd_ringbuf_snap_t`, `emd_inspector_cfg_t`, the muxer backends). Phase 2 adds a thin façade so Go can drive one camera at a time without depending on internal pool/bus types.

### 3.1 Versioning

```c
#define EMD_ABI_VERSION_MAJOR 1
#define EMD_ABI_VERSION_MINOR 0
#define EMD_ABI_VERSION_PATCH 0

uint32_t emd_abi_version(void);   /* returns 0x010000 = 1.0.0 */
const char *emd_build_info(void); /* version + commit + codec set */
```

Go calls `emd_abi_version()` at startup and refuses to run if the runtime ABI doesn't match the headers it was built against.

### 3.2 Per-camera handle

```c
/* Opaque handle. Internally owns the RTSP client state, ring buffer,
 * inspector state, and one pthread spawned at emd_cam_run() time. */
typedef struct emd_cam emd_cam_t;

/*
 * Open a camera. Returns NULL on failure; on failure errbuf carries a
 * precise message.
 *
 * cfg is copied; the caller may free or reuse it immediately.
 */
emd_cam_t *emd_cam_open(const emd_camera_cfg_t *cfg,
                        char *errbuf, size_t errbuf_len);

/*
 * Run the camera ingest loop on the calling thread.
 *
 * Blocks until emd_cam_stop() is called from another thread or a fatal
 * unrecoverable error occurs. Returns:
 *   0  — stopped cleanly via emd_cam_stop()
 *  -1  — fatal error (errbuf carries reason)
 *  -2  — config rejected by camera (e.g. SDP says unsupported codec)
 *
 * Reconnects on transient RTSP/RTP failures with exponential backoff
 * inside the function.
 */
int emd_cam_run(emd_cam_t *cam, char *errbuf, size_t errbuf_len);

/*
 * Signal the running camera to stop. Safe to call from any thread.
 * Returns immediately; emd_cam_run will return once the inner loop
 * unblocks.
 */
void emd_cam_stop(emd_cam_t *cam);

/* Close a stopped camera and release all resources. */
void emd_cam_close(emd_cam_t *cam);
```

### 3.3 Event callback (C → Go)

The Phase 1 `emd_event_t` is reused verbatim. Phase 2 adds a callback registration to deliver events directly to the caller instead of pushing onto a bus.

```c
/* Callback invoked from the camera thread on each detection event.
 * The callee MUST NOT block; it should copy the event and return.
 * 'evt' is owned by libemd and is valid only for the duration of the call. */
typedef void (*emd_event_cb_t)(void *user_ctx, const emd_event_t *evt);

void emd_cam_set_event_cb(emd_cam_t *cam, emd_event_cb_t cb, void *user_ctx);

/* Optional: also forward inspector stats periodically (e.g. every N
 * frames) so the Go side can populate metrics without polling. */
typedef struct {
    uint16_t cam_id;
    uint64_t mono_ns;
    double   bpf_ewma;
    double   bpf_slow;
    uint8_t  fsm_state;       /* emd_inspector_fsm_t */
    uint8_t  rtsp_state;      /* emd_rtsp_state_t */
} emd_stats_sample_t;

typedef void (*emd_stats_cb_t)(void *user_ctx, const emd_stats_sample_t *s);
void emd_cam_set_stats_cb(emd_cam_t *cam,
                          emd_stats_cb_t cb, void *user_ctx,
                          uint32_t every_n_frames);
```

Implementation note (Go side): the callback is a `static` C function in cgo land that bridges to an exported Go function via `cgo.Handle`. The user context is a `cgo.Handle.Value()` lookup yielding a `chan Event` or `chan StatsSample`. Sends use `try_send`-equivalent semantics (`select { case ch <- e: default: drop }`) so a slow consumer never blocks the camera thread.

### 3.4 Snapshot and recording

The Phase 1 `emd_ringbuf_snap_t` and `emd_recorder_write_clip()` are exposed unchanged. Phase 2 adds a convenience wrapper that combines snapshot + write so Go doesn't need to import `ringbuf.h` directly:

```c
typedef struct {
    const char         *out_path;     /* .part file in inflight dir */
    const char         *container;    /* "mpegts" | "fmp4" */
    emd_fsync_policy_t  fsync_policy;
} emd_clip_request_t;

/*
 * Pulls a snapshot covering [from_pts, to_pts] from the camera's ring,
 * widens it backwards to the nearest IDR + parameter sets, writes the
 * clip to req->out_path (the .part file), and fills hdr_out.
 *
 * Returns 0 on success, -1 if no data in range, -2 on muxer error.
 * Does NOT perform the rename — the Go side does that after fsync.
 */
int emd_cam_record(emd_cam_t *cam,
                   uint64_t from_pts_90khz, uint64_t to_pts_90khz,
                   const emd_clip_request_t *req,
                   emd_clip_header_t *hdr_out,
                   char *errbuf, size_t errbuf_len);
```

The `emd_clip_header_t` from Phase 1 (`recorder.h`) is returned without modification.

### 3.5 What is removed from the C tree

The following Phase 1 modules become *unused* in the agent build of `libemd` and are excluded from the default static archive (kept only for the `emd-standalone` legacy binary, which we ship until the agent is GA):

- `emd_supervisor` — replaced by Go supervisor.
- `emd_mqtt` (MQTT-C backend and mosquitto plugin) — replaced by Go publisher.
- `emd_event_bus_t` operations on the producer side — the bus is bypassed; events flow via callback.
- `emd_recorder_pool_t` — recording is Go-driven.
- The Prometheus HTTP endpoint inside `emd_metrics` — Go serves `/metrics`. The lock-free counters themselves stay and become read-only via a new `emd_metrics_snapshot()` accessor.
- `tomlc99` link — Go parses the TOML.

### 3.6 What is retained

Unchanged from Phase 1:

- `emd_net`, `emd_rtsp`, `emd_rtp`, `emd_h264_depay`, `emd_h265_depay`, `emd_h264_parse`, `emd_h265_parse`.
- `emd_inspector` (entirely — same code, same defaults, same hysteresis FSM).
- `emd_ringbuf` (entirely).
- `emd_mux_mpegts`, `emd_mux_fmp4`.
- Decoded-frame interface (§7.8 of Phase 1) — still the seam for future decoders.
- All optional plugins (`emd_decoder_libav`, `emd_bgsub_opencv`, etc.) — loaded by `libemd` via dlopen, transparent to Go.
- Build flags: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong …` (Phase 1 §14).

### 3.7 Header layout

A new aggregating header:

```
include/emd/agent_abi.h    /* the Phase 2 façade — what Go bindings against */
```

This header includes only the public Phase 1 types it forwards (`emd_camera_cfg_t`, `emd_event_t`, `emd_clip_header_t`, `emd_inspector_cfg_t`) and declares the §3.2–§3.4 functions. It is the only header the cgo build needs to see.

---

## 4. Go binding layer

### 4.1 Package layout

```
cmd/
  emd-agent/                      # main; CLI flags, signal handling
internal/
  libemd/                         # cgo wrapper (one Go type per C handle)
    bindings.go                   # cgo decls, struct mirrors
    camera.go                     # Camera type + Run/Stop
    callbacks.go                  # event/stats trampolines + cgo.Handle
    record.go                     # Record method + path helpers
    metrics.go                    # snapshot() reading C counters
  agent/
    supervisor.go                 # spawn/restart/hot-reload
    config.go                     # TOML, validation, hot-reload diff
    events.go                     # Event, EventType, codec enum
    rules/                        # Tier 1 gate rules (§5)
    recorder/                     # event → snapshot → mux → rename
    outbox/                       # bbolt-backed durable queue
    publisher/                    # nats + mqtt drivers
    uploader/                     # S3-compatible multipart
    controlplane/                 # mTLS, config sync, OTA hooks
    metrics/                      # prom exporter + OTel
    janitor/                      # retention enforcement
  testutil/
    fakecam/                      # exposes libemd test fakes to Go tests
third_party/
  libemd/                         # vendored libemd headers + .a/.so
```

### 4.2 cgo build configuration

```go
// internal/libemd/bindings.go

/*
#cgo CFLAGS:  -I${SRCDIR}/../../third_party/libemd/include
#cgo LDFLAGS: -L${SRCDIR}/../../third_party/libemd/lib -lemd -lm -lpthread
#cgo linux,arm64 LDFLAGS: -static-libgcc
#include <emd/agent_abi.h>
extern void goEmdEventTrampoline(void*, emd_event_t*);
extern void goEmdStatsTrampoline(void*, emd_stats_sample_t*);
*/
import "C"
```

Cross-compile commands:

```sh
# Linux aarch64, glibc
CC=aarch64-linux-gnu-gcc \
  GOOS=linux GOARCH=arm64 CGO_ENABLED=1 \
  go build -tags 'netgo,osusergo' -ldflags '-s -w' -o emd-agent ./cmd/emd-agent

# Linux aarch64, musl (zig cc for the toolchain)
CC="zig cc -target aarch64-linux-musl" \
  GOOS=linux GOARCH=arm64 CGO_ENABLED=1 \
  go build -tags 'netgo,osusergo,sqlite_omit_load_extension' \
  -ldflags '-linkmode external -extldflags "-static"' \
  -o emd-agent ./cmd/emd-agent
```

A static-PIE outcome equivalent to Phase 1's binary is achievable with the musl toolchain; glibc + cgo limits us to a dynamically-linked static-libgcc binary.

### 4.3 Per-camera goroutine

```go
// internal/agent/supervisor.go (sketch)

type cameraEntry struct {
    cfg     CamConfig
    cam     *libemd.Camera
    cancel  context.CancelFunc
    done    chan struct{}
    state   atomic.Int32   // worker state
    restart restartTracker
}

func (s *Supervisor) runCamera(ctx context.Context, e *cameraEntry) {
    defer close(e.done)
    runtime.LockOSThread()  // required: emd_cam_run owns the OS thread
    defer runtime.UnlockOSThread()

    for {
        cam, err := libemd.OpenCamera(e.cfg, s.eventCh, s.statsCh)
        if err != nil {
            if e.restart.giveUp() { e.state.Store(workerFailed); return }
            sleepWithJitter(e.restart.next(), ctx)
            continue
        }
        e.cam = cam
        e.state.Store(workerRunning)

        go func() { <-ctx.Done(); cam.Stop() }()

        err = cam.Run() // blocks; returns when Stop() called or fatal error
        cam.Close()

        if ctx.Err() != nil { return }
        // fatal error: log, backoff, retry
        s.metrics.cameraRestarts.WithLabelValues(e.cfg.Name).Inc()
        sleepWithJitter(e.restart.next(), ctx)
    }
}
```

### 4.4 Event trampoline

```go
// internal/libemd/callbacks.go

//export goEmdEventTrampoline
func goEmdEventTrampoline(userCtx unsafe.Pointer, cEvt *C.emd_event_t) {
    h := *(*cgo.Handle)(userCtx)
    sink := h.Value().(EventSink)
    sink.Send(Event{
        ID:           C.GoString(&cEvt.event_id[0]),
        CamID:        uint16(cEvt.cam_id),
        Type:         EventType(cEvt._type),
        Reason:       C.GoString(&cEvt.reason[0]),
        StartedPTS:   uint64(cEvt.started_pts_90khz),
        StartedMono:  time.Unix(0, int64(cEvt.started_mono_ns)),
        Codec:        uint8(cEvt.codec),
        FPS:          float64(cEvt.fps_estimate),
        CamName:      C.GoString(&cEvt.cam_name[0]),
    })
}

type EventSink interface { Send(Event) }   // implementations: bufferedChan, droppingChan

type droppingChan struct{ ch chan<- Event; dropped *atomic.Uint64 }

func (d droppingChan) Send(e Event) {
    select {
    case d.ch <- e:
    default:
        d.dropped.Add(1)
    }
}
```

The send is non-blocking by design. A slow consumer increments `events_dropped_total{cam,reason="chan_full"}` and is treated as a metrics-visible incident, never as back-pressure into the camera thread.

### 4.5 Stats sampling

The agent registers a stats callback with `every_n_frames = fps × 5` (≈ 5-second cadence) per camera. Samples are read off `statsCh` by the metrics goroutine and translated into Prometheus gauges. No additional FFI traffic is required to update `/metrics`.

---

## 5. Edge gate rules (Tier 1)

Phase 2 introduces a thin rule layer between event ingestion and downstream publish/upload. The intent (from the architecture discussion) is to avoid spending bandwidth and downstream GPU cycles on fragments that cannot possibly matter for the current tenant configuration.

### 5.1 Where it sits

```
event ──► Gate Rules ──► (accept) ──► Recorder + Publisher + Uploader
                     ──► (drop)   ──► drops counter + audit publish (low QoS)
```

Tier 1 rules run *after* the C inspector raised the event but *before* the recorder schedules a clip write. They evaluate using:

- camera metadata (`name`, `zone`, `armed`, tags)
- wall-clock time + camera's local timezone
- the event's `reason` (z-score, intra ratio, type)
- agent-wide state (e.g. site armed/disarmed)

They do NOT have access to detection content from a model (that's Tier 2, handled downstream by the GPU worker tier — out of scope here).

### 5.2 Rule language

CEL (Google Common Expression Language, `cel-go`). Read-only sandbox, statically typed, deterministic, no I/O. Rule files live in `/etc/emd-agent/rules.d/*.cel` and reload on SIGHUP.

```
// /etc/emd-agent/rules.d/01-arming.cel
//
// Drop fragments from cameras that are currently disarmed.
rule drop_disarmed = !camera.armed

// /etc/emd-agent/rules.d/02-business-hours.cel
//
// Drop motion in public zones during business hours.
rule drop_business_hours_public =
    camera.zone == "public" &&
    time.hour_local >= 9 && time.hour_local < 18 &&
    event.reason.matches("z=[0-9.]+")  // ordinary motion, no gradual

// /etc/emd-agent/rules.d/03-sample-through.cel
//
// Always sample a small percentage to the downstream pipeline for
// drift detection.
override sample_through = hash(event.event_id) % 100 < 1
```

Evaluation order: every rule prefixed `drop_` runs; if any matches, the fragment is dropped — *unless* any `override` rule matches, which forces accept. Drops are themselves published as `fragments.dropped` events (QoS 0, no retain) so the platform can audit gate decisions.

### 5.3 Default rules shipped

- `drop_disarmed` — any camera with `armed = false` in config.
- `drop_short_burst` — `reason.matches("z=[0-9.]+")` AND `event_duration_ms < 200` (suppress impulse noise from camera AGC steps).
- `sample_through` — 1% of all events bypass drops for downstream observability.

All defaults can be disabled per camera via TOML.

### 5.4 Performance budget

CEL evaluation is in the low microseconds for these rule shapes; the entire gate-rules pass is budgeted at ≤ 50 µs per event. Events fire seconds apart per camera, so this is irrelevant — but the budget is enforced in benchmarks to catch regressions.

---

## 6. Recorder driver (Go side)

### 6.1 Responsibilities

The Go recorder driver replaces the Phase 1 `emd_recorder_pool_t`. For each accepted event it:

1. Computes `(from_pts, to_pts) = (event.started_pts - pre_roll*90000, event.started_pts + post_roll*90000)`.
2. Builds the inflight path `${inflight_root}/${cam}/${event_id}.${ext}.part`.
3. Calls `emd_cam_record(cam, from_pts, to_pts, &req, &hdr_out, errbuf, len)`.
4. On success: `fsync` the directory, `rename` to `${clip_root}/${cam}/${YYYY}/${MM}/${DD}/${ts}-${reason}.${ext}`, then emit a `clip` message to the publisher.
5. On second event within the post-roll window: extend the existing clip's `to_pts` and reset the close deadline. (`emd_cam_record` is called once at close time, not incrementally.)

### 6.2 Coalescence and `clip_max_seconds`

A small per-camera state machine in `internal/agent/recorder/coalesce.go` mirrors Phase 1 §6.4 exactly:

```
IDLE      --(event E)--> PENDING(close_at = E.ts + post_roll)
PENDING   --(event E')-> PENDING(close_at = E'.ts + post_roll)   if E'.ts <= close_at
                                  (no new clip; extend duration)
PENDING   --(deadline) -> WRITING --(success)--> IDLE
PENDING   --(duration > clip_max)-> WRITING --(success)--> IDLE
```

The state machine is unit-tested with table-driven cases identical to Phase 1's `test_inspector_motion` style.

### 6.3 Worker pool

A pool of `max(2, num_cameras / 4)` goroutines consumes from a `chan recordRequest`. Each worker performs steps 3–5. Pool size and queue depth are config-tunable.

### 6.4 Failure handling

- `emd_cam_record` returns `-1` (no data) → emit `recording.skipped` audit event, no clip written, no publish.
- `emd_cam_record` returns `-2` (mux error) → delete `.part`, increment `recorder_errors_total{cam,reason="mux"}`.
- `rename` returns `ENOSPC` → delete `.part`, publish a `status` payload `{"online":true,"disk":"full"}`, suspend new recording requests until the janitor frees space.

---

## 7. Outbox

A durable on-disk queue that absorbs publish and upload work so the agent can survive broker / network / S3 outages without losing events.

### 7.1 Backing store

`bbolt` (formerly `boltdb/bolt`). Pure-Go, single-file, ACID with `fsync` per commit. Chosen over SQLite to avoid a second cgo dependency and keep the agent buildable with `CGO_ENABLED=0` for non-libemd contexts (tests, tooling).

Layout:

```
${data_dir}/outbox.db
  bucket "events"     k=event_id (ULID, time-ordered), v=protobuf(EventEnvelope)
  bucket "clips"      k=event_id,                       v=protobuf(ClipUpload)
  bucket "drops"      k=event_id,                       v=protobuf(DropEnvelope)
```

### 7.2 Write path

Every event accepted by the gate rules is written to `events` *before* the publisher is given a copy. The publisher acks back on broker confirmation (NATS PubAck / MQTT QoS 1 PUBACK), at which point the key is deleted.

Clip uploads work the same way against the `clips` bucket: a record is written when the clip is renamed into `clip_root`, deleted when S3 confirms `CompleteMultipartUpload`.

### 7.3 Replay

On startup the agent scans every bucket and re-emits unacked records, in key-order, to the publishers/uploaders. This gives at-least-once delivery semantics end-to-end, matching Phase 1 §10.2 (`QoS 1` MQTT) and adding it for NATS and S3.

### 7.4 Sizing and back-pressure

- `outbox.max_bytes` default 1 GiB. When ≥ 90% full, the gate rules are biased toward dropping (`drop_outbox_full` synthetic rule); when 100%, new events are dropped and counted.
- Compaction runs hourly.

---

## 8. Publisher

### 8.1 Transport selection

Configurable per-topic. Default for new deployments: NATS JetStream. MQTT remains supported with the Phase 1 wire contract for backward compatibility and IoT integrations.

```toml
[publisher]
default          = "nats"        # nats | mqtt
parallel_mqtt    = true          # publish to both NATS and MQTT
```

### 8.2 NATS subjects

Mirroring the Phase 1 MQTT topics but in NATS subject form, and namespaced per-tenant:

| Subject                                                         | Stream      | Replaces (Phase 1 MQTT)                                    |
|-----------------------------------------------------------------|-------------|------------------------------------------------------------|
| `t.{tenant}.site.{instance}.status`                             | STATUS      | `emd/{instance}/status`                                    |
| `t.{tenant}.site.{instance}.cam.{cam_id}.fragment.created`      | FRAGMENTS   | `emd/{instance}/cameras/{cam_id}/event` (+clip path)       |
| `t.{tenant}.site.{instance}.cam.{cam_id}.fragment.dropped`      | AUDIT       | (new)                                                      |
| `t.{tenant}.site.{instance}.cam.{cam_id}.clip`                  | CLIPS       | `emd/{instance}/cameras/{cam_id}/clip`                     |
| `t.{tenant}.site.{instance}.cam.{cam_id}.stats`                 | (no stream) | `emd/{instance}/cameras/{cam_id}/stats`                    |
| `t.{tenant}.site.{instance}.cmd.>`                              | -           | `emd/{instance}/cmd/+`                                     |

Payloads are JSON for ergonomic interop, but contain `v: 1` for forward compatibility. A FlatBuffers variant is available behind `[publisher] encoding = "flatbuffers"` for high-fanout deployments.

### 8.3 Fragment event payload (Phase 2 addition)

The Phase 1 event payload (§10.3) is preserved. A new `fragment` field is added describing where the clip will land in object storage:

```json
{
  "v": 1,
  "instance": "edge-01",
  "cam_id": "driveway",
  "event_id": "01HXY3Q…",
  "type": "motion",
  "reason": "z=4.7,intra_ratio=3.1",
  "started_at": "2026-05-16T14:22:09.413Z",
  "started_pts_90khz": 4123456789,
  "fps_estimate": 14.97,
  "codec": "h264",
  "fragment": {
    "key":           "tenants/acme/cam/driveway/2026/05/16/142209-01HXY3Q.ts",
    "bucket":        "emd-fragments",
    "region":        "us-west-2",
    "size_bytes":    4831234,
    "duration_ms":   14000,
    "sha256":        "…",
    "expected_eta":  "2026-05-16T14:22:24.000Z"
  }
}
```

Downstream consumers can either wait for the separate `clip` event (S3 upload confirmed) or, if they accept eventual visibility, begin a head check on the key immediately.

### 8.4 Connection handling

- TLS mandatory in production. mTLS preferred. CA pinning supported.
- Reconnect with exponential backoff `[1, 2, 4, 8, 16, 30]` s, identical cap to Phase 1 §8.1.
- LWT (`status` with `online=false`) configured at connect.
- Publish ack timeout 10 s, after which the event is re-queued from outbox.

---

## 9. Object storage uploader

### 9.1 Backends

`aws-sdk-go-v2` against S3-compatible endpoints (AWS S3, MinIO, Cloudflare R2, Backblaze B2, Garage). The same code path serves on-prem MinIO and hosted S3. The endpoint, region, credentials, bucket, and key template are configured per profile:

```toml
[storage.primary]
backend          = "s3"
endpoint         = "https://s3.us-west-2.amazonaws.com"
region           = "us-west-2"
bucket           = "emd-fragments"
key_template     = "tenants/{tenant}/cam/{cam}/{YYYY}/{MM}/{DD}/{HHMMSS}-{event_id}.{ext}"
credentials      = "instance"   # instance | static | sts | file
sse              = "aws:kms"
storage_class    = "STANDARD_IA"
multipart_threshold_mb = 16
multipart_part_mb      = 8
```

### 9.2 Upload protocol

- Files ≤ `multipart_threshold_mb`: `PutObject` with `x-amz-checksum-sha256` from the sidecar.
- Larger: `CreateMultipartUpload` → parallel `UploadPart` (concurrency 4) → `CompleteMultipartUpload` with the per-part ETags.
- Retries: 5 attempts with exponential backoff and jitter. On final failure the upload returns to the outbox and is retried on the next agent boot or 60-second sweep.
- Lifecycle hint: objects are tagged `retention=short` by default. The recorder driver re-tags objects referenced by a fired alarm to `retention=long`; bucket lifecycle policies (provisioned by the platform, not the agent) act on those tags.

### 9.3 Pre-signed URLs (optional)

Sites without outbound credentials can instead request short-lived pre-signed PUT URLs from the control plane. Agent flow becomes: ask control plane for URL → `PUT` directly. Same outbox semantics; same checksum.

---

## 10. Control plane

### 10.1 Connection

Long-lived NATS subscription (`t.{tenant}.site.{instance}.cmd.>`) plus an HTTP/gRPC client to the control plane for things that need request/response (cert renewal, signed-URL minting, OTA manifest fetch).

mTLS, with device certs issued during provisioning (out-of-band the first time, then rotated via SPIFFE-style workload identity).

### 10.2 Commands subscribed

Extends Phase 1 §10.5:

- `cmd.snapshot.{cam_id}` — force an event (testing).
- `cmd.reload` — re-read TOML config + rule files.
- `cmd.healthz` — one-shot status.
- `cmd.arm` / `cmd.disarm` — toggle site-armed state (drives `drop_disarmed`).
- `cmd.upload.{event_id}.reupload` — retry an upload from the outbox.
- `cmd.ota.apply` — fetch + verify + swap binary (see §10.4).
- `cmd.profile` — start a 60 s pprof CPU profile, publish to outbox.

### 10.3 Config sync

The agent can run in "local TOML" mode or "remote control plane" mode. In remote mode it pulls signed config bundles from the control plane and writes them to `/etc/emd-agent/`. SIGHUP triggers reload as in local mode. Schema:

```
ConfigBundle {
  version: uint64
  signed_by: <key id>
  toml_blob: string
  rules: map<filename, string>
  expiry: timestamp
}
```

### 10.4 OTA

Out of scope for v1.0 of the agent. The mechanism is reserved (`cmd.ota.apply` + a `bin/` payload bucket) and will be implemented in v1.1 using a verify-then-swap-then-restart approach with `systemd`'s `Type=notify`. Until then, updates are deployed by the OS package manager.

---

## 11. Configuration

`/etc/emd-agent/agent.toml`. Hot-reload on SIGHUP, identical semantics to Phase 1 §11 (parse into a temp struct, validate, swap atomically; on validation failure keep the previous config and log the error).

Sections specific to Phase 2 (the Phase 1 `[runtime]`, `[recording]`, `[cameras.*]` blocks are inherited and parsed by the Go config loader):

```toml
[agent]
mode              = "single"        # single | sharded
instance_id       = "edge-01"
data_dir          = "/var/lib/emd-agent"
sd_notify         = true

[publisher]
default           = "nats"
parallel_mqtt     = false

[publisher.nats]
url               = "nats://broker.example.com:4222"
creds_file        = "/etc/emd-agent/nats.creds"
tls_ca            = "/etc/emd-agent/ca.pem"
tls_cert          = "/etc/emd-agent/cert.pem"
tls_key           = "/etc/emd-agent/key.pem"
stream_fragments  = "FRAGMENTS"
stream_clips      = "CLIPS"

[publisher.mqtt]
url               = "mqtts://broker.example.com:8883"
client_id_prefix  = "emd"
qos               = 1
tls_ca            = "/etc/emd-agent/ca.pem"

[storage.primary]
# see §9.1

[outbox]
path              = "/var/lib/emd-agent/outbox.db"
max_bytes         = 1_073_741_824
fsync             = "always"        # always | batch | never

[rules]
dir               = "/etc/emd-agent/rules.d"
defaults_enabled  = true

[controlplane]
enabled           = true
url               = "https://control.example.com"
device_cert       = "/etc/emd-agent/device.crt"
device_key        = "/etc/emd-agent/device.key"

[metrics]
listen            = "0.0.0.0:9464"
otel_endpoint     = "https://otel.example.com:4317"
otel_token_file   = "/etc/emd-agent/otel.token"
```

All values are validated at parse time; invalid config → fatal startup error with `key=…` message. Hot-reload errors leave the previous config in place (same as Phase 1).

---

## 12. Observability

### 12.1 Metrics

The Phase 1 `emd_*` counters and gauges remain (read via `emd_metrics_snapshot()`). The agent adds:

- `emd_agent_camera_state{cam,state}` — replaces `emd_rtsp_state` aggregation
- `emd_agent_camera_restarts_total{cam,reason}`
- `emd_agent_events_dropped_total{cam,reason="rule_*|chan_full|outbox_full"}`
- `emd_agent_rules_eval_seconds` — CEL evaluation latency histogram
- `emd_agent_outbox_pending{bucket}`
- `emd_agent_outbox_fsync_seconds`
- `emd_agent_publish_total{transport,result,subject_kind}`
- `emd_agent_publish_latency_seconds{transport,subject_kind}`
- `emd_agent_upload_total{backend,result}`
- `emd_agent_upload_bytes_total{backend}`
- `emd_agent_upload_inflight{backend}`
- `emd_agent_record_seconds` — wall-clock of `emd_cam_record` calls
- `emd_agent_build_info{version,commit,libemd_version}` (gauge=1)

### 12.2 Logs

`slog` with the JSON handler. Required fields: `ts`, `level`, `cam_id` (when in cam context), `event_id` (when in event context), `subsystem`, `msg`. Compatible with Phase 1's log schema for downstream consumers.

### 12.3 Traces

OpenTelemetry, propagating trace context across events:

- `Camera.Run` opens a long-lived span per camera session.
- Each event creates a child span carrying `event_id` as a baggage attribute, with sub-spans for `rules.eval`, `record.write`, `publish.nats`, `upload.s3`.
- The published JSON payload includes `traceparent` so downstream GPU workers can continue the same trace through to alarms.

### 12.4 Health

- `sd_notify(WATCHDOG=1)` from the supervisor every `WatchdogSec/2`.
- HTTP `/healthz` (process up) and `/readyz` (every camera connected ≥ once, outbox queryable, publishers connected) on the metrics port — same shape as Phase 1 §12.3.

---

## 13. Testing strategy

### 13.1 Inherited

All Phase 1 tests (unit, fuzz, integration IT‑01…IT‑18) continue to run against `libemd` as-is. The CI matrix (Phase 1 §17.7) is extended with a Go axis (Go 1.22, 1.23).

### 13.2 New unit tests (Go side)

- `internal/agent/recorder/coalesce_test.go` — table-driven coalescence FSM tests, including `clip_max_seconds`.
- `internal/agent/rules/cel_test.go` — every default rule + a fixture matrix of camera × time × event.
- `internal/agent/outbox/outbox_test.go` — write/read/replay/compact under simulated crash (use `bbolt`'s in-memory mode).
- `internal/agent/publisher/nats_test.go` — against an embedded NATS test server (`nats-server` library).
- `internal/agent/publisher/mqtt_test.go` — against a Mochi MQTT in-process broker (`mochi-mqtt/server`).
- `internal/agent/uploader/s3_test.go` — against `minio` test container or `aws-sdk-go-v2/feature/s3/manager` mocks.
- `internal/libemd/callbacks_test.go` — cgo trampoline correctness; uses a stubbed `libemd` mock built from a tiny C harness.

### 13.3 New integration scenarios

Run against the Phase 1 `emd-fake-rtsp` and `emd-fake-mqtt` harnesses, plus an `nats-server` and a `minio` instance, all in Docker Compose.

| ID    | Scenario                                                                                              | Pass criteria |
|-------|-------------------------------------------------------------------------------------------------------|---------------|
| AG-01 | Single camera, motion burst → fragment event on NATS, clip on disk, clip uploaded to MinIO, `clip` event on NATS | All four happen within 5 s; payloads round-trip the Phase 1 schema |
| AG-02 | Identical to AG-01 but with `parallel_mqtt=true`                                                       | Both transports receive identical payloads |
| AG-03 | NATS broker offline for 30 s during burst                                                              | No clip lost; event replays on reconnect; outbox depth observable |
| AG-04 | S3 endpoint returns 503 for 60 s                                                                       | Upload retries succeed; eventually consistent; one `clip` event emitted per event |
| AG-05 | Disarm via `cmd.disarm`, then motion burst                                                              | No `fragment.created` event; `fragment.dropped` audit event published; no clip on disk |
| AG-06 | 16 cameras, all motion concurrently                                                                    | No cross-camera ordering or duplication issues; per-event end-to-end latency P99 ≤ 8 s |
| AG-07 | C-side simulated crash (test-only `emd_cam_panic()` hook)                                              | Mode A: agent restarts under systemd, outbox replays. Mode B: only affected shard restarts |
| AG-08 | TOML hot-reload changes a camera URL                                                                   | Old camera worker exits within 5 s; new worker connects; unaffected cameras emit no spurious events |
| AG-09 | Outbox 100% full                                                                                       | `drop_outbox_full` synthetic rule fires; metrics surface; oldest entries not deleted (no silent data loss) |
| AG-10 | Trace propagation                                                                                       | A `traceparent` set on a synthetic upstream span appears on the published payload and on the OTel collector |
| AG-11 | 24 h soak with 8 cameras                                                                                | RSS bounded; outbox compaction works; no fd leak; no goroutine leak (`pprof goroutines == steady`) |
| AG-12 | License audit                                                                                           | `go-licenses report` produces only MIT/BSD/Apache/MPL output; no GPL strings in the binary |

### 13.4 Performance gates

Per-event wall-clock at the agent boundary (event callback fires → fragment event published):

| Workload                                           | Budget P99 |
|----------------------------------------------------|------------|
| Event → rules eval → outbox write → publish (NATS) | ≤ 5 ms     |
| Event → recorder driver invocation                 | ≤ 1 ms     |
| Recorder driver → clip on disk (10 s clip, fMP4)   | ≤ 1.5 s    |
| Recorder driver → clip on S3 (10 MB, EU↔US WAN)    | ≤ 8 s      |

Per-camera steady-state overhead (Go side, idle camera, no events) is ≤ 0.5% of one core.

### 13.5 CI matrix

| Axis                | Values                                                       |
|---------------------|--------------------------------------------------------------|
| OS                  | Ubuntu 22.04, Alpine 3.19 (musl)                             |
| Go                  | 1.22, 1.23                                                   |
| Arch                | x86_64 native, aarch64 native (self-hosted) + aarch64 QEMU   |
| libemd build        | Phase 1 release artifact (matrixed) + main HEAD              |
| Race detector       | `-race` on all unit tests                                    |
| Sanitizers (libemd) | inherits Phase 1                                             |

Required green: every cell of (OS × Go × Arch × libemd release) for unit tests; integration suite green on Ubuntu × Go-latest × x86_64 × libemd release.

---

## 14. Build and packaging

- Go ≥ 1.22.
- `libemd` is consumed as a versioned tarball produced by the Phase 1 CI: headers under `include/`, `libemd.a` and `libemd.so` under `lib/`, SBOM under `sbom.cdx.json`. Vendored into `third_party/libemd/` by `make vendor-libemd`.
- Binary target: `emd-agent`. Static-linked where the toolchain allows (musl); dynamic libgcc-only on glibc with cgo.
- Container image: distroless `gcr.io/distroless/cc-debian12` base (need libc for cgo), agent binary + CA certs + a stripped-down libemd. Image size target ≤ 25 MB.
- systemd unit shipped under `dist/systemd/emd-agent.service`, `Type=notify`, `Restart=always`, `RestartSec=3`, `WatchdogSec=30`.
- SBOM (CycloneDX) emitted by CI: union of Go module graph + libemd's SBOM.

---

## 15. Security

Inherits Phase 1 §15 for everything inside `libemd`. Additions:

- Drops privileges to `emd:emd` after binding the metrics port (Go side, via `setresuid`).
- `seccomp-bpf` allowlist applied via `libseccomp-golang`. Disallowed: `ptrace`, `mount`, `pivot_root`, `kexec_*`, `bpf`, `clone3` with new namespaces, `process_vm_*`.
- Camera credentials read from `${EMD_CREDENTIALS}` (mode 0600) or systemd `LoadCredential=`; never logged. The Go config loader scrubs `password`/`auth` fields before any log emission.
- mTLS for all outbound: NATS, MQTT, S3 (where supported), control plane.
- Outbox file mode `0600`. Clip files `0640`.
- All third-party Go modules pinned with `go.sum`; `govulncheck` runs in CI and fails the build on any HIGH/CRITICAL hit.
- Supply chain: `cosign`-signed release tarballs and container images.

---

## 16. Performance budgets

In addition to Phase 1 §16 (which still applies to the C hot path):

| Workload (Go side, T1 reference: Ampere Altra 1 core, 8 cameras 1080p30 H.264) | Budget       |
|--------------------------------------------------------------------------------|--------------|
| Idle agent overhead                                                            | < 2% of core |
| Per-event end-to-end (event callback → fragment.created on NATS)               | ≤ 5 ms P99   |
| Recorder driver → clip on disk (10 s fMP4)                                     | ≤ 1.5 s P99  |
| Outbox commit                                                                  | ≤ 2 ms P99   |
| Memory floor (8 cameras, no events)                                            | ≤ 80 MiB RSS |

Benchmarks under `bench/` are part of CI; regressions > 15% fail the build.

---

## 17. Spec deltas — changes to Phase 1

The following sections of `edge-motion-detector-spec.md` (v0.1) require edits when this Phase 2 spec is accepted. These edits ship in `edge-motion-detector-spec.md` v0.2 and do not change `libemd`'s behaviour, only the description of how it is consumed.

- **§1.1 Goals.** Add: "May be consumed as a library (`libemd`) by an external supervisor process; see Phase 2 spec."
- **§3 High-level architecture.** Annotate the diagram: "Supervisor, Recorder pool, and Notifier are *either* the in-process C components shipped in the standalone `emd` binary, *or* are replaced by the Go agent (`emd-agent`) per the Phase 2 spec. The per-camera worker box is unchanged in either mode."
- **§5.1.1.** Note that `MQTT-C` and `tomlc99` are only linked when building the standalone `emd` binary; the `libemd` archive does not require them.
- **§5 Module decomposition.** Add a column "Phase 2 location" with values `libemd` or `emd-agent (Go)`. Modules `emd_supervisor`, `emd_mqtt`, parts of `emd_metrics`, parts of `emd_config` move to `emd-agent (Go)`.
- **§9.3 Common machinery.** Note that in agent mode the atomic rename, retention janitor, and sidecar JSON write are performed by the Go side using values returned from `emd_cam_record`.
- **§10 MQTT contract.** Add: "Wire-compatible payloads are also published over NATS by the Phase 2 agent; see Phase 2 §8."
- **§13 Extension points.** Add a row: "`emd_agent_*` — process-level extension points (rules, publisher, uploader, controlplane) live in Go and are out of scope for the C plugin model."
- **§17.5 Integration test harness.** Note that the harness is reused unchanged by Phase 2's `AG-*` scenarios and that the Go side adds NATS and S3 fakes.
- **§18 Traceability.** Add Phase 2 entries: NATS publish, S3 upload, outbox durability, edge gate rules.
- **§19 Open questions.** Move "Outbound clip uploader as an `emd_uploader_*` plugin (S3, GCS, R2)" to "Implemented by Phase 2 agent."

---

## 18. Migration plan

1. **Cut Phase 1 v1.0.0.** Standalone `emd` binary released, IT‑01…IT‑18 green.
2. **Refactor for ABI.** Split `emd_main.c` from the rest of the C tree; produce `libemd.a` and `libemd.so` artifacts in CI. Add `include/emd/agent_abi.h`. Implement `emd_cam_open / _run / _stop / _close / _record / _set_event_cb / _set_stats_cb`. Lands as Phase 1 v1.1.0 — fully backward-compatible (standalone `emd` still works).
3. **Stand up `emd-agent` skeleton.** Empty Go module, cgo wrapper compiling against `libemd` v1.1.0, supervisor that can run one camera and print events to stderr. Internal milestone, no release.
4. **Replicate Phase 1 wire contract.** MQTT publisher reaches feature parity with `emd_mqtt`. Re-run IT‑01…IT‑18 through the agent. This is the proof point that the boundary is right.
5. **Add Phase 2-only capabilities.** NATS, S3, outbox, rules, control plane, traces, OTel. New AG‑* tests.
6. **Cut `emd-agent` v1.0.0.** Both binaries (`emd` standalone + `emd-agent`) ship from the same monorepo, share `libemd`, and remain supported. New deployments target the agent; existing deployments are not forced to migrate.
7. **Defer.** Sharded subprocess mode (§2.4 Mode B), OTA (§10.4), GCS / Azure Blob uploaders.

---

## 19. Open questions

1. Should the gate rules tier also receive frame-rate or bitrate samples from the inspector, allowing rules like "drop fragments where the encoder is in low-bitrate mode (likely empty scene)"? Would require a Tier 1 augmentation to the event payload from `libemd`.
2. Sharded mode (§2.4 B) — is the FlatBuffers serialization a meaningful overhead vs in-process callback? Needs a microbenchmark.
3. Whether to expose the outbox as a queryable read replica (e.g. via the metrics port) so an operator can see what's pending without inspecting `bbolt` directly.
4. Whether to ship a "lite" agent build without S3 / NATS for the smallest edge devices, falling back to MQTT + local clip retention only.
5. Whether to also offer Rust bindings against the same `libemd` ABI for organizations that prefer Rust outside. The ABI is language-agnostic; the work is mostly the equivalent of the `internal/libemd` package plus a `agent` crate.
6. GPU worker tier (Tier 2 rules, YOLO, third-party gateway) is intentionally out of scope here — it will be specified in `gpu-worker-spec.md` once the agent is GA, since the wire contract between agent and workers is the only coupling and is fully specified in §8.
