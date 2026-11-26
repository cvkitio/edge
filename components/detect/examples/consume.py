"""Example downstream consumer: read detection events from the hub and save snapshots.

This is the shape every further-processing step takes (a vision-model
describer, alerting, clip upload): a durable pull consumer on the hub's DETECT
stream, so it resumes where it left off after a restart and never misses an
event that arrived while it was down.

    pip install nats-py
    python consume.py --server nats://nats.cvkit:4222 --durable describer --out ./snaps

From outside the cluster: kubectl -n cvkit port-forward svc/nats 4222 and use
nats://127.0.0.1:4222.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path

import nats
from nats.js.api import AckPolicy, ConsumerConfig, DeliverPolicy


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default="nats://127.0.0.1:4222")
    ap.add_argument("--domain", default="hub", help="JetStream domain holding DETECT")
    ap.add_argument("--durable", default="example")
    ap.add_argument("--subject", default="events.detect.>", help="e.g. events.detect.home.driveway")
    ap.add_argument("--out", type=Path, default=Path("snaps"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    nc = await nats.connect(args.server)
    js = nc.jetstream(domain=args.domain)
    sub = await js.pull_subscribe(
        args.subject, durable=args.durable, stream="DETECT",
        config=ConsumerConfig(ack_policy=AckPolicy.EXPLICIT, deliver_policy=DeliverPolicy.ALL, max_ack_pending=100),
    )
    print(f"consuming {args.subject} from DETECT as {args.durable!r}; ctrl-c to stop")
    try:
        while True:
            try:
                msgs = await sub.fetch(10, timeout=5)
            except nats.errors.TimeoutError:
                continue
            for m in msgs:
                ev = json.loads(m.data)
                line = f"{ev['ts']} {ev['site']}/{ev['camera']} {ev['kind']:5} {ev['label']} " \
                       f"conf={ev['max_confidence']:.2f}"
                if ev["kind"] == "end":
                    line += f" duration={ev['duration_s']}s"
                snap = ev.get("snapshot")
                if snap:
                    # Snapshots sit on a per-event subject, so fetch the last (only) one by subject.
                    raw = await js.get_last_msg("DETECT", snap["subject"], direct=True)
                    path = args.out / f"{ev['event_id']}.jpg"
                    path.write_bytes(raw.data)
                    line += f" -> {path}"
                print(line)
                await m.ack()
    finally:
        await nc.drain()


if __name__ == "__main__":
    asyncio.run(main())
