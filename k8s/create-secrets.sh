#!/bin/bash
# Create Kubernetes secrets from .env file
# Usage: ./k8s/create-secrets.sh

set -e

# Check if .env exists
if [ ! -f ".env" ]; then
    echo "❌ Error: .env file not found"
    echo "📝 Copy .env.example to .env and fill in your credentials"
    echo ""
    echo "   cp .env.example .env"
    echo "   # Edit .env with your actual values"
    echo ""
    exit 1
fi

# Load .env file
echo "📦 Loading environment variables from .env..."
export $(grep -v '^#' .env | xargs)

# Validate required variables
REQUIRED_VARS=("GITHUB_TOKEN" "GITHUB_USERNAME" "CAMERA_USERNAME" "CAMERA_PASSWORD")
for var in "${REQUIRED_VARS[@]}"; do
    if [ -z "${!var}" ]; then
        echo "❌ Error: Required variable $var is not set in .env"
        exit 1
    fi
done

echo "✅ All required variables found"
echo ""

# Create namespace if it doesn't exist
kubectl create namespace emd --dry-run=client -o yaml | kubectl apply -f -

# Create image pull secret for GHCR
echo "🔐 Creating GHCR image pull secret..."
kubectl create secret docker-registry ghcr-secret \
  --docker-server=ghcr.io \
  --docker-username="${GITHUB_USERNAME}" \
  --docker-password="${GITHUB_TOKEN}" \
  --docker-email="${GITHUB_USERNAME}@users.noreply.github.com" \
  --namespace=emd \
  --dry-run=client -o yaml | kubectl apply -f -

echo "✅ GHCR secret created/updated"
echo ""

# Note: Camera credentials should be in agent.toml, not as a k8s secret
# because the TOML config format doesn't easily support environment variable substitution
echo "📝 Note: Camera credentials should be set in k8s/agent.toml"
echo "   The RTSP URL format is: rtsp://${CAMERA_USERNAME}:${CAMERA_PASSWORD}@<camera-ip>/path"
echo ""
echo "✅ Secrets setup complete!"
echo ""
echo "Next steps:"
echo "1. Update k8s/agent.toml with camera credentials"
echo "2. Deploy: ./k8s/deploy-kubectl.sh"
