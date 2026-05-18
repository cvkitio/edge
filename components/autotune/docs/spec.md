# cvkit-autotune — Implementation Spec

Status: Ready for implementation
Owner: `cvkitio/autotune`
Last updated: 2026-05-18
Related design: [`cvkit/edge/docs/auto-tune-design.md`](../../cvkit/edge/docs/auto-tune-design.md) (revision 2)
Edge contract: [`cvkit/edge/docs/emd-tuning-reporting-spec.md`](../../cvkit/edge/docs/emd-tuning-reporting-spec.md)

## 0. Read this first

This spec is for an implementer building the `cvkitio/autotune` component from a green-field repository. The component is two Go binaries and a shared library that together do the off-edge work the design doc describes: SSIM/vision-model confirmation, S3 upload, parameter search, canary application.

If you're picking this up, read in this order:

1. The component [`README.md`](../README.md) — one-paragraph orientation.
2. The design doc above — *why* this component exists and the system shape.
3. The edge spec above — the *contract* this component consumes. Schemas of events, NATS subjects, REST endpoints. All of it.
4. This file — *what* to build, in what order.

You do not need to know C to work on this component. You do need to understand NATS JetStream, basic libav (or wrap a Go binding), an ONNX runtime, and the AWS SDK.

## 1. Scope

In scope.

- A new Go module rooted at `/Users/andrewsinclair/workspace/cvkitio/autotune` with two binaries (`cvkit-postproc`, `cvkit-autotune`) and shared `pkg/*` packages.
- A long-running post-processor that consumes raw motion events from NATS, runs a two-stage gate (OpenCV background-subtraction → SSIM → vision model), uploads confirmed clips to S3, and republishes confirmed / suppressed events back to NATS.
- An operator-initiated tuner CLI that pulls labelled event history from JetStream, runs a coarse grid + Bayesian refinement search, and applies the resulting per-camera config via the edge REST API with a canary.
- Pluggable vision back-ends: local ONNX (YOLOv8/RF-DETR), HuggingFace Inference Endpoints, OpenRouter multimodal LLMs.
- Deployment artefacts for Docker, Kubernetes (cloud GPU node), and systemd (on-prem).

Out of scope.

- Anything that runs on the edge. The edge spec covers all edge-side work.
- A new alerting / notification UI. Confirmed events get republished to NATS; downstream alert delivery (MQTT bridge, webhook fan-out, mobile push) is a separate component.
- Multi-tenant or RBAC concerns for the post-processor. Assume single-tenant per deployment.
- Continuous closed-loop tuning. Tuner runs are operator-initiated.

## 2. Hard contracts

You cannot change these without coordinating a PR series with `cvkit/edge`:

- **NATS subject schema** (defined in the edge spec §3, W5 and the design doc §5.2). Subjects: `events.raw.<site>.<camera>`, `events.confirmed.<site>.<camera>`, `events.suppressed.<site>.<camera>`, `clips.uploaded.<site>.<camera>`, `events.feedback.<site>.<camera>`, `config.history.<site>.<camera>`.
- **Event payload JSON shape** (edge spec W4). The `pkg/nats` event type is the canonical Go mirror of that schema. Do not add fields here without adding them to the edge first.
- **Edge REST surface** — `GET /api/cameras`, `GET /api/cameras/{name}/events`, `GET /api/cameras/{name}/config`, `PUT /api/cameras/{name}/config`, `GET /api/clips`, `GET /api/clips/{id}`, `GET /api/clips/{id}/thumbs`. Use the OpenAPI doc at `/docs/openapi.json` as the source of truth; generate clients from it.

## 3. Repository scaffold

```
autotune/
├── README.md
├── LICENSE                       # Apache-2.0
├── go.mod
├── go.sum
├── Makefile                      # build, test, lint, docker
├── .golangci.yml
├── cmd/
│   ├── cvkit-postproc/
│   │   └── main.go
│   └── cvkit-autotune/
│       └── main.go
├── pkg/
│   ├── nats/                     # JetStream wiring + schemas
│   │   ├── client.go
│   │   ├── schema.go             # Event, Suppressed, Confirmed, Feedback
│   │   ├── streams.go            # JetStream stream/consumer definitions
│   │   └── ...
│   ├── edge/                     # REST client to emd-agent
│   │   ├── client.go
│   │   ├── events.go             # GET /api/cameras/{n}/events
│   │   ├── clips.go              # GET /api/clips/{id}, /thumbs
│   │   └── config.go             # GET/PUT /api/cameras/{n}/config
│   ├── s3/                       # uploader
│   │   ├── uploader.go
│   │   └── key.go                # key_template rendering
│   ├── vision/                   # decoder + gates + models
│   │   ├── decoder.go            # libav seam
│   │   ├── ssim.go
│   │   ├── bgsub.go              # OpenCV-based background subtraction
│   │   ├── model.go              # ModelBackend interface
│   │   ├── onnx.go               # local ONNX (yolov8, rf-detr)
│   │   ├── huggingface.go
│   │   ├── openrouter.go
│   │   └── classes.go            # PERSON/VEHICLE/ANIMAL/OTHER mask helpers
│   ├── tuner/                    # confusion table + search
│   │   ├── confusion.go
│   │   ├── grid.go
│   │   ├── bayes.go
│   │   ├── objective.go
│   │   └── plan.go
│   ├── postproc/                 # pipeline orchestration
│   │   ├── worker.go
│   │   ├── pipeline.go
│   │   ├── deadline.go
│   │   └── metrics.go
│   └── obs/                      # logging, metrics, tracing
│       ├── log.go
│       └── metrics.go
├── deploy/
│   ├── docker/
│   │   ├── Dockerfile.postproc
│   │   └── Dockerfile.autotune
│   ├── k8s/
│   │   ├── postproc-deployment.yaml
│   │   ├── postproc-service.yaml
│   │   └── jetstream-config.yaml
│   └── systemd/
│       └── cvkit-postproc.service
├── configs/
│   ├── postproc.toml.example
│   └── tuner.yaml.example
├── testdata/
│   ├── clips/                    # small fixture clips
│   ├── events/                   # canonical event payloads
│   └── models/                   # tiny ONNX fixtures
└── docs/
    ├── spec.md                   # this file
    └── design.md                 # mirror of cvkit/edge/docs/auto-tune-design.md
```

Module path: `github.com/cvkitio/autotune`.

Go version: 1.22 (matching edge `go.mod` so toolchains line up).

License: Apache-2.0.

## 4. Workstreams

Twelve workstreams, ordered by dependency. W1–W3 are scaffolding; W4–W7 unlock the post-processor; W8–W10 are the tuner; W11–W12 are deployment and tests.

### W1 — Module scaffold + obs

**Files**

- `go.mod`, `Makefile`, `.golangci.yml`, `pkg/obs/log.go`, `pkg/obs/metrics.go`.

**Tasks**

- `go mod init github.com/cvkitio/autotune`. Pin `go 1.22`.
- Standard `Makefile` targets: `build`, `test`, `lint`, `cover`, `docker`, `clean`.
- `pkg/obs/log` wraps `log/slog` with JSON output by default, level controlled by env `LOG_LEVEL`.
- `pkg/obs/metrics` exposes a shared `*prometheus.Registry` and a `Serve(addr)` helper. Listener path `/metrics`, the listener is the same socket that exposes `/healthz` and `/readyz`.

**Acceptance**

- `go build ./...` succeeds with empty `cmd/cvkit-postproc/main.go` and `cmd/cvkit-autotune/main.go`.
- `make lint` passes (golangci-lint must include `govet`, `staticcheck`, `errcheck`, `ineffassign`).

### W2 — `pkg/nats`: client + schema

**Files**

- `pkg/nats/schema.go` — Go types for every payload on every subject.
- `pkg/nats/client.go` — connection wrapper.
- `pkg/nats/streams.go` — JetStream stream + consumer definitions.
- `pkg/nats/schema_test.go` — golden-file round-trip against `testdata/events/*.json`.

**Schema** (mirror exactly the edge spec W4)

```go
package nats

type Event struct {
    EventID         string    `json:"event_id"`
    Site            string    `json:"site"`
    Camera          string    `json:"camera"`
    CamID           uint16    `json:"cam_id"`
    Ts              time.Time `json:"ts"`
    TsMonoNS        uint64    `json:"ts_mono_ns"`
    Type            string    `json:"type"`           // motion|scene_change|idr_burst
    ZScore          float64   `json:"z_score"`
    IntraRatio      float64   `json:"intra_ratio"`
    Bytes           uint64    `json:"bytes"`
    BPFSlow         float64   `json:"bpf_slow"`
    BPFEwma         float64   `json:"bpf_ewma"`
    BPFVar          float64   `json:"bpf_var"`
    SinceKF         uint32    `json:"since_kf"`
    Reason          string    `json:"reason"`
    FSMBefore       string    `json:"fsm_before"`
    FSMAfter        string    `json:"fsm_after"`
    PTSStart        uint64    `json:"pts_start"`
    PTSEnd          uint64    `json:"pts_end"`
    Codec           string    `json:"codec"`
    FPS             float64   `json:"fps"`
    ClipID          string    `json:"clip_id"`
    ClipPath        string    `json:"clip_path"`
    TargetClassMask uint8     `json:"target_class_mask"`
    AgentVersion    string    `json:"agent_version"`
}

type Label struct {
    Classes    []string  `json:"classes"`     // e.g. ["person", "car"]
    MaxConf    float64   `json:"max_conf"`
    Model      string    `json:"model"`       // e.g. "yolov8n@int8"
    Frames     []FrameLabel `json:"frames"`   // per decoded sample frame
}

type FrameLabel struct {
    OffsetMS    uint32         `json:"offset_ms"`
    Detections  []Detection    `json:"detections"`
    SSIM        float64        `json:"ssim,omitempty"`
    BGSubArea   float64        `json:"bgsub_area,omitempty"` // 0..1, fraction of frame
}

type Detection struct {
    Class      string  `json:"class"`
    Confidence float64 `json:"confidence"`
    BBox       [4]float64 `json:"bbox"` // x1,y1,x2,y2 normalised
}

type ConfirmedEvent struct {
    Event
    Label       Label  `json:"label"`
    S3URL       string `json:"s3_url,omitempty"`
    ConfirmedBy string `json:"confirmed_by"`  // postproc id, or "deadline"
}

type SuppressedEvent struct {
    Event
    Label          *Label `json:"label,omitempty"`
    SuppressReason string `json:"suppress_reason"` // ssim_above|no_target_class|bgsub_low|disabled
    SuppressedBy   string `json:"suppressed_by"`
}

type UploadedClip struct {
    EventID string    `json:"event_id"`
    Site    string    `json:"site"`
    Camera  string    `json:"camera"`
    S3URL   string    `json:"s3_url"`
    SHA256  string    `json:"sha256"`
    Bytes   uint64    `json:"bytes"`
    Ts      time.Time `json:"ts"`
}

type Feedback struct {
    EventID string    `json:"event_id"`
    Site    string    `json:"site"`
    Camera  string    `json:"camera"`
    Verdict string    `json:"verdict"`   // "true_positive" | "false_positive" | "missed"
    Operator string   `json:"operator"`
    Ts      time.Time `json:"ts"`
    Note    string    `json:"note,omitempty"`
}
```

**Streams** (`pkg/nats/streams.go`)

Define `EnsureStreams(js nats.JetStreamContext, cfg StreamConfig) error` that creates/updates four streams:

| Stream      | Subjects               | Retention   | Max age | Storage |
|-------------|------------------------|-------------|---------|---------|
| `EVENTS`    | `events.>`             | Limits      | 14 days | File    |
| `CLIPS`     | `clips.uploaded.>`     | Limits      | 90 days | File    |
| `CONFIG`    | `config.history.>`     | Limits      | 365 days| File    |
| `FEEDBACK`  | `events.feedback.>`    | Limits      | 365 days| File    |

`max_msgs_per_subject` set per stream (events: 1M, clips: 100k, config: 100, feedback: 10k). Replication factor configurable; default 1 for single-node, 3 for production cluster.

**Client wrapper** (`pkg/nats/client.go`)

```go
type Client struct { ... }
func Dial(cfg Config) (*Client, error)        // opens conn + JetStream context
func (c *Client) Publish(subj string, b []byte) error  // sync ack on JS
func (c *Client) Subscribe(subj, durable string, h Handler) (*Subscription, error)
func (c *Client) Close()
```

`Handler` is `func(ctx context.Context, msg *Msg) error`. If the handler returns nil, the message is acked; if it returns an error, it is nak'd with backoff. The subscription respects `max_ack_pending` for back-pressure.

**Acceptance**

- A round-trip marshal/unmarshal test against `testdata/events/sample_motion.json` passes byte-identical on the field set (use `encoding/json` with `RawMessage` to assert no field drift).
- `EnsureStreams` is idempotent — running it twice on the same JetStream produces no errors and no changes.

**Tests**

- Embedded NATS test server (`natsserver.RunRandClientPortServer` with JetStream enabled in tmpdir) — connect, ensure streams, publish, subscribe with durable, receive, ack.
- `schema_test.go` — golden round-trip.

### W3 — `pkg/edge`: REST client

**Files**

- `pkg/edge/client.go`, `events.go`, `clips.go`, `config.go`, `client_test.go`.

**Client shape**

```go
type Client struct { ... }
func New(baseURL string, opts ...Option) *Client

// /api/cameras
func (c *Client) ListCameras(ctx context.Context) ([]string, error)

// /api/cameras/{name}/events
func (c *Client) StreamEvents(ctx context.Context, cam string, from, to time.Time) (<-chan nats.Event, <-chan error)

// /api/cameras/{name}/config
func (c *Client) GetConfig(ctx context.Context, cam string) (*InspectorConfig, error)
func (c *Client) UpdateConfig(ctx context.Context, cam string, patch *InspectorConfigPatch) error

// /api/clips
func (c *Client) FetchClip(ctx context.Context, clipID string) (io.ReadCloser, error)
func (c *Client) FetchThumbs(ctx context.Context, clipID string) (io.ReadCloser, error)
```

`InspectorConfig` and `InspectorConfigPatch` mirror the edge OpenAPI exactly. `InspectorConfigPatch` uses pointer fields for partial updates.

`StreamEvents` consumes the NDJSON endpoint with a `bufio.Scanner`, decoding one event per line into the channel. Closes both channels on EOF or ctx-cancel.

**Per-site routing**

A `Bridges` map in postproc config translates `site` → base URL (the design doc `[edge.bridges]` block). The post-processor instantiates one `*edge.Client` per site at startup.

**Acceptance**

- `httptest.NewServer` test: stubbed `/api/cameras/x/events` returning NDJSON yields the expected event count on the channel.
- A 4xx from the edge produces a typed error that the caller can `errors.As`.
- A network failure during `FetchClip` is wrapped with the URL it was hitting.

**Tests**

- `client_test.go` — happy path for each method, plus one error case per method.

### W4 — `pkg/vision/decoder`: libav seam

**Files**

- `pkg/vision/decoder.go` — interface.
- `pkg/vision/decoder_astiav.go` — first implementation using `github.com/asticode/go-astiav` (LGPL FFmpeg dynamic-link).
- `pkg/vision/decoder_test.go` — fixture-based tests.

**Interface**

```go
type Decoder interface {
    // Open returns a stream-handle for a clip on disk (or in a Reader-at).
    Open(r io.ReaderAt, size int64) (Stream, error)
}

type Stream interface {
    // Sample returns up to n decoded frames spread evenly across the clip.
    // Each frame is downscaled to maxWidth preserving aspect.
    // Frames are returned in PTS order.
    Sample(n int, maxWidth int) ([]Frame, error)
    Close() error
}

type Frame struct {
    OffsetMS uint32
    Width    int
    Height   int
    PixFmt   PixFmt  // YUV420P, RGB24
    Data     []byte  // packed; Stride = Width * bytesPerPixel(PixFmt)
}
```

**Implementation notes**

- Hard-code H.264 and H.265 codec opens. Other codecs return `ErrUnsupportedCodec`.
- Frames are allocated from a `sync.Pool` keyed by `(width, height, pixfmt)` to avoid GC pressure under load.
- The decoder never seeks past EOF, never decodes audio, never decodes B-frames more aggressively than necessary to satisfy `Sample`.

**Acceptance**

- Decoding a 5-second fixture clip with `Sample(3, 320)` returns three frames at evenly-spaced offsets (within ±50 ms tolerance).
- Concurrent decode of 16 clips on 4 cores does not panic and completes within a generous deadline.

**Tests**

- `decoder_test.go` with two fixture clips (h264 and h265).

### W5 — `pkg/vision/gates`: SSIM + BG-sub

**Files**

- `pkg/vision/ssim.go`, `bgsub.go`, `gates_test.go`.

**SSIM**

Pure-Go implementation against `Frame` (no extra deps). Compares two YUV420P frames on the Y plane only (chroma is too noisy for FP rejection). Implement the Wang et al. 2004 formula with the standard 8×8 sliding window. Public API:

```go
func SSIM(a, b Frame) (float64, error)
```

Use a single `[]float64` scratch buffer to avoid allocs per call. Validate frames have matching dims; error if not.

**Background subtraction**

OpenCV is the only practical fast path. Use `github.com/hybridgroup/gocv` (Apache-2.0, wraps OpenCV via cgo). Public API:

```go
type BGSub struct { ... }
func NewBGSub() (*BGSub, error)         // wraps MOG2
func (b *BGSub) Apply(f Frame) (areaFraction float64, err error)
func (b *BGSub) Close() error
```

`Apply` returns the fraction of pixels (0..1) considered foreground after morphological opening (3×3, one iteration). The caller maintains one `BGSub` per camera (it has internal state across calls). The post-processor instantiates these in `pkg/postproc/worker.go` per-camera.

**Acceptance**

- SSIM of two identical frames is 1.0 exactly.
- SSIM of a frame and that frame plus uniform Gaussian noise σ=10 is < 0.98.
- BGSub on a sequence "100 static frames then a person walks across" produces area_fraction < 0.01 for the static phase and > 0.05 for the walking phase.

**Tests**

- `gates_test.go` with synthetic frames generated in the test (no external fixtures needed for the SSIM tests; one short h264 fixture for the BGSub walking test).

### W6 — `pkg/vision/model`: ONNX + HF + OpenRouter

**Files**

- `pkg/vision/model.go` — interface + factory.
- `pkg/vision/onnx.go` — onnxruntime-go backend.
- `pkg/vision/huggingface.go` — Inference Endpoint backend.
- `pkg/vision/openrouter.go` — multimodal LLM backend.
- `pkg/vision/classes.go` — class enum + bitmask helpers.
- `pkg/vision/model_test.go` — table-driven with a fake backend.

**Interface**

```go
type ModelBackend interface {
    // Detect runs the model on each frame and returns FrameLabel per frame.
    Detect(ctx context.Context, frames []Frame, allowedClasses uint8) ([]natschemas.FrameLabel, error)
    Name() string  // e.g. "yolov8n@int8"
    Close() error
}

type Config struct {
    Backend    string   // "onnxruntime" | "huggingface" | "openrouter"
    ModelPath  string   // for onnxruntime
    ModelID    string   // for huggingface / openrouter
    Endpoint   string   // hf/openrouter URL
    APIKey     string   // hf/openrouter
    GPU        string   // "" | "cuda:0" | "metal"
    BatchSize  int
    Classes    map[string]bool   // model output class → enabled
    MaxRPS     float64           // rate limit for hf/openrouter
    MonthlyUSD float64           // budget cap for openrouter
}

func New(cfg Config) (ModelBackend, error)
```

**ONNX backend**

Use `github.com/yalue/onnxruntime_go`. YOLOv8 input layout is NCHW float32, 640×640, RGB normalised to [0,1]. Output is `[1, 84, 8400]` (4 box coords + 80 class scores per anchor). Apply confidence threshold (`class_min_conf` from gate config), NMS at 0.45 IoU, map class indices to class names via COCO labels file.

GPU selection: if `cfg.GPU == "cuda:0"`, request CUDA execution provider; fall back to CPU with a logged warning if CUDA is not built in. Same pattern for CoreML on darwin.

**HuggingFace backend**

POST decoded frame as JPEG to the endpoint URL with `Authorization: Bearer <api_key>`. Parse response (HF object-detection schema). Rate-limit with `golang.org/x/time/rate` per the `MaxRPS` config.

**OpenRouter backend**

Compose a 3-frame grid into one JPEG, POST as a multimodal message asking the model to enumerate detected target classes with confidence. Parse a structured JSON response (model is instructed to return JSON). Token cost tracked in a per-call counter; sum against `MonthlyUSD` cap and refuse calls past the cap (with a metric).

**Acceptance**

- A fake backend that returns "person, 0.9" on one frame and "nothing" on another produces a correct `[]FrameLabel`.
- ONNX backend on a YOLOv8-n model + a fixture frame containing a person produces a `person` detection with confidence > 0.5.
- HF backend with a stubbed HTTP server respects the rate limit.

**Tests**

- `model_test.go` — fake-backend table tests + one real ONNX test gated by `-tags onnx` build constraint so CI without the runtime can still build the package.

### W7 — `pkg/postproc`: pipeline

**Files**

- `pkg/postproc/pipeline.go` — per-event pipeline.
- `pkg/postproc/worker.go` — per-camera worker.
- `pkg/postproc/deadline.go` — per-event deadline enforcement.
- `pkg/postproc/metrics.go` — Prometheus collectors.
- `pkg/postproc/pipeline_test.go`.

**Per-event pipeline** (called from worker)

```go
func (p *Pipeline) Handle(ctx context.Context, evt nats.Event) error {
    ctx, cancel := context.WithTimeout(ctx, p.cfg.Gate.Deadline)
    defer cancel()

    // 1) Cheap thumbs vote
    thumbs, err := p.edge.FetchThumbs(ctx, evt.ClipID)
    if err == nil {
        if frames := parseThumbStrip(thumbs); len(frames) > 0 {
            if area := p.bgsub[evt.Camera].Apply(frames[0]); area < p.cfg.Gate.BGMinAreaPct {
                return p.publishSuppressed(evt, "bgsub_low", &Label{...})
            }
        }
    }

    // 2) Decode for SSIM + model
    clip, err := p.edge.FetchClip(ctx, evt.ClipID)
    if err != nil { return p.publishConfirmed(evt, "fetch_error", nil) }
    defer clip.Close()
    stream, err := p.decoder.Open(asReaderAt(clip))
    if err != nil { return p.publishConfirmed(evt, "decode_error", nil) }
    defer stream.Close()
    frames, err := stream.Sample(3, 320)
    if err != nil { return p.publishConfirmed(evt, "sample_error", nil) }

    // 3) SSIM vs baseline
    if baseline, ok := p.baselines.Get(evt.Camera); ok {
        if ssim, _ := SSIM(frames[0], baseline); ssim > p.cfg.Gate.SSIMSkipThreshold {
            return p.publishSuppressed(evt, "ssim_above", buildLabel(frames, ssim, nil))
        }
    }

    // 4) Model
    labels, err := p.model.Detect(ctx, frames, evt.TargetClassMask)
    if err != nil { return p.publishConfirmed(evt, "model_error", nil) }
    if !anyQualifyingDetection(labels, p.cfg.Gate.ClassMinConf) {
        if !p.cameraIsSlowTarget(evt.Camera) {
            return p.publishSuppressed(evt, "no_target_class", buildLabel(frames, 0, labels))
        }
    }

    // 5) Upload + confirm
    s3URL := ""
    if p.cfg.S3.Enabled {
        s3URL, err = p.s3.Upload(ctx, clipReader, evt)
        if err != nil {
            return p.publishConfirmed(evt, "uploaded_error", buildLabel(frames, 0, labels))
        }
    }
    p.baselines.Update(evt.Camera, frames[0])
    return p.publishConfirmed(evt, p.id, buildLabel(frames, 0, labels), s3URL)
}
```

**Per-camera worker**

```go
type Worker struct { ... }
func NewWorker(cam string, p *Pipeline, sub *nats.Subscription) *Worker
func (w *Worker) Run(ctx context.Context) error
```

Each worker is one goroutine consuming from a per-camera durable subscription. Concurrency cap is implicit in NATS `max_ack_pending`.

**Per-event deadline**

When the context deadline fires before publish, the pipeline publishes a CONFIRMED event with `confirmed_by="deadline"` and `Label{}` empty. Fail-open. Increment `cvkit_postproc_deadline_total{camera}`.

**Metrics**

- `cvkit_postproc_events_total{site,camera,result}` — result in `{confirmed,suppressed,deadline}`.
- `cvkit_postproc_event_duration_seconds{site,camera,stage}` — histogram, stages in `{thumbs,decode,ssim,model,upload}`.
- `cvkit_postproc_suppressed_total{site,camera,reason}`.
- `cvkit_postproc_model_inference_seconds{model}`.
- `cvkit_postproc_s3_upload_seconds`.
- `cvkit_postproc_s3_upload_bytes_total{site,camera}`.

**Acceptance**

- A synthetic event with thumbs that BGSub votes "low area" produces a suppressed event in < 50 ms.
- A synthetic event that the model finds "person" on produces a confirmed event with `s3_url` set.
- A model backend that hangs is killed by the deadline and produces a confirmed (fail-open) event with `confirmed_by="deadline"`.

**Tests**

- `pipeline_test.go` with fakes for `pkg/edge`, `pkg/vision/*`, `pkg/s3`. Table-driven scenarios covering each branch above.

### W8 — `pkg/s3`: uploader

**Files**

- `pkg/s3/uploader.go`, `key.go`, `uploader_test.go`.

**Tasks**

- Use `github.com/aws/aws-sdk-go-v2`. Multipart upload for clips > 8 MB.
- `key_template` renders with Go template syntax: `{{.Site}}/{{.Camera}}/{{.Date}}/{{.EventID}}.ts`. Date is `YYYY/MM/DD` from `evt.Ts`.
- Server-side encryption configurable (`AES256` default; `aws:kms` with KMS key id).
- Storage class configurable (`STANDARD`, `STANDARD_IA`, `INTELLIGENT_TIERING`).
- Public API:

```go
type Uploader struct { ... }
func New(cfg Config) (*Uploader, error)
func (u *Uploader) Upload(ctx context.Context, body io.Reader, size int64, evt nats.Event) (s3URL string, err error)
func (u *Uploader) Close() error
```

**Acceptance**

- Upload of a 1 MB body to a `minio` test container produces an object at the rendered key.
- Multipart upload of a 32 MB body completes.
- An IAM error is wrapped with the bucket/key it was targeting.

**Tests**

- `uploader_test.go` using `github.com/minio/minio-go/v7` test server or a stubbed `s3iface`. Skip in CI if neither is available; mark with `-tags integration`.

### W9 — `cmd/cvkit-postproc`

**Files**

- `cmd/cvkit-postproc/main.go`, `configs/postproc.toml.example`.

**Wiring**

```go
func main() {
    cfg := config.LoadOrExit(os.Args)
    log := obs.NewLogger(cfg.LogLevel)

    nc, _ := nats.Dial(cfg.NATS)
    defer nc.Close()
    if err := nats.EnsureStreams(nc.JS(), cfg.Streams); err != nil { log.Fatal(err) }

    decoder, _ := vision.NewDecoder()
    model,   _ := vision.NewModel(cfg.Model)
    defer model.Close()
    s3,      _ := s3pkg.New(cfg.S3)
    edges    := edge.NewBridges(cfg.Edge.Bridges)

    p := postproc.NewPipeline(postproc.Deps{
        NATS: nc, Edge: edges, Decoder: decoder, Model: model, S3: s3,
        Cfg:  cfg.Gate,
    })

    // One worker per camera per site
    sites := discoverSites(ctx, edges) // GET /api/cameras on each bridge
    for site, cams := range sites {
        for _, cam := range cams {
            sub, _ := nc.Subscribe(
                fmt.Sprintf("events.raw.%s.%s", site, cam),
                fmt.Sprintf("postproc-%s-%s-%s", cfg.ID, site, cam),
                p.Handler(site, cam),
            )
            go postproc.NewWorker(cam, p, sub).Run(ctx)
        }
    }

    obs.ServeMetrics(":9465")
    waitForSignal(ctx)
}
```

**Config file** (`configs/postproc.toml.example`)

See the design doc §5.3 for the full layout. The implementer should regenerate it from the loaded `Config` struct so it stays in sync.

**Health checks**

- `/healthz` — always 200 if the process is up.
- `/readyz` — 200 if NATS is connected AND at least one edge bridge is reachable AND the model is loaded.

**Acceptance**

- Cold-start: binary connects to NATS, ensures streams, discovers cameras from each edge bridge, subscribes, and is ready in < 5 seconds.
- SIGTERM: cleanly drains in-flight messages within `cfg.ShutdownTimeout` (default 30s), then exits 0.
- A misconfigured S3 bucket logs a clear error and exits non-zero before any subscriptions are made (fail-fast).

### W10 — `pkg/tuner` + `cmd/cvkit-autotune`

**Files**

- `pkg/tuner/confusion.go` — build confusion table from labelled events.
- `pkg/tuner/grid.go` — coarse grid search.
- `pkg/tuner/bayes.go` — Bayesian refinement.
- `pkg/tuner/objective.go` — F-beta with hard recall constraint.
- `pkg/tuner/plan.go` — produce a `Plan` artefact (JSON).
- `cmd/cvkit-autotune/main.go` — Cobra-style CLI with `scan`, `label`, `search`, `apply`, `rollback` subcommands.

**Confusion table**

Input: a slice of `(Event, Label)` from JetStream (raw + confirmed/suppressed joined by `event_id`). Optional feedback overrides from `events.feedback`. Output:

```go
type LabelledEvent struct {
    Event nats.Event
    Label nats.Label   // empty if unknown
    Verdict Verdict    // TruePositive | FalsePositive | Unknown
}
```

Verdict resolution:
- If feedback exists for this event, use it.
- Else if model max_conf ≥ class_min_conf and at least one detected class ∈ camera's `target_class_mask`, → TruePositive.
- Else → FalsePositive.
- If no label exists at all (raw never confirmed/suppressed), Unknown — exclude from the table.

**Grid search**

```go
type Candidate struct {
    MotionZHigh        float64
    IntraRatioHigh     float64
    MinBytesThreshold  uint32
    OnThreshold        uint8
    SSIMSkipThreshold  float64
}

func Grid(events []LabelledEvent) []Candidate
func Evaluate(events []LabelledEvent, c Candidate) Confusion
```

Sweep:
- `MotionZHigh ∈ {2.5, 3.0, 3.5, 4.0, 4.5, 5.0}`
- `IntraRatioHigh ∈ {2.0, 2.5, 3.0, 3.5}`
- `MinBytesThreshold ∈ {0, 500, 1000, 2000, 4000, 8000}`
- `OnThreshold ∈ {1, 2, 3}`
- `SSIMSkipThreshold ∈ {0.97, 0.98, 0.985, 0.99}`

~1700 combinations. Evaluate is O(N) per combination; 1700 × 1000 events × 5 µs = ~8 seconds. Fast enough to skip parallelism.

**Bayesian refinement**

Take the top-10 grid candidates by objective, run a small GP-EI search over the continuous params (`MotionZHigh`, `IntraRatioHigh`, `SSIMSkipThreshold`) for ~50 iterations. Use a lightweight Go GP (`github.com/peterbourgon/gpopt` or implement a minimal Matern-5/2 + EI directly — the search space is 3D and the eval is cheap, so a hand-rolled solution is acceptable).

**Objective**

F-beta with β=0.5 (precision-weighted). Hard constraint: recall on the labelled-TP set may not drop by more than `--max-recall-loss` (default 2 percentage points) below the current config's recall.

```go
func FBeta(c Confusion, beta float64) float64
func IsAcceptable(c Confusion, baseline Confusion, maxRecallLoss float64) bool
```

If the camera has `slow_target == true`, append synthetic TP events to the table (configurable via `tuner.yaml`) so the constraint protects them.

**Plan**

```go
type Plan struct {
    RunID         string
    Camera        string
    Site          string
    Generated     time.Time
    Baseline      Snapshot
    Proposed      Snapshot
    ExpectedFPDrop float64
    ExpectedTPLoss float64
    FlippedTPtoFN  []nats.Event   // up to 10 samples
    FlippedFPtoTN  []nats.Event   // up to 10 samples
}

type Snapshot struct {
    Config InspectorConfig
    Confusion Confusion
}
```

`plan.go` writes/reads the plan as JSON. The CLI prints a human summary; `--json` prints the structured form for machine consumption.

**CLI** (use `github.com/spf13/cobra`)

```
cvkit-autotune scan      --nats <url> --camera <name>  --since 7d  --out events.jsonl
cvkit-autotune label     --in events.jsonl --model <id> --out labelled.jsonl
cvkit-autotune search    --in labelled.jsonl --camera <name> --out plan.json
cvkit-autotune apply     --plan plan.json --agent http://edge:8080 [--dry-run] [--no-canary]
cvkit-autotune rollback  --agent http://edge:8080 --camera <name> --run-id <id>
```

Each subcommand exits non-zero on error with a clear message. `apply` runs the canary by default:

1. PUT new config.
2. Subscribe to `events.confirmed.<site>.<camera>` for 15 minutes.
3. Compare rate to baseline (10-minute rolling pre-window stored in `plan.json`).
4. If outside `[0.3x, 3x]` band, PUT baseline back, exit 1 with a clear log line.
5. Else PUT current config to `config.history.<site>.<camera>` with `run_id` and exit 0.

**Acceptance**

- `search` on a synthetic 1000-event dataset (300 TP, 700 FP) produces a plan with `ExpectedFPDrop > 0.5` while `ExpectedTPLoss < 0.02`.
- `apply --dry-run` prints a diff and does not call PUT.
- `rollback` restores any previous config in the JetStream history.

**Tests**

- `pkg/tuner/*_test.go` — confusion correctness, grid search reproducibility, F-beta math, constraint enforcement.
- `cmd/cvkit-autotune/*_test.go` — integration test exercising `scan → label → search → apply` against an embedded NATS + a stub edge HTTP server.

### W11 — Deployment artefacts

**Docker**

- `deploy/docker/Dockerfile.postproc` — multi-stage. Stage 1 builds the Go binary against `golang:1.22-alpine` (or `nvidia/cuda:12-devel` if GPU support is needed). Stage 2 minimal runtime: `ubuntu:22.04` with `libavcodec`, `libavformat`, `libavutil`, `libopencv-core`, optional `libcuda` if GPU. The binary at `/usr/local/bin/cvkit-postproc`. ENTRYPOINT `cvkit-postproc --config /etc/cvkit/postproc.toml`.
- `deploy/docker/Dockerfile.autotune` — same builder, smaller runtime (`gcr.io/distroless/base-debian12`). The CLI binary at `/usr/local/bin/cvkit-autotune`.

**Kubernetes**

- `deploy/k8s/postproc-deployment.yaml` — Deployment with `nvidia.com/gpu: 1` resource request when GPU is desired. Liveness on `/healthz`, readiness on `/readyz`. ConfigMap for `postproc.toml`. Secret for NATS creds, AWS creds, model API keys.
- `deploy/k8s/postproc-service.yaml` — ClusterIP Service exposing `:9465` for Prometheus scrape.
- `deploy/k8s/jetstream-config.yaml` — Job that runs `cvkit-postproc ensure-streams` once at install.

**systemd**

- `deploy/systemd/cvkit-postproc.service` — `Type=notify` if the binary supports sd_notify; otherwise `Type=simple`. `Restart=always`, `User=cvkit`, hardened (`ProtectSystem=strict`, `ProtectHome=true`, `NoNewPrivileges=true`).

**Acceptance**

- `docker build -f deploy/docker/Dockerfile.postproc .` produces an image under 500 MB without GPU support, under 4 GB with CUDA.
- `kubectl apply -f deploy/k8s/` with appropriate secrets produces a Running pod within 60 seconds.

### W12 — Cross-cutting tests + CI

- All Go code passes `go vet`, `staticcheck`, `errcheck`, `ineffassign`, `gosec`.
- Coverage ≥ 80% for `pkg/postproc`, `pkg/tuner`, `pkg/nats`, `pkg/edge`. ≥ 60% acceptable for `pkg/vision` (model backends have integration tests gated by build tags).
- Integration test: `tests/integration/e2e_test.go` brings up embedded NATS + minio + a stub edge HTTP server, fires 100 synthetic events through the pipeline, asserts the right balance of confirmed/suppressed publishes and S3 PUTs.
- Tuner end-to-end test: synthetic 1000-event dataset → `search` → `apply` (against the stub edge) → assert the agent receives the new config.
- CI matrix:
  - `lint` — golangci-lint.
  - `unit` — `go test ./...` (no GPU, no docker).
  - `integration` — `go test -tags=integration ./...` (brings up minio + NATS via testcontainers-go).
  - `docker` — build both images on every PR.
  - License audit — `go-licenses` ensures no GPL deps.

## 5. Configuration files

### `configs/postproc.toml.example`

The full example lives in the design doc §5.3; the implementer copies it verbatim and adds inline comments. Validate at load time:

- NATS URL is reachable (warn but do not fail if it isn't — the client will reconnect).
- At least one `[edge.bridges.*]` entry exists.
- If `s3.enabled = true`, `s3.bucket` and credentials env vars are non-empty.
- Model backend config is consistent (`backend = "onnxruntime"` requires `model_path` exists on disk).

### `configs/tuner.yaml.example`

```yaml
default_target_classes:
  - person
  - vehicle

slow_target_cameras:
  - name: north_perimeter
    synthetic_tp_events:
      - { z_score: 1.2, intra_ratio: 1.1, bytes: 380, label: person }
      - { z_score: 1.4, intra_ratio: 1.0, bytes: 400, label: person }

search:
  objective: f0.5
  max_recall_loss: 0.02
  min_events: 200

apply:
  canary_window_minutes: 15
  acceptable_rate_band: [0.3, 3.0]
```

## 6. Hard ordering

W1 → W2 → W3 unblock W4–W7 (post-processor depends on all three).
W4, W5, W6 can be parallel after W3 lands.
W7 depends on W4, W5, W6, W8.
W9 depends on W7.
W10 depends on W2 and W3 (it shares the same packages).
W11 depends on W9 and W10.
W12 runs throughout.

A reasonable two-developer split: one drives W1→W2→W4→W5 (NATS + decoder + gates); the other drives W3→W6→W8 (edge client + model + S3); they converge on W7 and W9; one of them takes W10 while the other finalises W11 and W12.

## 7. Open issues for the implementer to resolve

- **Decoder choice.** First implementation is `go-astiav` (LGPL FFmpeg dynamic-link). If the edge ever ships `emd_decoder_libav` as a stable plugin, migrate `pkg/vision/decoder` to consume it via CGo so we have one decoder seam across the system. Note in `pkg/vision/decoder.go` and re-flag in W4.
- **GP implementation.** Bayesian refinement can use an external library or a hand-rolled Matern-5/2 + EI. A hand-rolled implementation is ~200 lines and removes a dep — recommended if the implementer is comfortable with GPs.
- **Operator feedback ingestion.** The edge spec does not (yet) describe how operators submit feedback to `events.feedback.<site>.<camera>`. The first sketch is a small HTTP endpoint on the post-processor; if the project has a separate review UI, that UI publishes directly. Resolve in the first sprint.
- **Per-class detection thresholds.** Currently one `class_min_conf` applies to all classes. Some sites will want a higher bar for `dog` than `person`. Leave a TODO in `pkg/postproc/pipeline.go` and revisit after the first deployment.

## 8. Out-of-scope reminders

- Do not modify anything in `cvkit/edge`. If the edge contract is wrong, file an issue and update the edge spec — do not patch around it here.
- Do not add long-term state to the post-processor. Per-camera SSIM baselines are in-memory only; the source of truth is JetStream and S3.
- Do not bypass NATS. Every event the post-processor handles must come from a NATS subscription; every confirmed/suppressed verdict must be a NATS publish. Direct HTTP from edge to post-processor is forbidden.

## 9. Pointer to the producer

The shape of every event payload this component consumes is defined in:

`/Users/andrewsinclair/workspace/cvkitio/cvkit/edge/docs/emd-tuning-reporting-spec.md`

Any change to the JSON shape requires updating both specs in the same PR series. The edge spec links back here.
