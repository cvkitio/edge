# cvkit platform

Monorepo for the cvkit edge camera processing platform.

## Components

| Component | Path | Purpose |
|-----------|------|---------|
| **emd** | `components/emd/` | Edge motion detector — RTSP camera processing, z-score detection, clip recording |
| **autotune** | `components/autotune/` | Parameter search & tuning — optimises emd detector thresholds |
| **postprocess** | `components/postprocess/` | Event pipeline — picks up motion events and processes them (S3 upload, webhooks) |

## Shared Infrastructure

All components communicate via **NATS JetStream**. See `deploy/` for Kubernetes manifests and Docker Compose for local development.

### NATS Subject Schema

| Subject | Publisher | Consumer |
|---------|-----------|----------|
| `events.raw.<site>.<camera>` | emd | autotune, postprocess |
| `events.confirmed.<site>.<camera>` | postprocess | — |
| `events.suppressed.<site>.<camera>` | postprocess | — |
| `clips.uploaded.<site>.<camera>` | postprocess | — |
| `events.feedback.<site>.<camera>` | operator | autotune |
| `config.history.<site>.<camera>` | autotune | — |

## Quick Start (local)

```bash
# Build everything
make build

# Start local dev stack (Docker Compose)
make deploy-local
```

## Deploy to Kubernetes

```bash
# Apply all resources to the cvkit namespace
kubectl apply -k deploy/k8s/
```

## Documentation

See `docs/` for architecture, API reference, and deployment guides.
