#!/bin/bash
# Load Docker image into k3s containerd

set -e

IMAGE_NAME="emd-agent:latest"
TAR_FILE="/tmp/emd-agent.tar"

echo "Saving Docker image to tarball..."
docker save "$IMAGE_NAME" -o "$TAR_FILE"

echo "Finding k3s containerd socket..."
CONTAINERD_SOCK="/run/k3s/containerd/containerd.sock"

if [ -S "$CONTAINERD_SOCK" ]; then
    echo "Importing to k3s containerd..."
    sudo ctr -n k8s.io -a "$CONTAINERD_SOCK" images import "$TAR_FILE"
    echo "Image imported successfully!"
    sudo ctr -n k8s.io -a "$CONTAINERD_SOCK" images ls | grep emd-agent
else
    echo "ERROR: k3s containerd socket not found at $CONTAINERD_SOCK"
    echo "Please run this script on the k3s node directly"
    exit 1
fi
