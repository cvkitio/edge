#!/bin/bash
# Complete deployment script for EMD Agent to k3s

set -e

echo "========================================="
echo "EMD Agent Deployment to K3s"
echo "========================================="
echo ""

# Configuration
K3S_NODE="andrew-ubuntu-2404"
K3S_USER="andrew"
IMAGE_NAME="emd-agent:latest"
NAMESPACE="emd"

# Step 1: Build image
echo "[1/7] Building Docker image..."
docker build -t "$IMAGE_NAME" . || {
    echo "ERROR: Docker build failed"
    exit 1
}

# Step 2: Save image
echo "[2/7] Saving image to tarball..."
docker save "$IMAGE_NAME" -o /tmp/emd-agent.tar
echo "  Saved: /tmp/emd-agent.tar ($(du -h /tmp/emd-agent.tar | cut -f1))"

# Step 3: Copy to k3s node
echo "[3/7] Copying image to k3s node..."
scp /tmp/emd-agent.tar "$K3S_USER@$K3S_NODE:/tmp/" || {
    echo "ERROR: Failed to copy image to k3s node"
    echo "Please ensure SSH access to $K3S_USER@$K3S_NODE is configured"
    exit 1
}

# Step 4: Import into k3s
echo "[4/7] Importing image into k3s containerd..."
ssh "$K3S_USER@$K3S_NODE" 'sudo k3s ctr images import /tmp/emd-agent.tar' || {
    echo "ERROR: Failed to import image"
    exit 1
}

echo "  Verifying image..."
ssh "$K3S_USER@$K3S_NODE" 'sudo k3s ctr images ls | grep emd-agent'

# Step 5: Create namespace and configmap
echo "[5/7] Creating Kubernetes resources..."
kubectl create namespace "$NAMESPACE" --dry-run=client -o yaml | kubectl apply -f -
kubectl create configmap emd-agent-config \
    --from-file=k8s/agent.toml \
    -n "$NAMESPACE" \
    --dry-run=client -o yaml | kubectl apply -f -

# Step 6: Deploy
echo "[6/7] Deploying to Kubernetes..."
kubectl apply -f k8s/deployment.yaml 2>&1 | grep -v "ServiceMonitor"

# Step 7: Wait for pod
echo "[7/7] Waiting for pod to be ready..."
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
echo "  kubectl port-forward -n $NAMESPACE svc/emd-agent-api 8080:8080"
echo ""
echo "Then open:"
echo "  Web UI:  http://localhost:8080/"
echo "  Swagger: http://localhost:8080/docs"
echo "  Metrics: http://localhost:9464/metrics"
echo ""
