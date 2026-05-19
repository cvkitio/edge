# Auto-Tune Design — Reducing False Positives in the Edge Motion Detector

Status: Draft for review (revision 2)
Owner: TBD
Last updated: 2026-05-18

Revision history.
- r1 — initial design with in-process two-stage gate inside `emd-agent`.
- r2 — gate and tuner extracted into a separate `cvkitio/autotune` component, decoupled from edge via NATS pub/sub so the post-processor can run cloud-side on GPU hardware and absorb blocking work (S3 upload, larger models) without back-pressuring the edge.

## 1. Problem

The encoded-domain z-score detector in `emd_inspector` is doing its job: it correctly fires when bytes-per-frame moves a statistically significant distance from the per-camera baseline. The trouble is that on quiet scenes the baseline is *very* low (e.g. `axis_82_2` settles at ~354 B/frame), so visually insignificant events — auto-exposure changes, a leaf, a moth on the IR illuminator — become z-score outliers and cut clips.

`docs/archive/FALSE_POSITIVE_ANALYSIS.md` already enumerates the root cause and proposes a static per-camera override (raise `motion_z_high`, add a `min_bytes_threshold`). That is the right shape of fix; the missing piece is choosing those numbers *automatically*, per camera, from evidence — and doing so without violating the hot-path invariants that make `emd` cheap.

A second class of problem the operator pointed out: a true positive that the encoded-domain detector will never catch on its own. A sniper crawling in a ghillie suit at 5 cm/s produces almost no inter-frame delta — the encoder will happily P-frame it at 354 bytes. We do not want the auto-tuner to suppress those by making every camera less sensitive. The tuner has to know what the camera is *for* before it decides what to suppress.

## 2. Goals and non-goals

Goals.

1. Drive per-camera false-positive rate down significantly (target: ≥ 70% reduction on representative static cameras) without losing true positives the system already catches.
2. Add a frame-similarity / object-class confirmation stage that runs *after* the cheap z-score detector fires, decoupled from the edge so it can run cloud-side on GPU hardware when that is the right place to put it.
3. Tune per *target class*: a camera labelled "person, vehicle" gets tuned differently than one labelled "person, animal, slow-moving" (the ghillie-suit case).
4. Make tuning a one-shot CLI: `cvkit-autotune --camera axis_82_2 --target person,vehicle --since 7d` should produce a config diff that can be reviewed, then PUT back via the edge agent's existing `/api/cameras/{name}/config` endpoint.
5. Preserve every hot-path invariant in `edge/CLAUDE.md` — no decode on the camera worker, no GPL in the production edge binary, no per-NAL malloc, the inspector remains a pure C statistical function.
6. Absorb the inevitable blocking work — S3 upload of clips, large-model inference, third-party API calls (HuggingFace, OpenRouter) — *off* the edge process so a slow upload or a stalled model never affects ingest.

Non-goals.

- Replacing the z-score detector. The whole point is that it is fast; we are wrapping it, not displacing it.
- Building a new tracker, multi-object tracker, or alerting pipeline. Auto-tune just classifies events and emits config.
- On-device GPU inference at the edge. The first release runs the validator off-edge; on-edge GPU is a deployment option for future revisions.
- Closed-loop continuous tuning. Phase 1 is operator-initiated and operator-approved.

## 3. Architecture overview

```
       edge node                         pub/sub                    cvkitio/autotune
   ┌──────────────────┐         ┌───────────────────┐      ┌────────────────────────────┐
   │ emd-agent (Go)   │         │  NATS (JetStream) │      │  post-processor (worker)   │
   │ + libemd (C)     │         │                   │      │  - subscribes events.raw   │
   │                  │  publish│  events.raw.>     │ sub  │  - fetches clip            │
   │  inspector ──────┼────────▶│  events.confirmed │◀─────┤  - decode (libav)          │
   │  recorder ──────▶│ clip on │  clips.uploaded   │ pub  │  - SSIM + YOLO / DETR /    │
   │  REST /api/...   │   disk  │                   │      │    OpenCV BG-sub gate      │
   │                  │         └───────────────────┘      │  - S3 upload (blocking ok) │
   │  GET /clips/{id} │◀──fetch clip───────────────────────┤  - publishes confirmed +   │
   │  PUT /cameras/   │                                    │    uploaded events         │
   │       {n}/config │◀─── tuner applies new config ──────┤                            │
   └──────────────────┘                                    │  tuner (CLI / cron):       │
                                                           │  - scans confirmed events  │
                                                           │  - labels with model       │
                                                           │  - grid + Bayesian search  │
                                                           │  - emits config plan       │
                                                           │  - canary apply via REST   │
                                                           └────────────────────────────┘
                                                                 ▲              │
                                                                 │              ▼
                                                          GPU optional         S3
```

**Edge** does not change shape. It gains exactly three things: a NATS publisher in the existing event aggregator, a thumbnail strip cached next to each clip, and the extra tuner-writable fields in the inspector config (§4). Everything else — decode, vision models, S3, NATS subscription, grid search — lives in `cvkitio/autotune`. The edge binary stays clean of LGPL/GPL, free of vision deps, and free of S3 credentials.

**NATS** is the seam. It has JetStream for durability so a post-processor restart doesn't drop events, it has leaf-node support so an on-prem edge cluster can run a local broker that mirrors to a cloud broker (no direct egress from edge), and the Go client is small. The choice over MQTT-as-bus (the system already speaks MQTT for operator alerts) is that NATS gives us subject hierarchies, request/reply, and JetStream replay without us standing up a separate broker class for this — MQTT stays for downstream alert delivery to operator tools.

**cvkitio/autotune** is one Go repository, two executables, one shared library:

- `cmd/cvkit-postproc` — long-running NATS subscriber that does Stage-2 confirmation, S3 upload, and republishes confirmed events. Deployable as a sidecar container on the edge node, a node-local systemd service, or a cloud Kubernetes Deployment. Same binary; deployment shape is a configuration choice.
- `cmd/cvkit-autotune` — operator-initiated CLI that scans confirmed events from JetStream history, runs the parameter search, and applies via the edge REST API.
- `pkg/vision` — shared package: libav decoder seam, OpenCV BG-subtraction, ONNX runtime wiring, model-zoo bindings (YOLOv8, RF-DETR, Owl-ViT, HF endpoints, OpenRouter LLM).

The two executables share the vision package on purpose: the post-processor's confidence scores and labels are exactly what the tuner needs to optimise against, so reusing one labeller keeps online and offline behaviour identical.

## 4. Detection model after auto-tune

The hot path stays z-score plus debounce, but we extend the per-camera C config with parameters the tuner is allowed to write:

```
emd_inspector_cfg_t {
    // existing
    double   motion_z_high;
    double   intra_ratio_high;
    uint8_t  on_threshold;
    uint8_t  off_threshold;
    ...
    // new — settable by tuner
    uint32_t min_bytes_threshold;       // absolute BPF floor; suppress below
    double   bpf_relative_floor;        // multiplier on bpf_slow; suppress below k×baseline
    double   z_high_warmup;             // higher threshold for first N frames after IDR
    uint16_t z_high_warmup_frames;
    uint8_t  target_class_mask;         // bitfield: PERSON|VEHICLE|ANIMAL|...
}
```

The first two suppress the static-camera FPs documented in `FALSE_POSITIVE_ANALYSIS.md` cheaply, in the C inspector, before any event leaves the worker thread. `z_high_warmup` / `z_high_warmup_frames` addresses the IDR-burst artefact (the encoder briefly inflates after a keyframe, faking a z-score outlier). `target_class_mask` is metadata — the C inspector ignores it — but it is plumbed through the published event so the downstream post-processor can filter detections by class.

Above the inspector the *two-stage detection model* now looks like this:

1. **Stage 1 (on-edge, C inspector).** Encoded-domain z-score + intra-ratio + min-bytes gate. Same hot-path code as today, never touches pixels. Publishes a raw event to `events.raw.<site>.<camera>`.
2. **Stage 2 (off-edge, post-processor).** Subscribes to raw events, fetches the clip + thumbnail strip from the edge, decodes a small frame strip with libav, runs SSIM-vs-baseline, optionally runs the configured vision model restricted to `target_class_mask`, and decides confirm/drop. Confirmed events are republished to `events.confirmed.<site>.<camera>`. Suppressed events go to `events.suppressed.<site>.<camera>` (with the labeller's reason) so the tuner has full negative-class data.

Crucially the gate is *optional and fail-open*. If the post-processor is down, raw events still get recorded and operators can still see them (subject-fan-out: an MQTT bridge can be subscribed to `events.raw.>` or `events.confirmed.>` depending on whether the operator wants noisy or filtered alerts). If the camera is flagged `slow_target`, Stage 2 degrades to SSIM-only — it will not require the vision model to confirm a ghillie-suited target it cannot see. The auto-tuner chooses Stage 2's parameters from data; it never deletes the gate.

## 5. Components

### 5.1 Edge agent additions (`cvkit/edge`)

| Endpoint                                  | Method | Purpose                                                                                                                          |
|-------------------------------------------|--------|----------------------------------------------------------------------------------------------------------------------------------|
| `/api/cameras/{name}/events`              | GET    | Replay event metadata for a time window. Used by the tuner when JetStream history is shorter than the desired tuning window.     |
| `/api/cameras/{name}/config` (extended)   | PUT    | Existing endpoint accepts the new tuner-writable fields from §4.                                                                 |
| `/api/clips/{id}/thumbs`                  | GET    | A 3-frame thumbnail strip (160 px JPEG) generated lazily on first request and cached next to the clip. Lets the post-processor cheap-vote on obvious FPs without pulling the full clip. |
| `/api/clips/{id}` (existing)              | GET    | The post-processor uses this to fetch the full clip for decode + vision pass.                                                    |

Plus one new module: `internal/nats`, a thin publisher that the existing event aggregator (`agent.Supervisor.Start`) fans events into. Configuration is a single block in `agent.toml`:

```toml
[nats]
url            = "nats://127.0.0.1:4222"   # local broker, or leaf-node to cloud
subject_prefix = "events"
site           = "site-warehouse-3"
publish_raw    = true                       # always; this is the inbound subject
```

The publisher is async and lossy under back-pressure — if the broker is unreachable the agent logs and drops, it never blocks the camera workers. JetStream stream definition lives in `autotune` (it is the *consumer* who declares its persistence requirements), so the edge does not need to know the retention policy.

The new tuner-writable fields in `emd_inspector_cfg_t` go through the existing `emd_cam_update_inspector_cfg` atomic-swap path so config updates remain lock-free on the hot path.

No new dependencies on the edge: NATS Go client is ~100 KB of code, libav stays out (no decode happens here), the vision libraries stay out.

### 5.2 NATS subject schema

```
events.raw.<site>.<camera>           ← published by edge
events.confirmed.<site>.<camera>     ← published by post-processor
events.suppressed.<site>.<camera>    ← published by post-processor (with reason)
clips.uploaded.<site>.<camera>       ← published by post-processor after S3 PUT
```

Raw event payload (JSON, ~400 B):

```json
{
  "event_id":   "01HXY3Q5...",
  "site":       "site-warehouse-3",
  "camera":     "axis_82_2",
  "ts":         "2026-05-18T03:01:14.221Z",
  "type":       "motion",
  "z_score":    5.52,
  "intra_ratio": 1.1,
  "bytes":      906,
  "bpf_slow":   354.0,
  "reason":     "z=5.52",
  "pts_start":  1234567890,
  "pts_end":    1234589012,
  "clip_id":    "01HXY3Q5...",
  "clip_url":   "https://edge-3.local:8443/api/clips/01HXY3Q5...",
  "thumbs_url": "https://edge-3.local:8443/api/clips/01HXY3Q5.../thumbs",
  "target_class_mask": 0x07,
  "agent_version":     "1.0.0"
}
```

Confirmed event = raw event + `{label: {classes: [...], max_conf: 0.91, model: "yolov8n@int8"}, s3_url: "s3://...", confirmed_by: "postproc-3"}`. Suppressed event = raw event + `{suppressed: true, suppress_reason: "ssim>0.98" | "no_target_class" | "bg_subtract_area<0.1%"}`.

JetStream stream `events.>` has retention sized for the tuner's longest training window (default 14 days). The post-processor runs as a JetStream pull consumer with `max_ack_pending` bounded so a stuck processor cannot wedge the stream.

### 5.3 Post-processor (`autotune/cmd/cvkit-postproc`)

One long-running Go binary, one goroutine per camera (configurable cap), the same shape as `emd-agent` so operators get a familiar deployment story. Pipeline per event:

1. Pull raw event off `events.raw.<site>.>`.
2. Fast-vote on the thumbnail strip first (cheap: an HTTP GET against the edge thumbnails endpoint + OpenCV BG-subtraction). If the change-area is < `bg_min_area_pct` of the frame, vote SUPPRESS, publish to `events.suppressed`, ack. This rejects the leaf/insect class of FP in single-digit milliseconds without paying for a decode.
3. If thumbnails are inconclusive: fetch the full clip from the edge, decode the first ~3 frames at event-start with libav, run SSIM against the most-recent confirmed-good baseline thumbnail for that camera (post-processor keeps a 1-per-camera rolling baseline). If SSIM > `ssim_skip_threshold`, vote SUPPRESS.
4. If still inconclusive: run the configured vision model restricted to `target_class_mask`. If any frame contains a qualifying detection with confidence ≥ `class_min_conf`, vote CONFIRM, else SUPPRESS — *unless* the camera is `slow_target`, in which case borderline-but-no-detection still confirms (we trust the encoded-domain detector when we know the visual confirm is unreliable).
5. If CONFIRM and `s3.enabled`: stream-upload the clip from disk (the post-processor fetched the bytes anyway) to S3 with a deterministic key. Publish to `events.confirmed` *only after* the S3 PUT is acknowledged, so downstream alerts always point to an uploaded artefact. Publish to `clips.uploaded` with the S3 URL.
6. Ack the JetStream message.

Per-event hard deadline: 5 seconds wall-clock by default, configurable per camera. If the deadline fires, the post-processor publishes the event as CONFIRMED with `confirmed_by: "deadline"` — fail-open: better to keep a possible TP than to drop one because the box is busy or the model is slow.

Configuration knobs (excerpt of `postproc.toml`):

```toml
[nats]
url             = "nats://nats.internal:4222"
subject_prefix  = "events"
durable_name    = "postproc-3"
max_ack_pending = 256

[gate]
enabled               = true
ssim_skip_threshold   = 0.985
bg_min_area_pct       = 0.10
class_min_conf        = 0.55
deadline_ms           = 5000

[model]
backend       = "onnxruntime"      # or "huggingface", "openrouter"
model_path    = "/models/yolov8n-int8.onnx"
batch_size    = 4
gpu           = "cuda:0"           # omit for CPU; "metal" on darwin

[s3]
enabled       = true
endpoint      = "https://s3.us-west-2.amazonaws.com"
bucket        = "cvkit-clips-prod"
key_template  = "{site}/{camera}/{date}/{event_id}.ts"
storage_class = "STANDARD_IA"
sse           = "AES256"

[edge.bridges]
# Map site → edge agent base URL. Used to fetch /api/clips/{id} and /thumbs.
"site-warehouse-3" = "https://edge-3.internal:8443"
```

This is the binary that benefits from cloud-side GPU. The same binary runs on edge with `model.gpu` unset; it runs in a Kubernetes Deployment with `model.gpu = "cuda:0"` and pulls events from many sites via NATS leaf links.

### 5.4 Vision models

Default validator: YOLOv8-n int8, ~6 MB, ~20 ms/frame on a single x86 core, ~3 ms on a small GPU. Covers `person`, `car`, `truck`, `bus`, `bicycle`, `motorcycle`, `dog`, `cat`, `bird`, `horse` — maps cleanly to the target classes the user asked for.

Swappable via `model.backend`:

- `onnxruntime` — local ONNX (YOLOv8/RF-DETR). Default for self-hosted.
- `huggingface` — a HuggingFace Inference Endpoint URL; batched, rate-limited. For sites without local compute or for the cloud post-processor when scaling out.
- `openrouter` — an OpenRouter multimodal LLM (Claude Haiku, Gemini Flash) called with the decoded frame strip as a 3-image grid. Very-low-volume fallback or "hard case" escalation. Per-site monthly budget enforced in the post-processor.

Because the post-processor is now cloud-eligible, we can practically use bigger models than would fit on an edge box: RF-DETR, Owl-ViT for open-vocabulary detection (useful for sites with unusual target classes like "deer" or "drone"), or even a vision-language model when the operator needs human-readable scene summaries.

### 5.5 Tuner (`autotune/cmd/cvkit-autotune`)

Operator-initiated CLI. Subcommands:

```
cvkit-autotune scan      --nats nats://.. --camera <name> --since 7d
cvkit-autotune label     --nats nats://.. --camera <name> --model yolov8n.onnx --classes person,vehicle
cvkit-autotune search    --camera <name> --objective f0.5
cvkit-autotune apply     --agent http://edge:8080 --camera <name> --plan plan.json --dry-run
cvkit-autotune apply     --agent http://edge:8080 --camera <name> --plan plan.json
cvkit-autotune rollback  --agent http://edge:8080 --camera <name> --run-id <id>
```

`scan` reads JetStream history (raw + confirmed + suppressed) for the camera. `label` is only needed when raw events have never been classified — normally the post-processor has already labelled them, so the tuner reuses those labels rather than re-running the model. `search` is the parameter sweep in §6. `apply` PUTs the chosen config back to the edge.

The user picked YOLO + heuristics auto-labelling. The "+ heuristics" piece:

- Time-of-day priors (a 03:00 trigger on an indoor office camera is structurally more suspicious — weight toward TP).
- "Train me" labels — when an operator marks a confirmed event as actually-not-interesting *or* marks a suppressed event as "you missed this", that gets stored back in NATS on `events.feedback.<site>.<camera>` and the tuner uses it to override the model label on the next run. This is the loop that catches the ghillie-suit case over time without us having to design a separate detector for it.

## 6. The tuning loop

Per camera, given N labelled events from the last window (recommend N ≥ 200, falling back to cluster-level search below 100):

1. **Build the confusion table** from JetStream history. For each event we have the z-score, intra-ratio, byte counts at the time it fired, *and* the post-processor's label. Sweep candidate cutoffs over the joint space `(motion_z_high, intra_ratio_high, min_bytes_threshold, on_threshold, ssim_skip_threshold)` to compute TP/FP/FN at each combination.
2. **Coarse grid first**, on the closed-form thresholds — z in `{2.5, 3.0, 3.5, 4.0, 4.5, 5.0}`, min_bytes in `{0, 500, 1000, 2000, 4000, 8000}`, etc. ~600 cells, evaluated in seconds because there is no re-decode.
3. **Bayesian refinement** around the top 10 cells (Gaussian Process with EI acquisition), to fine-tune `ssim_skip_threshold` and the warmup parameters, which are continuous.
4. **Objective function** defaults to F0.5 (precision-weighted — false positives hurt more than false negatives in this domain), with a hard constraint that recall on the labelled TP set drops by no more than `--max-recall-loss` (default 2%). The constraint protects the ghillie-suit class: if a camera is flagged `slow_target`, we add synthetic "slow target" events (a few frames of low-z, low-intra ground truth) into the TP set and the constraint forces the search to keep them.
5. **Hierarchical fallback**. For cameras with too few events to fit reliably, fall back to a cluster-level config: cluster cameras by their statistical fingerprint (median bpf, bpf variance, fps, FOV class) and use the cluster's median tuning. The user asked specifically for "a camera or group of cameras"; the cluster path is how the "group" case works.
6. **Output plan**. JSON: current config, proposed config, expected FP reduction, expected TP loss, sample of 10 events that flip TP→FN and 10 that flip FP→TN. The operator skims the flips, then `apply`.

### 6.1 Safety in production

`apply` does a canary by default: PUTs the new config to the edge, watches event rates on `events.confirmed.<site>.<camera>` for 15 minutes via NATS, and reverts if the rate spikes ≥ 3× or drops to zero (something is wrong). The agent supports runtime config updates without restart (§3.5 of `agent_abi.h`), so this is cheap.

Configs the tuner writes are stamped with `tuner_run_id` and stored in JetStream on `config.history.<site>.<camera>` (keyed by camera, last 20 retained) so any operator can `cvkit-autotune rollback --run-id <id>` to any previous tuned config. The edge stays stateless about config history; JetStream is the source of truth.

## 7. Mapping to the user's three asks

1. **SSIM-style change metric that knows when not to fire.** Stage-2 in the post-processor does the SSIM check on candidate events only, with the `slow_target` flag preserving sensitivity for ghillie-suit-class targets. SSIM threshold is tuned per camera, not global. The OpenCV BG-subtraction fast-path in §5.3 rejects the cheapest FPs (leaves, insects) before SSIM even runs.
2. **Target-class tuning.** `target_class_mask` is per camera, and the sidecar's objective is computed against labels for that target class only. A camera tuned for "vehicle" will accept a relatively high `min_bytes_threshold` (vehicles produce big P-frame deltas) where a "person" camera will not.
3. **YOLO / HuggingFace / OpenRouter / OpenCV.** §5.3 + §5.4 — local YOLOv8-n via ONNX is the default, HF Endpoints and OpenRouter are config flags for low-volume or escalation sites, and OpenCV BG-subtraction is the fast-path FP rejector that lives in front of YOLO to cut compute cost. Because the post-processor now lives outside the edge process, swapping in GPU-only models (RF-DETR, Owl-ViT) at cloud-scale is just a config change, not a separate code path.

## 8. Decoupling benefits

Putting the post-processor behind NATS gives us four things the inline-gate design did not:

- **Blocking work is fine.** S3 upload of clips, HF Inference Endpoint calls, OpenRouter LLM calls all have multi-second tails. The post-processor can take them; the edge never waits.
- **Elastic compute.** One cloud-side post-processor pool can serve many edge sites. Sites with cheap-class targets need almost no inference. Sites with hard targets (open-vocabulary, low-light) can route to bigger models without re-deploying the edge.
- **Replay.** JetStream retains raw events for the tuner's window. Re-tuning, A/B-evaluating a new model, or backfilling labels after a model upgrade all become "consume from a JetStream subject", not "go pull from the edge".
- **Operational independence.** A bad model deploy in the post-processor cannot affect ingest. Roll the post-processor at will; the edge keeps recording.

## 9. Phasing

**Phase A — cheap wins, no new component (1–2 weeks).** Extend `emd_inspector_cfg_t` with `min_bytes_threshold`, `bpf_relative_floor`, and the warmup pair. Add `/api/cameras/{name}/events` and a JSONL event ring on the edge. Stand up `cvkit-autotune` skeleton in `cvkitio/autotune` that runs encoded-domain-only search against the JSONL ring — no NATS, no decode, no model. This alone should knock out the `axis_82_2` class of FP described in the archive doc.

**Phase B — NATS + post-processor skeleton (2 weeks).** Add `internal/nats` to the edge agent. Stand up `cvkit-postproc` that subscribes to raw events and republishes them to confirmed unchanged (a passthrough). Stand up JetStream with the retention policy. Operate this way for a week to validate the pipe shape before any gating logic goes live.

**Phase C — gate logic and S3 (2–3 weeks).** Add OpenCV BG-subtraction fast path, SSIM stage, libav decoder seam in `pkg/vision`. Add S3 upload. Run in shadow mode (post-processor logs the suppress decision but still publishes as confirmed) for one full week before letting suppression actually drop events.

**Phase D — vision model and full tuner (2–3 weeks).** Add YOLOv8-n via onnxruntime-go. Plumb the labelled events from JetStream history into the tuner's confusion table; ship grid + Bayesian search and canary apply. Bring in HF/OpenRouter behind a feature flag. Cloud GPU deployment recipe for the post-processor.

**Phase E — cluster tuning and operator feedback loop (later).** Hierarchical fallback for low-volume cameras, integrating `events.feedback.>` into the label set, per-cluster default configs in the install bundle.

## 10. Open questions

- Does the existing `emd_decoder_libav` plugin already exist as code, or only as a planned target in the spec? `edge/CLAUDE.md` references it. The post-processor needs a libav-backed decoder; either we adopt the same plugin via a small CGo seam, or the post-processor links libav directly. The latter is simpler given the post-processor binary is not constrained the way the edge is.
- NATS broker placement: single cloud broker with leaf-node clients per site, single on-prem broker with cloud bridge, or dual? Recommendation is leaf-node-per-site with a cloud cluster, but this depends on the customer's network posture (some on-prem sites have no egress at all — in those cases the post-processor must run on-prem and the tuner runs against the local JetStream).
- Storage budget for JetStream and clip thumbnails — needs a decision per disk-quota policy in `[disk]`. Rough sizing: events at ~500 B × 1000 events/day × 14 days × 64 cameras × replication 3 ≈ 1.3 GB JetStream; thumbnails at ~30 KB × 3 per clip × 50 clips/day × 30 days × 64 cams ≈ 9 GB on the edge node.
- Whether `target_class_mask` is a per-camera scalar or a small map of `{class: weight}`. The map form is more expressive but adds plumbing through the C ABI; the scalar form is what's drawn above.
- Cost ceiling for OpenRouter / HF Inference Endpoint per site per month, and how the post-processor enforces it (token bucket, hard cap, fallback to local).

## 11. Risks

- **NATS broker outage.** Mitigated by JetStream durability and by the edge's fail-open publisher (events are kept in the recorder's normal output path regardless of broker health; only the *confirmation* gate is skipped). An MQTT bridge subscribed to `events.raw.>` can deliver alerts during an outage if the operator wants pre-confirmation noise.
- **Post-processor suppresses a real event during a busy spike.** Mitigated by the 5 s fail-open deadline and by the canary on `apply`.
- **Auto-labels are wrong on a niche class.** The `slow_target` flag plus the operator feedback subject is the long-term fix; in the short term, `--max-recall-loss` defaults to a conservative 2%.
- **LGPL/GPL contamination of the edge binary.** Unchanged from r1 — libav lives entirely outside the edge process now, in the post-processor. The edge is even cleaner than before.
- **GPU isn't available at all sites.** Same answer as r1, plus: cloud post-processor pool is now an option, so on-prem-with-no-GPU sites can route inference to a cloud GPU pool over NATS leaf links if they have any egress at all.

## 12. Repository layout

`cvkit/edge` (this repo) — unchanged shape, gains `internal/nats`, three new REST endpoints, and the new `emd_inspector_cfg_t` fields.

`cvkitio/autotune` (new repo at `/Users/andrewsinclair/workspace/cvkitio/autotune`):

```
autotune/
├── README.md
├── go.mod
├── cmd/
│   ├── cvkit-postproc/        # long-running NATS subscriber
│   │   └── main.go
│   └── cvkit-autotune/        # operator CLI
│       └── main.go
├── pkg/
│   ├── vision/                # SSIM, BG-sub, ONNX, HF, OpenRouter
│   ├── nats/                  # JetStream wiring, schemas
│   ├── s3/                    # uploader
│   ├── edge/                  # client for edge REST (/api/clips, /thumbs, /config)
│   └── tuner/                 # grid + Bayesian search, confusion table
├── deploy/
│   ├── docker/                # Dockerfiles for postproc and autotune
│   ├── k8s/                   # Deployment, Service, JetStream config
│   └── systemd/               # postproc.service for on-prem
├── configs/
│   ├── postproc.toml.example
│   └── tuner.yaml.example
└── docs/
    └── design.md              # mirrors this file; canonical copy stays in cvkit/edge/docs/
```

The canonical design lives in `cvkit/edge/docs/auto-tune-design.md` (this file) because the edge contract is the one that has to stay stable; the `autotune/docs/design.md` is a checked-in copy/symlink for repo-local readers.
