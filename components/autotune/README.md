# cvkit-autotune

Post-processing and auto-tuning for the `cvkit/edge` motion detector.

This component is intentionally separate from the edge agent so that pixel-decode, vision-model inference, S3 upload, and any other potentially blocking work happen *off* the camera ingest path. It is also where larger / GPU-accelerated models live when the deployment puts them in the cloud.

## Two binaries, one component

- **`cvkit-postproc`** — long-running NATS subscriber. Consumes raw motion events published by edge agents on `events.raw.<site>.<camera>`, runs the two-stage gate (OpenCV background-subtraction → SSIM → vision model), uploads confirmed clips to S3, and republishes confirmed/suppressed events on `events.confirmed.>` / `events.suppressed.>`. Deployable on the edge node, on-prem next to the edge, or in a cloud GPU pod — same binary, different config.

- **`cvkit-autotune`** — operator-initiated CLI. Pulls labelled-event history from JetStream, runs a parameter sweep (coarse grid + Bayesian refinement) against an F0.5 objective, and applies the resulting per-camera config to the edge via its existing `/api/cameras/{name}/config` REST endpoint with a 15-minute canary.

## Canonical design

The full design — architecture, NATS subject schema, gate semantics, tuning loop, phasing, risks — lives in the `cvkit/edge` repository so the edge ABI / contract stays in one place:

[`cvkit/edge/docs/auto-tune-design.md`](../cvkit/edge/docs/auto-tune-design.md)

A mirrored copy will be kept here at `docs/design.md` for repo-local readers.

## Why a separate component

- **Decouples blocking work from ingest.** S3 uploads, HuggingFace / OpenRouter API calls, and large-model inference all have multi-second tails. Putting them behind NATS means a slow upload or a stalled model never back-pressures the edge.
- **Cloud-friendly.** The post-processor can run on a GPU node and serve many edge sites via NATS leaf-node links, letting us run models that would never fit on the edge box.
- **Independent rollout.** A bad model deploy in the post-processor cannot affect the edge. The edge keeps recording; only confirmation is affected.
- **License hygiene.** Vision libraries (libav for decode, ONNX Runtime, OpenCV) stay entirely outside the edge binary. The edge stays clean of LGPL/GPL contamination per its license policy.

## Status

Skeleton only — see the design doc for phasing. Phase B (NATS + post-processor passthrough) is the first milestone that puts this binary into a real deployment.
