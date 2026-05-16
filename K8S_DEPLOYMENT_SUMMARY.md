# K8s Deployment Summary - 16 Camera Array

## Overview

Complete deployment solution for monitoring 16 Axis cameras (VLAN 81: 12 cameras, VLAN 82: 2 cameras) on au01-0 K8s cluster.

## Components Provided

### 1. Camera Discovery & Config Generation

**`scripts/discover_axis_cameras.sh`**
- Scans each camera IP
- Probes all common RTSP paths
- Detects multiple feeds per camera
- Outputs JSON with codec, resolution, FPS for each feed

**`scripts/generate_config.py`**
- Reads discovered camera JSON
- Generates complete `agent.toml` config
- Auto-calculates bitrate, off_threshold based on camera specs
- Includes NATS, MQTT, S3 integration

**Example Usage:**
```bash
# Discover cameras
./scripts/discover_axis_cameras.sh k8s/camera_list.txt root '***REDACTED_PASSWORD***' > k8s/discovered_cameras.json

# Generate config
./scripts/generate_config.py k8s/discovered_cameras.json '***REDACTED_PASSWORD***' > k8s/agent.toml

# Result: Full TOML config for all discovered camera feeds
```

### 2. Container Build

**`Dockerfile`**
- Multi-stage build (libemd → Go agent → runtime)
- Target platform: linux/amd64 (x86_64)
- Base: debian:bookworm-slim
- Size: ~50 MB (optimized)
- Security: runs as non-root user (uid 1000)

**Build Command:**
```bash
docker build -t your-registry/emd-agent:phase2-mvp --platform linux/amd64 .
docker push your-registry/emd-agent:phase2-mvp
```

### 3. K8s Deployment

**`k8s/deployment.yaml`**
- Namespace: `emd`
- ConfigMap: agent.toml injection
- PVC: 100 GB for local storage
- Deployment: single replica (16 cameras)
- Service: metrics endpoint (port 9464)
- ServiceMonitor: Prometheus integration

**Resource Allocation:**
```yaml
resources:
  requests:
    cpu: 4 cores      # 250m per camera baseline
    memory: 4 GB      # 250 MB per camera
  limits:
    cpu: 8 cores      # Burst capacity
    memory: 8 GB      # 2x safety margin
```

### 4. Camera List

**`k8s/camera_list.txt`**
```
10.45.81.1  - 10.45.81.12  (VLAN 81)
10.45.82.1  - 10.45.82.2   (VLAN 82)
```

Total: 14 cameras (may discover 20-30 feeds if cameras have multiple streams)

## Deployment Steps

### Quick Start

```bash
# 1. Discover cameras
cd /Users/andrewsinclair/workspace/cvkitio/cvkit/edge
./scripts/discover_axis_cameras.sh k8s/camera_list.txt root '***REDACTED_PASSWORD***' > k8s/discovered_cameras.json

# 2. Generate config
./scripts/generate_config.py k8s/discovered_cameras.json '***REDACTED_PASSWORD***' > k8s/agent.toml

# 3. Build and push image
docker build -t your-registry/emd-agent:phase2-mvp --platform linux/amd64 .
docker push your-registry/emd-agent:phase2-mvp

# 4. Deploy to K8s
kubectl create namespace emd
kubectl create configmap emd-agent-config --from-file=agent.toml=k8s/agent.toml -n emd
kubectl apply -f k8s/deployment.yaml

# 5. Monitor
kubectl logs -f deployment/emd-agent -n emd
kubectl top pods -n emd
```

See `k8s/BUILD_AND_DEPLOY.md` for detailed instructions.

## Resource Usage Summary

### Per-Camera Profile

| Resource | Per Camera | 16 Cameras Total |
|----------|------------|------------------|
| CPU (baseline) | 100m | 1.6 cores |
| CPU (peak) | 150m | 2.4 cores |
| Memory | 40 MB | 640 MB |
| Network | 4-8 Mbps | 64-128 Mbps |
| Storage (temp) | 2 GB | 32 GB |

### Pod Allocation (with safety margins)

| Resource | Request | Limit | Expected | Utilization |
|----------|---------|-------|----------|-------------|
| CPU | 4 cores | 8 cores | 1.5-3.5 cores | 37-87% |
| Memory | 4 GB | 8 GB | 1.2-2.5 GB | 30-62% |
| Storage | 100 GB | - | 30-60 GB | 30-60% |

### Cost Estimate

**AWS EKS (us-west-2, spot instances):**
- Compute (c5.xlarge): $22/month
- Storage (100GB EBS): $8/month
- S3 (1TB/month): $23/month
- **Total: ~$53/month**

(On-demand: ~$92/month)

See `k8s/RESOURCE_ANALYSIS.md` for detailed breakdown.

## Architecture

```
┌─────────────────────────────────────────────────┐
│            K8s Node (au01-0 cluster)            │
│                                                 │
│  ┌───────────────────────────────────────────┐ │
│  │   Pod: emd-agent                          │ │
│  │                                           │ │
│  │   ┌────────────────┐                      │ │
│  │   │  Go Supervisor │                      │ │
│  │   │  (main.go)     │                      │ │
│  │   └────┬───────────┘                      │ │
│  │        │                                   │ │
│  │   ┌────▼───────────────┐                  │ │
│  │   │ 16 Camera Workers  │                  │ │
│  │   │ (LockOSThread)     │                  │ │
│  │   └────┬───────────────┘                  │ │
│  │        │ cgo                               │ │
│  │   ┌────▼───────────────┐                  │ │
│  │   │     libemd.so      │                  │ │
│  │   │ (C hot path)       │                  │ │
│  │   │                    │                  │ │
│  │   │ - RTSP/RTP         │                  │ │
│  │   │ - H.264 depay      │                  │ │
│  │   │ - Inspector        │                  │ │
│  │   │ - Ring buffer      │                  │ │
│  │   └─────────┬──────────┘                  │ │
│  │             │ events                       │ │
│  │   ┌─────────▼──────────┐                  │ │
│  │   │  Event Bus (chan)  │                  │ │
│  │   └──┬──────┬──────┬───┘                  │ │
│  │      │      │      │                       │ │
│  │   ┌──▼──┐ ┌▼───┐ ┌▼──────┐               │ │
│  │   │NATS │ │MQTT│ │Outbox │               │ │
│  │   └─────┘ └────┘ └───────┘               │ │
│  └───────────────────────────────────────────┘ │
│                                                 │
│  ┌────────────────────┐                        │
│  │  PVC (100 GB)      │                        │
│  │  /var/lib/emd-agent│                        │
│  └────────────────────┘                        │
└─────────────────────────────────────────────────┘
         │              │              │
         ▼              ▼              ▼
   [16 Cameras]   [NATS/MQTT]      [S3/MinIO]
   10.45.81.x     (cluster)        (cluster)
   10.45.82.x
```

## Monitoring

### Key Metrics

```bash
# View real-time logs with events
kubectl logs -f deployment/emd-agent -n emd | grep -E "(EVENT:|STATS:)"

# Check resource usage
kubectl top pods -n emd

# Metrics endpoint
kubectl port-forward -n emd deployment/emd-agent 9464:9464
curl http://localhost:9464/metrics | grep emd_agent
```

### Expected Output

```
2026/05/16 10:XX:XX agent.go:74: EVENT: cam=axis_10_45_81_1_0 type=motion reason=z=4.23 pts=...
2026/05/16 10:XX:XX agent.go:74: EVENT: cam=axis_10_45_81_7_0 type=motion reason=z=5.11 pts=...
2026/05/16 10:XX:XX agent.go:78: STATS: cam=0 bpf_ewma=7826.9 fsm=0 rtsp=0
2026/05/16 10:XX:XX agent.go:78: STATS: cam=7 bpf_ewma=12340.1 fsm=1 rtsp=0
...
```

### Prometheus Queries

```promql
# Event rate per camera
rate(emd_agent_events_total{type="motion"}[5m])

# CPU per camera (approximation)
rate(process_cpu_seconds_total[5m]) / 16

# Active cameras (RTSP playing state)
count(emd_agent_camera_rtsp_state{state="playing"} == 1)

# Motion detection rate
rate(emd_agent_events_total{type="motion"}[1h])
```

## Production Checklist

- [ ] Network connectivity: cameras reachable from cluster
- [ ] Credentials: stored in K8s Secret (not ConfigMap)
- [ ] NATS/MQTT: endpoints configured in cluster
- [ ] S3/MinIO: bucket created, credentials configured
- [ ] Storage class: 100GB PVC provisioner available
- [ ] Node selector: label nodes for camera workload
- [ ] Prometheus: ServiceMonitor CRD available
- [ ] Alerts: configured for disconnection, high CPU, dropped events
- [ ] Backup: PVC backup policy for outbox DB
- [ ] Logging: centralized log aggregation (ELK/Loki)

## Scaling Options

### Current: Single Pod (16 cameras)
✅ Simplest deployment  
✅ Low operational overhead  
❌ Single point of failure  
❌ Limited to ~20-30 cameras

### Option 1: Sharded Deployment (4 pods × 4 cameras)
- Split camera_list.txt into 4 files
- Generate 4 separate configs
- Deploy 4 StatefulSets
- Resource: 4 × (1.5 cores, 1.5 GB) = 6 cores, 6 GB total
- ✅ Better failure isolation
- ✅ Higher availability
- ❌ More operational complexity

### Option 2: Multi-Node Spread
- Use pod anti-affinity rules
- Spread replicas across nodes
- Requires StatefulSet with camera sharding
- ✅ Node failure tolerance

See Phase 2 spec §2.4 for sharded mode architecture.

## Troubleshooting

### Camera Not Connecting

```bash
# Check RTSP from pod
kubectl exec -it -n emd deployment/emd-agent -- /bin/sh
# (requires debug tools in image)

# Check logs for RTSP errors
kubectl logs -n emd deployment/emd-agent | grep -i "error.*rtsp"

# Common issues:
# - Network policy blocking RTSP port 554
# - Camera credentials incorrect
# - Camera URL path wrong (use discover script)
# - Camera firewall blocking cluster IPs
```

### High CPU Usage

```bash
# Identify which cameras are active
kubectl logs -n emd deployment/emd-agent | grep "STATS:.*fsm=1"

# If all cameras are active simultaneously:
# - Increase CPU limits
# - Split into sharded deployment
# - Check for network retransmissions (quality issues)
```

### Events Not Publishing

```bash
# Check NATS/MQTT connectivity
kubectl logs -n emd deployment/emd-agent | grep -E "(nats|mqtt)"

# Verify services are reachable
kubectl run -it --rm debug --image=busybox --restart=Never -- sh
# Inside pod: nc -zv nats.default.svc.cluster.local 4222
```

## Next Steps

1. **Immediate:** Run camera discovery to determine actual feed count
2. **Deploy:** Build image and deploy to au01-0
3. **Monitor:** Observe for 24-48h, tune resources
4. **Optimize:** Implement sharding if >20 total feeds discovered
5. **Production:** Add NATS/MQTT/S3 backends, enable outbox, add alerts

## Files Reference

```
edge/
├── Dockerfile                       # Container build
├── k8s/
│   ├── camera_list.txt             # 16 camera IPs
│   ├── deployment.yaml             # K8s manifests
│   ├── BUILD_AND_DEPLOY.md         # Deployment guide
│   └── RESOURCE_ANALYSIS.md        # Detailed resource breakdown
├── scripts/
│   ├── discover_axis_cameras.sh    # Camera discovery
│   └── generate_config.py          # Config generation
└── VIDEO_FILE_TEST_SUMMARY.md      # Phase 2 validation
```
