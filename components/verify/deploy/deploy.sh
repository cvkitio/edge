#!/usr/bin/env bash
# Build cvkit-verify, import it into k3s on nas01, (re)create its secrets from 1Password, apply.
#
#   components/verify/deploy/deploy.sh
#
# Secrets are set only when their variable is given, one field at a time, so a deploy never drops a
# field set earlier. Values go straight from `op read` (or the environment) into kubectl, never to disk:
#   OPENROUTER_REF=op://...        -> cvkit-verify OPENROUTER_API_KEY       (service key)
#   OPENROUTER_EVAL_REF=op://...   -> cvkit-verify-eval OPENROUTER_API_KEY  (benchmark key)
#   TELEGRAM_REF=op://... TELEGRAM_CHAT_ID=...  NTFY_URL=...  ALERT_WEBHOOK_URL=...
# (26 Nov 2026: the cvkitio and cvkitio-eval keys were created with the OpenRouter management key and
#  exist only in these Secrets until they are copied into 1Password.)
set -euo pipefail
cd "$(dirname "$0")/.."
CTX=${KUBE_CONTEXT:-nas01}
TAG=$(git rev-parse --short HEAD)$(git diff --quiet -- . || echo "-dirty")
docker build -q -t "cvkit-verify:$TAG" .
docker save "cvkit-verify:$TAG" | sudo -n k3s ctr -n k8s.io images import - >/dev/null

set_field() {  # secret key value: create the secret if needed, then merge one field
  kubectl --context "$CTX" -n cvkit get secret "$1" >/dev/null 2>&1 ||
    kubectl --context "$CTX" -n cvkit create secret generic "$1" >/dev/null
  kubectl --context "$CTX" -n cvkit patch secret "$1" --type merge \
    -p "{\"data\":{\"$2\":\"$(printf %s "$3" | base64 -w0)\"}}" >/dev/null
}
[ -n "${OPENROUTER_REF:-}" ] && set_field cvkit-verify OPENROUTER_API_KEY "$(op read "$OPENROUTER_REF")"
[ -n "${OPENROUTER_EVAL_REF:-}" ] && set_field cvkit-verify-eval OPENROUTER_API_KEY "$(op read "$OPENROUTER_EVAL_REF")"
if [ -n "${TELEGRAM_REF:-}" ]; then
  set_field cvkit-verify TELEGRAM_BOT_TOKEN "$(op read "$TELEGRAM_REF" | grep -oE '[0-9]{6,}:[A-Za-z0-9_-]{30,}' | head -1)"
fi
[ -n "${TELEGRAM_CHAT_ID:-}" ] && set_field cvkit-verify TELEGRAM_CHAT_ID "$TELEGRAM_CHAT_ID"
[ -n "${NTFY_URL:-}" ] && set_field cvkit-verify NTFY_URL "$NTFY_URL"
[ -n "${ALERT_WEBHOOK_URL:-}" ] && set_field cvkit-verify ALERT_WEBHOOK_URL "$ALERT_WEBHOOK_URL"
for s in cvkit-verify cvkit-verify-eval; do
  kubectl --context "$CTX" -n cvkit get secret "$s" >/dev/null || { echo "secret $s missing" >&2; exit 1; }
done

mkdir -p /srv/data/cvkit/verify/results /srv/data/share/cvkitio/vision-benchmark
sed "s/IMAGE_TAG/$TAG/g" deploy/k8s.yaml | kubectl --context "$CTX" apply -f -
kubectl --context "$CTX" -n cvkit rollout restart deploy/cvkit-verify >/dev/null
kubectl --context "$CTX" -n cvkit rollout status deploy/cvkit-verify --timeout=120s
