#!/bin/bash
# Port forward script for EMD Agent

NAMESPACE="emd"

echo "========================================="
echo "EMD Agent Port Forwarding"
echo "========================================="
echo ""

# Check if pod is ready
POD_NAME=$(kubectl get pods -n "$NAMESPACE" -l app=emd-agent -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)

if [ -z "$POD_NAME" ]; then
    echo "ERROR: No EMD agent pod found in namespace $NAMESPACE"
    echo "Please deploy first with: ./k8s/deploy-complete.sh"
    exit 1
fi

POD_STATUS=$(kubectl get pod "$POD_NAME" -n "$NAMESPACE" -o jsonpath='{.status.phase}')
echo "Pod: $POD_NAME"
echo "Status: $POD_STATUS"
echo ""

if [ "$POD_STATUS" != "Running" ]; then
    echo "WARNING: Pod is not running yet (status: $POD_STATUS)"
    echo "Waiting for pod to be ready..."
    kubectl wait --for=condition=ready pod "$POD_NAME" -n "$NAMESPACE" --timeout=60s || {
        echo "ERROR: Pod failed to become ready"
        kubectl get pod "$POD_NAME" -n "$NAMESPACE"
        exit 1
    }
fi

echo "Setting up port forwards..."
echo "  8080 -> API Server (Web UI + Swagger)"
echo "  9464 -> Metrics Server (Prometheus + Health)"
echo ""
echo "Access URLs:"
echo "  Web UI:       http://localhost:8080/"
echo "  Swagger Docs: http://localhost:8080/docs"
echo "  API Clips:    http://localhost:8080/api/clips"
echo "  Health:       http://localhost:9464/healthz"
echo "  Metrics:      http://localhost:9464/metrics"
echo ""
echo "Press Ctrl+C to stop port forwarding"
echo "========================================="
echo ""

# Start port forwards in background and wait
kubectl port-forward -n "$NAMESPACE" svc/emd-agent-api 8080:8080 9464:9464
