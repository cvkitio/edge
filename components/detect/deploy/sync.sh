#!/usr/bin/env bash
# Copy the detect component and the emd inspector source it builds to an edge box.
#   components/detect/deploy/sync.sh pi@10.3.0.51
set -euo pipefail
HOST=${1:?usage: sync.sh user@edge}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
EMD="$HERE/../emd"
rsync -a --delete --exclude native --exclude __pycache__ --exclude .pytest_cache "$HERE/" "$HOST:cvkit-detect/"
rsync -a --delete "$EMD/src/emd_inspector.c" "$HOST:cvkit-detect/native/"
rsync -a --delete "$EMD/include/" "$HOST:cvkit-detect/native/include/"
