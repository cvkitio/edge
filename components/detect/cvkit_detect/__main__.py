"""cvkit-detect: on-device detection on a Hailo, published to NATS JetStream.

Subjects (all stored in the local EDGE_DETECT stream, domain edge01, which the
hub's DETECT stream sources):

    events.motion.<site>.<camera>               JSON, Tier 0 gate open/close (no decode)
    events.detect.<site>.<camera>               JSON, schema cvkit.detect.v1 (Tier 1, Hailo)
    snapshots.<site>.<camera>.<event_id>        JPEG for a start event

and, not stored, a heartbeat every status_interval_s:

    edge.status.<site>.<edge>                   JSON
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import signal
import socket
import sys
import time

import gi
import nats
from nats.js.api import DiscardPolicy, StorageType, StreamConfig

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

from . import config as config_mod  # noqa: E402
from . import pipeline as pipeline_mod  # noqa: E402
from .pipeline import CameraPipeline, Result, Tier1Pool  # noqa: E402

log = logging.getLogger("cvkit_detect")


class App:
    def __init__(self, cfg: config_mod.Config):
        self.cfg = cfg
        self.queue: asyncio.Queue[Result] = asyncio.Queue(maxsize=1000)
        self.cams: list[CameraPipeline] = []
        self.started = time.time()
        self.published = 0
        self.motion_published = 0
        self.publish_errors = 0

    def _on_result(self, r: Result) -> None:
        try:
            self.queue.put_nowait(r)
        except asyncio.QueueFull:
            self.publish_errors += 1
            log.error("publish queue full; dropping %d events from %s", len(r.events), r.camera)

    async def _ensure_stream(self, js) -> None:
        sc = StreamConfig(
            name=self.cfg.stream,
            subjects=["events.detect.>", "events.motion.>", "snapshots.>"],
            storage=StorageType.FILE,
            max_age=self.cfg.stream_max_age_days * 86400,
            max_bytes=self.cfg.stream_max_bytes,
            discard=DiscardPolicy.OLD,
            duplicate_window=120,
            allow_direct=True,
            description="cvkit-detect events and snapshots from this edge box",
        )
        try:
            await js.add_stream(sc)
        except Exception:  # already exists with different config -> update
            await js.update_stream(sc)

    async def _publisher(self, js) -> None:
        site = self.cfg.site
        while True:
            r = await self.queue.get()
            for m in r.motion:
                doc = {"schema": "cvkit.motion.v1", "site": site, "camera": r.camera, "edge": self.cfg.edge, **m}
                try:
                    await js.publish(f"events.motion.{site}.{r.camera}", json.dumps(doc).encode(),
                                     headers={"Nats-Msg-Id": f"{r.camera}:{m['kind']}:{m['ts']:.3f}"})
                    self.motion_published += 1
                except Exception as ex:
                    self.publish_errors += 1
                    log.error("motion publish failed for %s: %s", r.camera, ex)
            for e in r.events:
                payload = e.to_dict(site=site, camera=r.camera, edge=self.cfg.edge, model=self.cfg.model)
                jpg = r.snapshots.get(e.event_id)
                try:
                    if jpg:
                        subj = f"snapshots.{site}.{r.camera}.{e.event_id}"
                        await js.publish(subj, jpg, headers={
                            "Nats-Msg-Id": f"{e.event_id}:snapshot",
                            "Content-Type": "image/jpeg",
                            "Cvkit-Event-Id": e.event_id,
                        })
                        payload["snapshot"] = {"subject": subj, "bytes": len(jpg)}
                    await js.publish(f"events.detect.{site}.{r.camera}", json.dumps(payload).encode(),
                                     headers={"Nats-Msg-Id": f"{e.event_id}:{e.kind}"})
                    self.published += 1
                    log.info("%s %s %s track=%d conf=%.2f%s", r.camera, e.kind, e.label, e.track_id,
                             e.max_confidence, f" duration={payload['duration_s']}s" if e.kind == "end" else "")
                except Exception as ex:  # local server down: JetStream is on this box, so rare
                    self.publish_errors += 1
                    log.error("publish failed for %s: %s", e.event_id, ex)

    async def _status(self, nc) -> None:
        subj = f"edge.status.{self.cfg.site}.{self.cfg.edge}"
        prev = {c.cam.name: (0, 0, 0.0, time.monotonic()) for c in self.cams}
        while True:
            await asyncio.sleep(self.cfg.status_interval_s)
            cams = {}
            for c in self.cams:
                f0, t10, open0, t0 = prev[c.cam.name]
                now = time.monotonic()
                dt = max(now - t0, 1e-6)
                open_s = c.gate.total_open(time.time())
                cams[c.cam.name] = {
                    "state": c.stats.state,
                    "fps": round((c.stats.frames - f0) / dt, 2),
                    "tier1_fps": round((c.stats.tier1_frames - t10) / dt, 2),
                    "gate": "open" if c.gate.is_open else "closed",
                    "gate_duty": round(min(1.0, (open_s - open0) / dt), 3),
                    "gate_opens": c.gate.opens,
                    "frames": c.stats.frames,
                    "tier1_frames": c.stats.tier1_frames,
                    "slot_waits": c.waits,
                    "events": c.stats.events,
                    "restarts": c.stats.restarts,
                    "source_size": c.stats.source_size,
                    "last_error": c.stats.last_error,
                }
                prev[c.cam.name] = (c.stats.frames, c.stats.tier1_frames, open_s, now)
            doc = {"edge": self.cfg.edge, "site": self.cfg.site, "model": self.cfg.model,
                   "uptime_s": round(time.time() - self.started), "published": self.published,
                   "publish_errors": self.publish_errors, "queue": self.queue.qsize(),
                   "tier1": {"slots": len(self.pool.all_slots()),
                             "busy": sum(1 for s in self.pool.all_slots() if s.owner is not None),
                             "waiting": len(self.pool.waiting), "denied": self.pool.denied},
                   "cameras": cams}
            log.info("status tier1=%s %s", json.dumps(doc["tier1"]), json.dumps(cams))
            try:
                await nc.publish(subj, json.dumps(doc).encode())
            except Exception as ex:
                log.warning("status publish failed: %s", ex)

    async def run(self) -> None:
        Gst.init(None)
        loop = asyncio.get_running_loop()
        nc = await nats.connect(self.cfg.nats_url, name=f"cvkit-detect@{self.cfg.edge}",
                                max_reconnect_attempts=-1, reconnect_time_wait=2)
        js = nc.jetstream()
        await self._ensure_stream(js)
        log.info("connected to %s; stream %s; model %s; %d camera(s)",
                 self.cfg.nats_url, self.cfg.stream, self.cfg.model, len(self.cfg.cameras))

        self.pool = Tier1Pool(self.cfg, self.cfg.cameras)
        self.cams = [CameraPipeline(self.cfg, cam, loop, self._on_result, self.pool) for cam in self.cfg.cameras]
        stop = asyncio.Event()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, stop.set)

        pool_task = asyncio.create_task(self.pool.run(stop.is_set), name="tier1-pool")
        await asyncio.sleep(0.5)  # let the slots reach PLAYING before cameras borrow them
        log.info("tier1: %d slot(s) for %d camera(s)", len(self.pool.all_slots()), len(self.cams))
        tasks = [asyncio.create_task(c.run(), name=c.cam.name) for c in self.cams]
        pub = asyncio.create_task(self._publisher(js))
        status = asyncio.create_task(self._status(nc))

        await stop.wait()
        log.info("stopping")
        for c in self.cams:
            c.stop()
        await asyncio.gather(*tasks, return_exceptions=True)  # each flushes open events
        await asyncio.gather(pool_task, return_exceptions=True)
        # The Tier 1 slots are not set to NULL: libgsthailo can segfault in
        # gst_object_unref when several hailonets sharing a vdevice tear down,
        # which would lose the end events still queued. main() exits with
        # os._exit instead, and closing the process releases the Hailo.
        while not self.queue.empty():  # let the publisher drain the end events
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.2)
        pub.cancel()
        status.cancel()
        await nc.drain()


def main() -> None:
    ap = argparse.ArgumentParser(prog="cvkit-detect", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--config", default="/etc/cvkit/detect.toml")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = config_mod.load(args.config)
    if not cfg.edge:
        cfg.edge = socket.gethostname()
    loop = asyncio.new_event_loop()
    code = 0
    try:
        loop.run_until_complete(App(cfg).run())
    except Exception:
        logging.getLogger("cvkit_detect").exception("fatal")
        code = 1
    if pipeline_mod.abandoned:
        logging.getLogger("cvkit_detect").warning(
            "exiting without joining %d stuck pipeline(s)", pipeline_mod.abandoned)
    # Events are flushed and NATS drained. Leave without running GStreamer/HailoRT
    # finalizers (see App.run) or joining stuck streaming threads.
    logging.shutdown()
    sys.stdout.flush()
    os._exit(code)


if __name__ == "__main__":
    main()
