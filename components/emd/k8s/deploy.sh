#!/bin/bash
# Deployment script for emd-agent to au01-0 K8s cluster
set -euo pipefail

NAMESPACE="emd"
IMAGE="${IMAGE:-emd-agent:phase2-mvp}"

echo "==> Deploying emd-agent to au01-0 cluster"
echo "    Image: $IMAGE"
echo "    Namespace: $NAMESPACE"
echo

# Create namespace (idempotent)
echo "==> Creating namespace $NAMESPACE"
kubectl create namespace "$NAMESPACE" --dry-run=client -o yaml | kubectl apply -f -

# Create ConfigMap from agent.toml
echo "==> Creating ConfigMap from agent.toml"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
kubectl create configmap emd-agent-config \
    --from-file=agent.toml="$SCRIPT_DIR/agent.toml" \
    --namespace="$NAMESPACE" \
    --dry-run=client -o yaml | kubectl apply -f -

# Update deployment image
echo "==> Updating deployment.yaml with image: $IMAGE"
sed "s|your-registry/emd-agent:phase2-mvp|$IMAGE|g" "$SCRIPT_DIR/deployment.yaml" > /tmp/emd-deployment.yaml

# Apply deployment
echo "==> Applying deployment"
kubectl apply -f /tmp/emd-deployment.yaml

# Wait for rollout
echo "==> Waiting for rollout to complete"
kubectl rollout status deployment/emd-agent -n "$NAMESPACE" --timeout=5m

# Show status
echo
echo "==> Deployment complete!"
echo
kubectl get pods -n "$NAMESPACE" -l app=emd-agent
echo
echo "==> To view logs:"
echo "    kubectl logs -f deployment/emd-agent -n $NAMESPACE"
echo
echo "==> To view events:"
echo "    kubectl logs -f deployment/emd-agent -n $NAMESPACE | grep -E '(EVENT:|STATS:)'"
echo
echo "==> To check resources:"
echo "    kubectl top pods -n $NAMESPACE"
