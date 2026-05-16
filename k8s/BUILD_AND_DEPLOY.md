# Build and Deploy to au01-0 K8s Cluster

## Prerequisites

- Docker (for building x86 image)
- kubectl configured for au01-0 cluster
- Access to cameras on VLAN 81/82 from cluster
- Camera credentials (username/password)

## Step 1: Discover Camera Feeds

```bash
cd /Users/andrewsinclair/workspace/cvkitio/cvkit/edge

# Discover all cameras and their available feeds
./scripts/discover_axis_cameras.sh k8s/camera_list.txt root 'YOUR_PASSWORD' > k8s/discovered_cameras.json

# Review discovered feeds
cat k8s/discovered_cameras.json | jq '.'
```

Expected output:
```json
{
  "cameras": [
    {
      "ip": "192.168.1.1",
      "path": "/axis-media/media.amp",
      "url": "rtsp://root:PASSWORD@192.168.1.1/axis-media/media.amp",
      "codec": "h264",
      "width": 1920,
      "height": 1080,
      "fps": 15
    },
    ...
  ]
}
```

## Step 2: Generate Config File

```bash
# Generate agent.toml from discovered cameras
./scripts/generate_config.py k8s/discovered_cameras.json 'YOUR_PASSWORD' > k8s/agent.toml

# Review generated config
head -50 k8s/agent.toml
```

## Step 3: Build Docker Image

```bash
# Build x86_64 image
docker build -t your-registry/emd-agent:phase2-mvp --platform linux/amd64 .

# Test locally (optional)
docker run --rm \
  -v $(pwd)/k8s/agent.toml:/etc/emd-agent/agent.toml:ro \
  your-registry/emd-agent:phase2-mvp

# Push to registry
docker push your-registry/emd-agent:phase2-mvp
```

## Step 4: Deploy to K8s

```bash
# Create namespace
kubectl create namespace emd

# Create config map from generated config
kubectl create configmap emd-agent-config \
  --from-file=agent.toml=k8s/agent.toml \
  -n emd

# Deploy
kubectl apply -f k8s/deployment.yaml

# Watch deployment
kubectl get pods -n emd -w
```

## Step 5: Verify Deployment

```bash
# Check pod status
kubectl get pods -n emd

# View logs
kubectl logs -f deployment/emd-agent -n emd

# Check metrics
kubectl port-forward -n emd deployment/emd-agent 9464:9464 &
curl http://localhost:9464/metrics | grep emd_

# Check events (look for EVENT: lines)
kubectl logs -n emd deployment/emd-agent | grep EVENT:
```

## Step 6: Monitor Resources

```bash
# Real-time resource usage
kubectl top pods -n emd

# Detailed metrics
kubectl get --raw /apis/metrics.k8s.io/v1beta1/namespaces/emd/pods | jq .
```

## Troubleshooting

### Camera Connection Issues

```bash
# Exec into pod
kubectl exec -it -n emd deployment/emd-agent -- /bin/sh

# Test camera connectivity from pod (requires curl/ffprobe in image)
# Note: Current image is minimal, may need debug container
kubectl debug -it -n emd deployment/emd-agent --image=busybox
```

### High Resource Usage

```bash
# Check per-camera stats
kubectl logs -n emd deployment/emd-agent | grep STATS:

# Adjust resource limits in deployment.yaml and re-apply
kubectl apply -f k8s/deployment.yaml
```

### Config Updates

```bash
# Update config
./scripts/generate_config.py k8s/discovered_cameras.json 'YOUR_PASSWORD' > k8s/agent.toml

# Replace configmap
kubectl create configmap emd-agent-config \
  --from-file=agent.toml=k8s/agent.toml \
  -n emd \
  --dry-run=client -o yaml | kubectl replace -f -

# Restart pod to pick up new config
kubectl rollout restart deployment/emd-agent -n emd
```

## Scaling Considerations

**Current Setup (Single Pod):**
- All 14-16 cameras in one agent process
- Resource allocation: 4-8 CPU, 4-8 GB RAM
- Single point of failure

**Recommended for Production:**
- Use StatefulSet with camera sharding
- 2-4 pods, each handling 4-8 cameras
- Higher availability, isolated failures

To implement sharding:
1. Split `camera_list.txt` into shards
2. Generate separate configs per shard
3. Deploy multiple StatefulSets with different configs

See Phase 2 spec §2.4 Mode B for architecture details.
