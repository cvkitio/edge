#!/bin/bash
# Deploy EMD Agent to k3s using kubectl (no SSH required)

set -e

echo "========================================="
echo "EMD Agent Deployment to K3s (kubectl)"
echo "========================================="
echo ""

IMAGE_NAME="emd-agent:latest"
NAMESPACE="emd"

# Step 1: Build image for amd64
echo "[1/6] Building Docker image for amd64..."
docker build --platform linux/amd64 -t "$IMAGE_NAME" . || {
    echo "ERROR: Docker build failed"
    exit 1
}

# Step 2: Save image
echo "[2/6] Saving image to tarball..."
docker save "$IMAGE_NAME" -o /tmp/emd-agent.tar
echo "  Saved: /tmp/emd-agent.tar ($(du -h /tmp/emd-agent.tar | cut -f1))"

# Step 3: Load via privileged pod
echo "[3/6] Loading image into k3s containerd via privileged pod..."

# Create temporary privileged pod
cat <<EOF | kubectl apply -f -
apiVersion: v1
kind: Pod
metadata:
  name: image-loader
  namespace: default
spec:
  hostPID: true
  hostNetwork: true
  containers:
  - name: loader
    image: alpine:latest
    command: ["sleep", "300"]
    securityContext:
      privileged: true
    volumeMounts:
    - name: host-root
      mountPath: /host
  volumes:
  - name: host-root
    hostPath:
      path: /
  restartPolicy: Never
EOF

# Wait for pod to be ready
echo "  Waiting for loader pod..."
kubectl wait --for=condition=ready pod/image-loader --timeout=60s

# Copy tarball to pod
echo "  Copying tarball to pod..."
kubectl cp /tmp/emd-agent.tar image-loader:/tmp/emd-agent.tar

# Import into k3s containerd (chroot to use host's k3s ctr)
echo "  Importing into k3s containerd..."
kubectl exec image-loader -- chroot /host k3s ctr images import /tmp/emd-agent.tar

# Verify
echo "  Verifying image..."
kubectl exec image-loader -- chroot /host k3s ctr images ls | grep emd-agent

# Cleanup loader pod
echo "  Cleaning up loader pod..."
kubectl delete pod image-loader

# Step 4: Create namespace and configmap
echo "[4/6] Creating Kubernetes resources..."
kubectl create namespace "$NAMESPACE" --dry-run=client -o yaml | kubectl apply -f -
kubectl create configmap emd-agent-config \
    --from-file=agent.toml=k8s/agent.toml \
    -n "$NAMESPACE" \
    --dry-run=client -o yaml | kubectl apply -f -

# Step 5: Deploy
echo "[5/6] Deploying to Kubernetes..."
kubectl apply -f k8s/deployment.yaml

# Step 6: Wait for pod
echo "[6/6] Waiting for pod to be ready..."
kubectl delete pod -l app=emd-agent -n "$NAMESPACE" --ignore-not-found=true --wait=false
sleep 5
kubectl wait --for=condition=ready pod -l app=emd-agent -n "$NAMESPACE" --timeout=120s || {
    echo "WARNING: Pod not ready yet, checking status..."
    kubectl get pods -n "$NAMESPACE"
    kubectl describe pod -n "$NAMESPACE" -l app=emd-agent | tail -20
}

echo ""
echo "========================================="
echo "Deployment Complete!"
echo "========================================="
echo ""

# Show status
echo "Pod Status:"
kubectl get pods -n "$NAMESPACE" -l app=emd-agent

echo ""
echo "Services:"
kubectl get svc -n "$NAMESPACE"

echo ""
echo "To access the Web UI and Swagger:"
echo "  ./k8s/port-forward.sh"
echo ""
echo "Then open:"
echo "  Web UI:  http://localhost:8080/"
echo "  Swagger: http://localhost:8080/docs"
echo "  Metrics: http://localhost:9464/metrics"
echo ""
