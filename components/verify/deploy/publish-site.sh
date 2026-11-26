#!/usr/bin/env bash
# Upload the vision benchmark's public page to https://cvkit.net/vision-benchmark.html.
#
# The monthly CronJob (k8s.yaml, vision-benchmark) writes the page to $SRC on nas01. The pod has no AWS
# credentials, so this runs on the host as andrew with the synced `personal` profile (account 826182175287,
# the cvkit.net bucket from cvkitio/infra site/terraform). It uploads only when the page changed, then
# invalidates that one path. Run daily by the systemd user timer in systemd/, or by hand.
set -euo pipefail

SRC=${SRC:-/srv/data/cvkit/verify/public/vision-benchmark.html}
BUCKET=${BUCKET:-cvkit-site-826182175287}
DIST=${DIST:-E3BZJ5S5M8UIBK}
KEY=vision-benchmark.html
AWS=(aws --profile personal)

[ -s "$SRC" ] || { echo "no page at $SRC yet"; exit 0; }
acct=$("${AWS[@]}" sts get-caller-identity --query Account --output text)
[ "$acct" = 826182175287 ] || { echo "profile personal is account $acct, not 826182175287" >&2; exit 1; }

local_md5=$(md5sum "$SRC" | cut -d' ' -f1)
remote_md5=$("${AWS[@]}" s3api head-object --bucket "$BUCKET" --key "$KEY" --query ETag --output text 2>/dev/null | tr -d '"' || true)
if [ "$local_md5" = "$remote_md5" ]; then
  echo "unchanged"
  exit 0
fi
"${AWS[@]}" s3 cp "$SRC" "s3://$BUCKET/$KEY" --content-type "text/html; charset=utf-8" \
  --cache-control "public, max-age=3600" --only-show-errors
"${AWS[@]}" cloudfront create-invalidation --distribution-id "$DIST" --paths "/$KEY" \
  --query Invalidation.Id --output text
echo "published https://cvkit.net/$KEY"
