# emd — Extended Reporting & Tuning Support (Implementation Spec)

Status: Ready for implementation
Owner: `cvkit/edge`
Last updated: 2026-05-18
Related design: [`auto-tune-design.md`](auto-tune-design.md) (revision 2)

## 0. Read this first

This spec is for an implementer who needs to extend `emd-agent` (the Go binary in `cmd/emd-agent`) and `libemd` (the C library under `src/` + `include/emd/`) so that an external auto-tuner and post-processor can do their work. The agent itself is not gaining detection logic — it is gaining *reporting* and a small number of new C inspector knobs the tuner is allowed to write.

If you're picking this up, read in this order:

1. [`CLAUDE.md`](../CLAUDE.md) — invariants you cannot violate.
2. [`auto-tune-design.md`](auto-tune-design.md) — the *why*. This spec is the *what* and *how*.
3. [`docs/archive/FALSE_POSITIVE_ANALYSIS.md`](archive/FALSE_POSITIVE_ANALYSIS.md) — the concrete false-positive case the new inspector knobs are designed to suppress.
4. This file.

## 1. Scope

In scope.

- Extend `emd_inspector_cfg_t` with five new fields the tuner can write and the inspector uses on the hot path.
- Plumb those fields through TOML parsing, the C ABI, the Go bindings, and the existing REST config endpoint.
- Add a per-camera event JSONL ring on disk (the *event log*) and a `GET /api/cameras/{name}/events` endpoint to read it.
- Add lazy thumbnail generation per clip and a `GET /api/clips/{id}/thumbs` endpoint to fetch it.
- Add a NATS publisher (`internal/nats`) that publishes raw events to `events.raw.<site>.<camera>`. Edge-side only; consumers live in `cvkitio/autotune`.
- Add the corresponding TOML section, REST validation, metrics, and tests.

Out of scope.

- The post-processor and tuner themselves. Those live in `cvkitio/autotune` — see [its spec](../../autotune/docs/spec.md).
- Pixel decode on the edge. The thumbnail generator is the *only* place the edge ever decodes pixels, and it runs lazily on a request goroutine (never on the camera worker).
- MQTT changes. MQTT alerting stays as it is; NATS is additive.
- New transport, new codec, new muxer.

## 2. Hot-path invariants (recap)

Restate-don't-violate, from [`CLAUDE.md`](../CLAUDE.md):

- **No mutex on the camera worker.** All new shared state goes through `_Atomic` with explicit memory orders, or through the existing `emd_cam_update_inspector_cfg` swap path.
- **No pixel decode on the camera worker.** Thumbnail generation runs on a request-scoped goroutine in the Go agent, hitting libav (`emd_decoder_libav` plugin, dynamic-link LGPL only) on demand.
- **Zero-copy / arena-allocated NAL records.** Unchanged. None of this work touches the RTP→ringbuf path.
- **Atomic clip writes.** Unchanged. Thumbnail strips are a sibling file `<clip>.thumbs.jpg` written `.part`→`rename`, same as clips.
- **MQTT failure must not block clips.** The same rule applies to NATS: the publisher is async, lossy under back-pressure, and never blocks the recorder or the worker.

If you find yourself wanting to lock something on the hot path, stop and re-read §3 of the design doc.

## 3. Workstreams

Eight workstreams, ordered by dependency. W1–W3 are required for Phase A of the design (encoded-domain tuning wins). W4–W7 are Phase B/C (NATS publisher and external post-processor enablement). W8 is the cross-cutting test plan.

### W1 — Extend `emd_inspector_cfg_t` and inspector logic

**Files**

- `include/emd/inspector.h` — add fields, update defaults macro, bump comment block.
- `src/emd_inspector.c` — apply new fields in `emd_inspector_process` and `emd_inspector_default_cfg`.
- `tests/test_inspector.c` (existing or new) — cmocka unit tests covering each new field.

**ABI considerations**

`emd_inspector_cfg_t` is part of the C ABI exposed by `agent_abi.h`. Adding fields to the **end** of the struct is a MINOR bump (1.0.0 → 1.1.0), not MAJOR, because Go callers populate the struct field-by-field and we provide `emd_inspector_default_cfg` for zero-initialisation safety. Bump `EMD_ABI_VERSION_MINOR` in `agent_abi.h`.

**New fields** (append, in this order)

```c
/* in emd_inspector_cfg_t, after gradual_window_frames: */

uint32_t min_bytes_threshold;  /* absolute BPF floor.
                                * If bytes < min_bytes_threshold, suppress
                                * the z-score signal regardless of z value.
                                * 0 = disabled (default). */

double   bpf_relative_floor;   /* multiplicative floor relative to bpf_slow.
                                * If bytes < bpf_relative_floor * bpf_slow,
                                * suppress the z-score signal.
                                * 0.0 = disabled (default). Typical: 1.5. */

double   z_high_warmup;        /* alternate, stricter motion_z_high used for
                                * the first z_high_warmup_frames after each IDR.
                                * Suppresses post-IDR P-frame inflation FPs.
                                * 0.0 = use motion_z_high (default). */

uint16_t z_high_warmup_frames; /* number of frames after IDR during which
                                * z_high_warmup applies.  0 = disabled. */

uint8_t  target_class_mask;    /* bitfield of intended target classes.
                                * Bits: EMD_TARGET_PERSON  = 1<<0,
                                *       EMD_TARGET_VEHICLE = 1<<1,
                                *       EMD_TARGET_ANIMAL  = 1<<2,
                                *       EMD_TARGET_OTHER   = 1<<7.
                                * Inspector does not use this; it is included
                                * in published events so downstream consumers
                                * can filter. 0 = unspecified. */
```

Define the `EMD_TARGET_*` constants near the new field.

**Inspector logic changes** (`emd_inspector_process`)

Apply the new gates **before** the existing z-score motion decision (immediately after the z computation, before the FSM step):

```c
/* suppress if absolute byte count is too low */
if (cfg->min_bytes_threshold > 0 && in->byte_count < cfg->min_bytes_threshold) {
    is_z_motion = false;
    /* leave is_intra_motion and is_unexpected_idr alone — those have their
     * own physical interpretation independent of byte count */
}

/* suppress if byte count is too low relative to baseline */
if (cfg->bpf_relative_floor > 0.0 &&
    (double)in->byte_count < cfg->bpf_relative_floor * s->bpf_slow) {
    is_z_motion = false;
}

/* warmup window after IDR uses stricter z threshold */
if (cfg->z_high_warmup > 0.0 &&
    cfg->z_high_warmup_frames > 0 &&
    s->since_kf != UINT32_MAX &&
    s->since_kf < cfg->z_high_warmup_frames) {
    is_z_motion = (z > cfg->z_high_warmup);
}
```

When `is_z_motion` is suppressed by either floor, append `",suppressed:floor"` to the reason string so the published event makes the cause explicit (the post-processor would otherwise have no idea why the threshold did not fire when it expected one).

**Defaults** (`emd_inspector_default_cfg`)

```c
cfg->min_bytes_threshold  = 0;     /* disabled */
cfg->bpf_relative_floor   = 0.0;   /* disabled */
cfg->z_high_warmup        = 0.0;   /* disabled */
cfg->z_high_warmup_frames = 0;
cfg->target_class_mask    = 0;
```

**Acceptance criteria**

- New fields default to zero; existing callers see no behavioural change with defaults.
- A camera configured with `min_bytes_threshold = 2000` and the trace from `FALSE_POSITIVE_ANALYSIS.md` produces zero motion events (where today it produces one).
- A camera configured with `z_high_warmup = 5.0, z_high_warmup_frames = 4` does not fire on a P-frame immediately after an IDR even when the encoder briefly inflates.
- `emd_inspector_default_cfg` zeroes all new fields.
- `EMD_ABI_VERSION_MINOR` bumped; `EMD_ABI_VERSION_MAJOR` unchanged.

**Tests** (`tests/test_inspector.c`)

Add four test cases following the existing cmocka pattern:

- `test_inspector_min_bytes_threshold_suppresses` — feed the fixture from `FALSE_POSITIVE_ANALYSIS.md` (frames at 354 B baseline, spike to 906 B); assert no event fires when `min_bytes_threshold = 2000`.
- `test_inspector_bpf_relative_floor_suppresses` — similar, with `bpf_relative_floor = 3.0`.
- `test_inspector_warmup_higher_threshold` — feed an IDR followed by a 2× P-frame; with `z_high_warmup = 8.0, z_high_warmup_frames = 3`, assert no event; without warmup, event fires.
- `test_inspector_target_class_mask_is_metadata_only` — set mask = 0xFF, assert detection behaviour is unchanged vs. mask = 0.

Register in `tests/CMakeLists.txt` via the existing `emd_add_test` macro.

### W2 — TOML parsing for new camera fields

**Files**

- `include/emd/config.h` — add fields to `emd_camera_cfg_t` to mirror the new inspector fields.
- `src/emd_config.c` — parse them in `parse_camera()`.
- `tests/test_config.c` (existing) — add cases.
- `configs/axis_p4708.toml` — add a commented-out example block showing the new keys.

**New fields in `emd_camera_cfg_t`**

```c
/* after configured_periodic_kf: */

uint32_t  min_bytes_threshold;
double    bpf_relative_floor;
double    z_high_warmup;
uint16_t  z_high_warmup_frames;
uint8_t   target_class_mask;
```

**TOML keys**

```toml
[cameras.<name>]
# existing keys unchanged
min_bytes_threshold   = 0
bpf_relative_floor    = 0.0
z_high_warmup         = 0.0
z_high_warmup_frames  = 0
target_classes        = ["person", "vehicle"]   # parsed into bitmask
```

`target_classes` is a string array in TOML for ergonomic config files; the parser converts to the bitmask. Unknown class strings produce a config error with line context.

**Parser**

In `parse_camera()` after `configured_periodic_kf` block, read each field with the existing `toml_*_in` helpers and copy into `cam->*`. For `target_classes`, walk the TOML array, lowercase each element, match to known classes (`person`/`vehicle`/`animal`/`other`), and OR-into `cam->target_class_mask`. Validate ranges:

- `min_bytes_threshold` ≤ 10_000_000 (sanity).
- `0.0 ≤ bpf_relative_floor ≤ 100.0`.
- `0.0 ≤ z_high_warmup ≤ 100.0`.
- `z_high_warmup_frames ≤ 600`.

Reject with `snprintf(errbuf, ...)` giving the camera name + offending key on violation.

Then wire the camera fields into the inspector config when the camera worker is created — find the existing place in `src/emd_supervisor.c` (or wherever `emd_inspector_cfg_t` is built from `emd_camera_cfg_t`) and add the five copies.

**Acceptance criteria**

- A TOML with all new keys parses cleanly and the loaded `emd_camera_cfg_t` reflects the values exactly.
- An invalid `target_classes = ["spaceship"]` produces an error message naming both the camera and the bad class.
- Defaults match W1 defaults when keys are omitted.

**Tests** (`tests/test_config.c`)

- `test_config_parses_new_tuning_fields` — read a fixture TOML with all five keys set, assert each field on the parsed struct.
- `test_config_rejects_unknown_target_class` — assert non-zero return and that the error message contains the camera name.
- `test_config_target_classes_array_to_bitmask` — verify bit positions are correct.

### W3 — Go bindings + REST extensions

**Files**

- `internal/libemd/bindings.go` — extend `CameraConfig` struct + `toCConfig`.
- `internal/libemd/camera.go` — extend `InspectorConfig` struct + `UpdateInspectorConfig` + `GetInspectorConfig`.
- `internal/agent/config.go` — add the five fields to the agent's `CameraConfig` struct and to `ToLibemdConfig`.
- `internal/api/handler.go` — extend `InspectorConfigResponse`, `InspectorConfigRequest`, and the validation block in `updateInspectorConfig`.
- `internal/api/openapi.go` — extend the OpenAPI schema for the config endpoint.
- `internal/api/handler_test.go` — extend.

**Go field names**

In `libemd.CameraConfig` and `libemd.InspectorConfig`:

```go
MinBytesThreshold  uint32
BPFRelativeFloor   float64
ZHighWarmup        float64
ZHighWarmupFrames  uint16
TargetClassMask    uint8
```

In `agent.CameraConfig` (TOML mirror):

```go
MinBytesThreshold  uint32   `toml:"min_bytes_threshold"`
BPFRelativeFloor   float64  `toml:"bpf_relative_floor"`
ZHighWarmup        float64  `toml:"z_high_warmup"`
ZHighWarmupFrames  uint16   `toml:"z_high_warmup_frames"`
TargetClasses      []string `toml:"target_classes"`  // converted to mask in ToLibemdConfig
```

`ToLibemdConfig` translates `TargetClasses` to the bitmask using the same class→bit mapping as the C parser (factor into a shared constant block to avoid drift).

**REST schema**

`InspectorConfigResponse` and `InspectorConfigRequest` gain the five fields. `Request` uses `*` pointers for all new fields so partial updates work. Validation in `updateInspectorConfig`:

```go
if currentCfg.BPFRelativeFloor < 0 || currentCfg.BPFRelativeFloor > 100 { ... }
if currentCfg.ZHighWarmup     < 0 || currentCfg.ZHighWarmup     > 100 { ... }
if currentCfg.ZHighWarmupFrames > 600 { ... }
// min_bytes_threshold and target_class_mask: any uint32 / uint8 is valid
```

Update `internal/api/openapi.go` to add the new properties under the `InspectorConfig` schema. Keep the example payload realistic — e.g. `{ "min_bytes_threshold": 2000, "z_high_warmup": 5.0, "z_high_warmup_frames": 4 }`.

**Acceptance criteria**

- `PUT /api/cameras/{name}/config` with `{"min_bytes_threshold": 2000}` returns 200, and a subsequent GET returns the same value.
- Round-trip: TOML → load → `ToLibemdConfig` → C struct → `emd_cam_update_inspector_cfg` → `GetInspectorConfig` → matches input.
- Updating only one new field via PUT leaves the others at their previous values (use the existing pointer-merge pattern).
- OpenAPI spec validates (no schema errors); the JSON examples render correctly in `/docs`.

**Tests**

- Extend `internal/libemd/camera_test.go` (or add one) with a round-trip test through `Get`/`Update`.
- Extend `internal/api/handler_test.go`: one PUT, one GET, one PUT with invalid range that 400s.

### W4 — Event log (per-camera JSONL ring on disk)

The post-processor reads events live from NATS; the tuner needs a longer history. JetStream history is bounded; the on-disk JSONL ring is the authoritative long-tail. Both the publisher (W5) and the on-disk ring (this workstream) are fed from the same in-process event broadcast point so they cannot diverge.

**Files**

- `internal/eventlog/log.go` — new package.
- `internal/eventlog/log_test.go` — tests.
- `internal/agent/supervisor.go` — write to the event log inside the existing `eventCh` consumer.

**Schema (one JSON object per line)**

```json
{
  "event_id":        "01HXY3Q5ABCDE",
  "site":            "site-warehouse-3",
  "camera":          "axis_82_2",
  "cam_id":          17,
  "ts":              "2026-05-18T03:01:14.221Z",
  "ts_mono_ns":      14123456789012,
  "type":            "motion",
  "z_score":         5.52,
  "intra_ratio":     1.1,
  "bytes":           906,
  "bpf_slow":        354.0,
  "bpf_ewma":        540.2,
  "bpf_var":         9876.0,
  "since_kf":        12,
  "reason":          "z=5.52",
  "fsm_before":      "idle",
  "fsm_after":       "active",
  "pts_start":       1234567890,
  "pts_end":         1234589012,
  "codec":           "h264",
  "fps":             25.0,
  "clip_id":         "01HXY3Q5ABCDE",
  "clip_path":       "/var/lib/emd/clips/axis_82_2/01HXY3Q5.ts",
  "target_class_mask": 7,
  "agent_version":   "1.0.0"
}
```

Notes on shape:

- `event_id` is the existing ULID used elsewhere (`emd_event_t.event_id`).
- `ts_mono_ns` is the camera worker monotonic clock; `ts` is wall-clock UTC at publish time. The post-processor uses `ts_mono_ns` for replay ordering and `ts` for human-readable logs.
- `clip_path` may be empty if no clip was written (suppressed event).
- All fields that the C inspector already computes (`z_score`, `bpf_slow`, `bpf_ewma`, `bpf_var`, `since_kf`, FSM transitions) need to be **added to `emd_event_t`** so the Go side can publish them. See "C ABI follow-up" below.

**C ABI follow-up**

Today `emd_event_t` carries `event_id`, `cam_id`, `type`, `reason`, PTS, codec, fps, name, pre/post-roll. Extend it (additive, MINOR bump) with:

```c
double   z_score;
double   intra_ratio;
size_t   byte_count;
double   bpf_slow;
double   bpf_ewma;
double   bpf_var;
uint32_t since_kf;
uint8_t  fsm_before;   /* emd_inspector_fsm_t */
uint8_t  fsm_after;
uint8_t  target_class_mask;
```

`emd_supervisor.c`'s event construction path (where it currently fills the existing fields after `emd_inspector_process` returns true) populates these from the inspector's state and result. The event then flows through the existing event callback (`emd_event_cb_t`) — no new ABI function needed.

**Ring policy**

One file per camera, rotated daily: `<data_dir>/eventlog/<camera>/YYYY-MM-DD.jsonl`. Per-camera cap (default 100 MB across all files; oldest day rotated out first when exceeded). Configurable in TOML:

```toml
[eventlog]
enabled       = true
root          = "/var/lib/emd/eventlog"   # defaults to <agent.data_dir>/eventlog
max_bytes_per_camera = 104857600          # 100 MB
retention_days       = 30
```

**Implementation rules**

- Single writer goroutine per camera. The event broadcast inside `supervisor.go` does a non-blocking send on a per-camera channel of cap 1024; if full, drop with a `emd_eventlog_dropped_total{camera}` metric. The broadcast must not block the consumer.
- Files are append-only. `O_APPEND` + `O_CREAT` + `0640`. fsync on rotation only (matching the `[recording] fsync_policy` philosophy).
- Rotation triggers: midnight UTC, file size > 16 MB, or process restart. Old files renamed `YYYY-MM-DD.jsonl` (the current writer always writes to `current.jsonl` and renames on rotate).

**Public API (Go)**

```go
package eventlog

type Writer struct { ... }
func New(root string, cfg Config) (*Writer, error)
func (w *Writer) Append(cam string, evt Event) error  // non-blocking, drops if full
func (w *Writer) Close() error

type Reader struct { ... }
func Open(root string, cam string) (*Reader, error)
// Range returns events with ts in [from, to], oldest first.
func (r *Reader) Range(from, to time.Time) ([]Event, error)
func (r *Reader) Close() error
```

**Acceptance criteria**

- Replaying the `FALSE_POSITIVE_ANALYSIS` fixture writes exactly one JSONL line with `z_score ≈ 5.52` and `bytes = 906`.
- Two days of rotation produces two files; the writer never holds more than two FDs open per camera.
- A full per-camera channel increments `emd_eventlog_dropped_total` and does not block the worker.

**Tests**

- `internal/eventlog/log_test.go` — append, read range, rotation, retention enforcement, full-channel drop.

### W5 — NATS publisher

**Files**

- `internal/nats/publisher.go` — new package.
- `internal/nats/publisher_test.go` — tests using `nats-server`'s embeddable test server.
- `internal/agent/supervisor.go` — second non-blocking send from the event broadcast.
- `go.mod` — add `github.com/nats-io/nats.go`.

**TOML config**

```toml
[nats]
enabled        = true
url            = "nats://127.0.0.1:4222"
subject_prefix = "events"
site           = "site-warehouse-3"
# auth (one of):
creds_file     = "/etc/emd/nats.creds"        # NATS JWT
# or:
nkey_file      = "/etc/emd/nats.nk"
# or omit both for unauthenticated dev.
```

**Subject**

`events.raw.<site>.<camera>` per event. Site and camera are interpolated from config. Subject characters limited to NATS-safe (`[a-zA-Z0-9_-]`); validate at config load.

**Payload**

Identical to the JSONL schema in W4, encoded with `encoding/json`. Reuse the `eventlog.Event` struct — do not define a second one.

**Publisher rules**

- One `nats.Conn` per agent, shared across cameras. Set `nats.NoReconnect(false)`, `nats.MaxReconnects(-1)`, `nats.ReconnectWait(2 * time.Second)`, `nats.PingInterval(20 * time.Second)`.
- Per-camera publish goroutine reading from a channel of cap 1024 (separate from the eventlog channel — different consumers, do not couple their back-pressure).
- Non-blocking `Publish` (the async core method). If the publisher buffer is full or the connection is down, drop and increment `emd_nats_publish_dropped_total{camera,reason}`.
- No JetStream client code in this package. JetStream durability is a server-side stream configuration (operator concern, documented in the autotune repo). Edge always publishes plain subjects.
- Shutdown: drain the publisher goroutines, then `conn.Drain()` (best-effort, deadline 2s), then `conn.Close()`. Recorder shutdown happens *after* publisher shutdown so any in-flight events get a chance to be published.

**Hot-path safety**

The supervisor's `eventCh` consumer is already off the C camera worker (the C side invokes the event callback which trampolines into Go and sends on `eventCh`; the consumer goroutine reads it). All three downstream sinks (recorder, eventlog, NATS) live in that consumer. None of them may block. Each does its own non-blocking handoff or its own bounded queue.

**Metrics**

- `emd_nats_publish_total{camera}` — counter.
- `emd_nats_publish_dropped_total{camera,reason}` — counter; reason in `{queue_full,disconnected,publish_error}`.
- `emd_nats_connection_state` — gauge: 0=disconnected, 1=connected, 2=reconnecting.

**Acceptance criteria**

- Embedded NATS test server receives an event payload matching the JSONL schema within 100 ms of the inspector firing.
- Killing the broker mid-stream does not stall the worker; the agent reconnects automatically when the broker is back.
- With `[nats] enabled = false` (default), no NATS client is constructed and the binary has no dependency on a running broker.

**Tests**

- `publisher_test.go` — happy path against `natsserver.RunBasicServer()`.
- Disconnect-during-publish test (stop server, publish 100 events, restart, verify no panic and counter incremented).
- Schema test: marshal an event, ensure JSON matches the W4 schema (`testdata/event_golden.json`).

### W6 — Lazy thumbnail generation + endpoint

**Files**

- `internal/thumbs/generator.go` — new package, wraps the `emd_decoder_libav` plugin via CGo.
- `internal/api/handler.go` — add `/api/clips/{id}/thumbs` handler.
- `internal/api/openapi.go` — extend.

**Generation policy**

On first GET, decode three frames from the clip: at 0%, 25%, and 50% of clip duration. Scale each to 160 px wide preserving aspect, JPEG-encode at quality 75, concatenate horizontally into a single 480 × ~90 JPEG. Write to `<clip>.thumbs.jpg.part`, fsync, rename to `<clip>.thumbs.jpg`. Serve directly.

On subsequent GETs, serve the cached file. If the cached file is older than the clip's mtime (shouldn't happen, but defensive), regenerate.

**Concurrency**

- A package-level singleflight (`golang.org/x/sync/singleflight`) keyed by `clip_id` so two concurrent requests for the same clip cause one decode.
- A bounded worker semaphore (`max_concurrent_thumbs`, default 4) — thumbnail generation may not starve the recorder if 100 requests arrive simultaneously.

**Decoder plugin**

If `emd_decoder_libav` does not exist as code today (an open question called out in the design), the implementer must either:

1. Stand up the plugin per the spec in `edge-motion-detector-phase2-spec.md` §7.8 (returns `emd_frame_t`), OR
2. Use a Go-native libav binding (e.g. `github.com/asticode/go-astiav`) **scoped to this package only** so the rest of the codebase remains decoder-free.

Recommend (2) for first delivery — faster to land — with a TODO to migrate to the plugin once it exists, so the post-processor and the thumbnail generator can share one decoder seam.

**Endpoint**

```
GET /api/clips/{id}/thumbs

200 OK
Content-Type: image/jpeg
[binary]

404 Not Found  — unknown clip id
503 Service Unavailable — decoder not available / over capacity (Retry-After: 5)
```

**Metrics**

- `emd_thumbs_generated_total` — counter (cache miss).
- `emd_thumbs_served_total` — counter (any 200).
- `emd_thumbs_decode_duration_seconds` — histogram.

**Acceptance criteria**

- A fresh GET on an existing clip returns a valid JPEG within 300 ms on a modern x86 core.
- A second GET within 1 s returns the cached file with `decode_duration` not observed.
- 50 concurrent GETs on the same clip produce one decode, not 50.

**Tests**

- Generate a thumbnail from a 5-second fixture clip; assert dimensions and JPEG SOI marker.
- Concurrent-request test asserts singleflight collapses to one decode (use a counter in the test decoder hook).

### W7 — Event-log REST endpoint

**Files**

- `internal/api/handler.go` — add `/api/cameras/{name}/events`.
- `internal/api/openapi.go` — extend.

**Endpoint**

```
GET /api/cameras/{name}/events?from=<rfc3339>&to=<rfc3339>&limit=<n>

200 OK
Content-Type: application/x-ndjson
<event json>\n
<event json>\n
...
```

Defaults:
- `from`: 24h ago.
- `to`: now.
- `limit`: 10000.
- Maximum window: 7 days. Reject larger windows with 400 (the operator should use the autotune sidecar's JetStream history for longer windows).

NDJSON, not a JSON array, so the tuner can stream-parse without loading the whole window into memory.

**Acceptance criteria**

- A 6-hour window with 200 events returns 200 lines in chronological order.
- An out-of-range `from` (older than retention) returns events from the earliest available, not 404.
- A 30-day window returns 400 with a message naming the 7-day cap.

**Tests**

- `internal/api/handler_test.go` — happy path, empty window, oversized window, malformed `from`.

### W8 — Cross-cutting tests, metrics, observability

- All new code passes `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror` and ASan/UBSan/TSan clean.
- All new Go code passes `go vet`, `staticcheck`, and the existing CI golangci-lint config (add lint exclusions only with a comment justifying the exclusion).
- Coverage for new C inspector branches ≥ 90% (gcovr).
- Coverage for new Go packages ≥ 80%.
- Metrics added in W4/W5/W6 are exposed at `/metrics` and unit-tested via `testutil.CollectAndCount`.
- End-to-end test (`tests/integration/e2e_test.py`) extended with one scenario:
  - Spin up an embedded NATS server, an emd-agent with the fake-RTSP fixture, and a subscriber.
  - Trigger a motion event using the existing fixture.
  - Assert: the subscriber receives one event payload matching the schema, the event-log JSONL file contains the same event, and a GET on `/api/clips/{id}/thumbs` returns a JPEG.

## 4. Configuration changes summary

`agent.toml` example after all workstreams land:

```toml
[agent]
mode        = "agent"
instance_id = "edge-3"
data_dir    = "/var/lib/emd"

[runtime]
log_level     = "info"
clip_root     = "/var/lib/emd/clips"
inflight_root = "/var/lib/emd/inflight"

[recording]
container    = "mpegts"
fsync_policy = "on_close"

[disk]
max_bytes_per_camera = 1_000_000_000
retention_days       = 7

[eventlog]
enabled              = true
root                 = "/var/lib/emd/eventlog"
max_bytes_per_camera = 104_857_600
retention_days       = 30

[nats]
enabled        = true
url            = "nats://127.0.0.1:4222"
subject_prefix = "events"
site           = "site-warehouse-3"
creds_file     = "/etc/emd/nats.creds"

[cameras.axis_82_2]
url               = "rtsp://..."
transport         = "tcp"
codec_hint        = "h264"
buffer_seconds    = 15
pre_roll_seconds  = 6
post_roll_seconds = 8
clip_max_seconds  = 120
motion_z_high     = 3.0
intra_ratio_high  = 2.5
on_threshold      = 2
off_threshold     = 45
gradual_enabled   = false

# new tuning knobs (defaults shown — omit to disable)
min_bytes_threshold  = 0
bpf_relative_floor   = 0.0
z_high_warmup        = 0.0
z_high_warmup_frames = 0
target_classes       = ["person", "vehicle"]
```

## 5. Phasing

| Phase | Workstreams | Deliverable                                                                 |
|-------|-------------|-----------------------------------------------------------------------------|
| A     | W1, W2, W3  | Inspector knobs available; can be set via TOML or REST; tuner can fix the `axis_82_2` FP class today by writing `min_bytes_threshold = 2000`. |
| B     | W4, W7      | Event log on disk + REST endpoint to read it. Tuner can run "scan + search" against historical data without NATS. |
| C     | W5          | NATS publisher live. Post-processor can subscribe and observe; still in shadow until the autotune side ships. |
| D     | W6          | Thumbnails endpoint live. Post-processor can fast-vote on obvious FPs without pulling full clips. |
| E     | W8          | Test plan complete; coverage and CI gates green.                            |

Phase A is independently shippable and delivers immediate value. Phases B–D unblock the autotune component without forcing it to ship in lockstep.

## 6. Acceptance gates (per workstream)

A workstream is "done" when:

1. All listed files are landed and reviewed.
2. All listed tests are added and pass locally (`ctest` for C, `go test ./...` for Go).
3. ASan + TSan are clean for any new C code (`build-asan` and `build-tsan` from `CLAUDE.md` §How to Run Tests).
4. The acceptance-criteria checklist is verified manually and noted in the PR description.
5. CI passes including the existing license audit, banned-functions grep, and OpenAPI lint.

## 7. Risks specific to implementation

- **C ABI drift.** The `emd_event_t` extension in W4 is the riskiest change because it crosses the CGo boundary. Add the new fields at the end of the struct; do not reorder. Bump `EMD_ABI_VERSION_MINOR`. Add a CGo build-time assertion (`_Static_assert(sizeof(emd_event_t) == EXPECTED_SIZE, ...)`) on the Go side to catch silent drift.
- **NATS dependency footprint.** `nats.go` pulls in `golang.org/x/crypto`, `nkeys`, `nuid`. Confirm with the license-audit step in CI; all are Apache-2.0.
- **Thumbnail generator + libav.** First decoder on the edge. Keep the Go libav package scoped to `internal/thumbs` only; do not export decoder types from this package. If you go the `astiav` route, document the LGPL FFmpeg runtime requirement in `DEPLOYMENT_STATUS.md`.
- **Event log disk growth.** With 64 cameras at 1 event/sec each, the JSONL would be ~2.6 GB/day. The retention/size caps make this safe, but operators need to know — add a row to `K8S_DEPLOYMENT_SUMMARY.md`.

## 8. Out-of-scope reminders

- Do not add the gate, decoder model, SSIM, or S3 upload to the agent. Those live in `cvkitio/autotune`.
- Do not change the existing event flow timing — recorder, MQTT, and the existing event callback should all see events at the same point they see them today.
- Do not add a per-camera mutex anywhere.

## 9. Pointer to the consumer

The shape of the JSONL / NATS payload is the *contract* this component delivers. The consumer side lives at:

`/Users/andrewsinclair/workspace/cvkitio/autotune/docs/spec.md`

Any change to the JSON shape requires updating both specs in the same PR series. The autotune spec links back here.
