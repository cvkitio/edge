#!/usr/bin/env bash
# Create (or update) the hub's DETECT stream, sourcing one EDGE_DETECT stream
# per edge box from that box's JetStream domain. Run anywhere the nats CLI can
# reach a server that is part of the leaf topology, e.g. on the edge box:
#
#   bash hub-streams.sh edge01 [edge02 ...]
#
# The hub stream has no subjects of its own: the edge publishes on
# events.detect.> locally, and that traffic also crosses the leaf link live, so
# capturing it by subject on the hub would store every message twice. Sourcing
# is the durable path; live subscribers on the hub still see events at once.
set -euo pipefail
NATS=${NATS:-nats}
[ $# -ge 1 ] || { echo "usage: $0 <edge-domain>..." >&2; exit 2; }

sources=""
for d in "$@"; do
  sources+="{\"name\":\"EDGE_DETECT\",\"external\":{\"api\":\"\$JS.$d.API\"}},"
done

cfg=$(mktemp); trap 'rm -f "$cfg"' EXIT
cat >"$cfg" <<EOF
{
  "name": "DETECT",
  "description": "cvkit-detect events and snapshots from every edge box",
  "subjects": [],
  "sources": [${sources%,}],
  "retention": "limits",
  "storage": "file",
  "discard": "old",
  "max_age": $((30 * 86400 * 1000000000)),
  "max_bytes": $((8 * 1024 * 1024 * 1024)),
  "allow_direct": true,
  "num_replicas": 1
}
EOF

if $NATS --js-domain hub stream info DETECT >/dev/null 2>&1; then
  $NATS --js-domain hub stream edit DETECT --config "$cfg" --force
else
  $NATS --js-domain hub stream add DETECT --config "$cfg"
fi
$NATS --js-domain hub stream info DETECT -j | grep -E '"(messages|bytes|lag|active)"' || true
