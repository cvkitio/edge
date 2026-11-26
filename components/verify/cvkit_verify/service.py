"""cvkit-verify: Tier 2. Check Tier 1 detections with a vision model, merge cameras that see the same thing,
publish verified events and raise alerts.

    hub DETECT stream (sourced from every edge box)
      events.detect.<site>.<cam>  kind=start  ─┐
      snapshots.<site>.<cam>.<id>              ├─> window (batch_window_s) ─> dedupe per camera group
                                               │      ─> one vision call per group ─> verdict
                                               └─> events.verified.<site>.<cam>   (every original event)
                                                   alerts.<site>.<cam>            (rules, cooldown) + push sinks

Cost scales with activity, not cameras: only start events are checked, overlapping cameras (config
``groups``) share one call per window, and a daily budget caps spend. Over budget or with the API down,
events are published as unverified and rules with ``unverified = true`` still alert, so a person is never
silently dropped.

The model comes from NATS KV ``cvkit_verify.model`` (written by the weekly benchmark's best-value pick),
else ``model`` in the config. The OpenRouter key is OPENROUTER_API_KEY.

  cvkit-verify -c /etc/cvkit-verify/config.toml
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import logging
import os
import signal
import time
import tomllib
from collections import defaultdict
from dataclasses import dataclass, field

import nats
import requests
from nats.js.api import ConsumerConfig, DeliverPolicy, DiscardPolicy, StorageType, StreamConfig

from . import vision
from .alerts import Alerter, Rule

log = logging.getLogger("cvkit_verify")


@dataclass
class Config:
    nats_url: str = "nats://nats.cvkit:4222"
    source_stream: str = "DETECT"
    subject: str = "events.detect.>"
    durable: str = "verify"
    out_stream: str = "VERIFIED"
    kv_bucket: str = "cvkit_verify"
    model: str = "anthropic/claude-haiku-5.5"
    fallback: str = ""
    batch_window_s: float = 4.0     # collect this long, then dedupe and verify
    max_checks_per_group: int = 3   # per window; after rejections, the next event in the group is checked
    max_usd_day: float = 2.0
    min_tier1_confidence: float = 0.0
    tz: str = "Australia/Hobart"
    status_interval_s: float = 60.0
    groups: dict[str, list[str]] = field(default_factory=dict)   # name -> cameras that overlap
    rules: list[Rule] = field(default_factory=list)


def load(path: str) -> Config:
    d = tomllib.load(open(path, "rb")) if path and os.path.exists(path) else {}
    rules = [Rule(**r) for r in d.pop("alert", [])]
    cfg = Config(**{k: v for k, v in d.items() if k in Config.__dataclass_fields__})
    cfg.rules = rules or [Rule()]
    return cfg


class Verifier:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.key = os.environ.get("OPENROUTER_API_KEY", "")
        self.model = cfg.model
        self.alerter = Alerter(cfg.rules, cfg.tz)
        self.cam_group = {c: g for g, cams in cfg.groups.items() for c in cams}
        self.pending: list[dict] = []
        self.session = requests.Session()
        self.day, self.spent = dt.date.today(), 0.0
        self.stats = defaultdict(int)
        self.budget_warned = False

    def group_of(self, site: str, cam: str) -> str:
        return f"{site}.{self.cam_group.get(cam, cam)}"

    # -- budget ------------------------------------------------------------
    def _budget_left(self) -> bool:
        if dt.date.today() != self.day:
            self.day, self.spent, self.budget_warned = dt.date.today(), 0.0, False
        return self.spent < self.cfg.max_usd_day

    # -- NATS --------------------------------------------------------------
    async def setup(self, js) -> None:
        sc = StreamConfig(name=self.cfg.out_stream, subjects=["events.verified.>", "alerts.>"],
                          storage=StorageType.FILE, max_age=30 * 86400, max_bytes=512 << 20,  # small JSON; the hub reserves bytes per stream
                          discard=DiscardPolicy.OLD, duplicate_window=300,
                          description="cvkit-verify: Tier 2 verdicts and alerts")
        try:
            await js.add_stream(sc)
        except Exception:  # noqa: BLE001  exists with other settings
            await js.update_stream(sc)
        try:
            self.kv = await js.key_value(self.cfg.kv_bucket)
        except Exception:  # noqa: BLE001
            self.kv = await js.create_key_value(bucket=self.cfg.kv_bucket, history=10,
                                                description="cvkit-verify settings (model)")
        await self.refresh_model()

    async def refresh_model(self) -> None:
        try:
            e = await self.kv.get("model")
            m = e.value.decode().strip()
        except Exception:  # noqa: BLE001  no key yet
            m = ""
        m = m or self.cfg.model
        if m != self.model:
            log.info("model %s -> %s", self.model, m)
        self.model = m

    async def snapshot(self, js, doc: dict) -> bytes | None:
        s = doc.get("snapshot") or {}
        if not s.get("subject"):
            return None
        try:
            return (await js.get_last_msg(self.cfg.source_stream, s["subject"], direct=True)).data
        except Exception as ex:  # noqa: BLE001
            log.warning("snapshot %s: %s", s["subject"], ex)
            return None

    # -- the work ----------------------------------------------------------
    def _check(self, jpeg: bytes, doc: dict) -> vision.Verdict:
        claim = vision.Claim(doc["label"], float(doc.get("max_confidence") or doc.get("confidence") or 0),
                             doc["bbox"], doc["camera"])
        v = vision.verify(self.key, self.model, jpeg, claim, timeout=45, max_tokens=2000, session=self.session)
        if not v.ok and self.cfg.fallback and self.cfg.fallback != self.model:
            log.warning("%s failed (%s); trying %s", self.model, v.error, self.cfg.fallback)
            v2 = vision.verify(self.key, self.cfg.fallback, jpeg, claim, timeout=45, max_tokens=2000,
                               session=self.session)
            v2.cost += v.cost
            v = v2
        return v

    async def process(self, js, batch: list[dict]) -> None:
        """Dedupe a window of start events by (camera group, label family) and verify one per group."""
        groups: dict[tuple[str, str], list[dict]] = defaultdict(list)
        for d in batch:
            fam = vision.FAMILY.get(d["label"], d["label"])
            groups[(self.group_of(d["site"], d["camera"]), fam)].append(d)
        self.stats["events"] += len(batch)

        async def one(key, docs):
            # Check the highest-scoring event first. A real verdict covers the rest of the group (the
            # same object seen by overlapping cameras). A rejection covers only that event: the next
            # one may be a different, real object (a hi-vis vest at 0.64 must not hide a person at 0.55).
            docs = sorted(docs, key=lambda d: -float(d.get("max_confidence") or d.get("confidence") or 0))
            checks = 0
            while docs:
                rep = docs[0]
                if checks >= self.cfg.max_checks_per_group:
                    self.stats["unverified"] += 1
                    await self.publish(js, key[0], docs, rep, None, "unverified", "check limit", None)
                    return
                jpeg = await self.snapshot(js, rep)
                v: vision.Verdict | None = None
                status = "unverified"
                if jpeg is None:
                    reason = "no snapshot"
                elif not self.key:
                    reason = "no API key"
                elif not self._budget_left():
                    reason = "daily budget used"
                    if not self.budget_warned:
                        self.budget_warned = True
                        log.warning("daily budget $%.2f used; publishing unverified", self.cfg.max_usd_day)
                else:
                    v = await asyncio.to_thread(self._check, jpeg, rep)
                    checks += 1
                    self.spent += v.cost
                    self.stats["calls"] += 1
                    status = "verified" if v.ok else "unverified"
                    reason = "" if v.ok else v.error
                self.stats[status] += 1
                if v is not None and v.ok and not v.real:
                    await self.publish(js, key[0], [rep], rep, v, status, reason, jpeg)
                    docs = docs[1:]
                    continue
                await self.publish(js, key[0], docs, rep, v, status, reason, jpeg)
                return

        await asyncio.gather(*(one(k, d) for k, d in groups.items()))

    async def publish(self, js, group: str, docs: list[dict], rep: dict, v: vision.Verdict | None,
                      status: str, reason: str, jpeg: bytes | None) -> None:
        real = v.real if (v and v.ok) else None
        label = v.label if (v and v.ok) else vision.FAMILY.get(rep["label"], rep["label"])
        verdict = None
        if v and v.ok:
            verdict = {"real": v.real, "label": v.label, "people": v.people, "confidence": v.confidence,
                       "description": v.description}
        for d in docs:
            out = {"schema": "cvkit.verified.v1", "event_id": d["event_id"], "site": d["site"],
                   "camera": d["camera"], "edge": d.get("edge"), "ts": d["ts"],
                   "verified_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds"),
                   "claim": {"label": d["label"], "confidence": d.get("max_confidence"), "bbox": d["bbox"],
                             "model": d.get("model")},
                   "status": status, "reason": reason, "real": real, "verdict": verdict,
                   "model": v.model if v else None, "cost": round(v.cost, 6) if v else 0,
                   "latency_s": round(v.seconds, 2) if v else None, "group": group,
                   "dedupe_of": None if d is rep else rep["event_id"], "snapshot": rep.get("snapshot")}
            await js.publish(f"events.verified.{d['site']}.{d['camera']}", json.dumps(out).encode(),
                             headers={"Nats-Msg-Id": f"{d['event_id']}:verified"})
        self.stats["dropped" if real is False else "kept"] += len(docs)
        if real is False:
            log.info("%s %s rejected: %s", rep["camera"], rep["label"], v.description if v else "")
            return
        conf = v.confidence if (v and v.ok) else 0.0
        rule = self.alerter.due(rep["camera"], group, label, conf, status == "verified")
        if not rule:
            return
        cams = sorted({d["camera"] for d in docs})
        title = f"cvkit {rep['site']}: {label} on {', '.join(cams)}" + ("" if status == "verified" else " (unverified)")
        text = (v.description if (v and v.ok) else f"Tier 1 saw {rep['label']} ({reason})")
        local = dt.datetime.fromisoformat(rep["ts"]).astimezone(self.alerter.tz).strftime("%H:%M:%S")
        text = f"{local} {text}"
        alert = {"schema": "cvkit.alert.v1", "rule": rule.name, "site": rep["site"], "cameras": cams,
                 "label": label, "status": status, "text": text, "event_id": rep["event_id"], "ts": rep["ts"],
                 "snapshot": rep.get("snapshot"), "verdict": verdict}
        sinks = await asyncio.to_thread(self.alerter.push, title, text, jpeg, alert)
        alert["sinks"] = sinks
        await js.publish(f"alerts.{rep['site']}.{rep['camera']}", json.dumps(alert).encode(),
                         headers={"Nats-Msg-Id": f"{rep['event_id']}:alert"})
        self.stats["alerts"] += 1
        log.info("ALERT %s: %s (%s)", title, text, ",".join(sinks) or "nats only")

    async def run(self) -> None:
        nc = await nats.connect(self.cfg.nats_url, name="cvkit-verify", max_reconnect_attempts=-1)
        js = nc.jetstream()
        await self.setup(js)
        sub = await js.pull_subscribe(self.cfg.subject, durable=self.cfg.durable, stream=self.cfg.source_stream,
                                      config=ConsumerConfig(deliver_policy=DeliverPolicy.NEW, ack_wait=120,
                                                            max_ack_pending=2000))
        log.info("verifying %s from %s with %s (budget $%.2f/day, %d group(s), %d rule(s))", self.cfg.subject,
                 self.cfg.source_stream, self.model, self.cfg.max_usd_day, len(self.cfg.groups), len(self.cfg.rules))
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for s in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(s, stop.set)
        last_status = last_model = time.monotonic()
        while not stop.is_set():
            window_end = time.monotonic() + self.cfg.batch_window_s
            batch, msgs = [], []
            while time.monotonic() < window_end and not stop.is_set():
                try:
                    got = await sub.fetch(200, timeout=max(0.1, window_end - time.monotonic()))
                except nats.errors.TimeoutError:
                    continue
                for m in got:
                    msgs.append(m)
                    try:
                        d = json.loads(m.data)
                    except ValueError:
                        continue
                    if d.get("kind") == "start" and float(d.get("max_confidence") or 0) >= self.cfg.min_tier1_confidence:
                        batch.append(d)
            if batch:
                try:
                    await self.process(js, batch)
                except Exception:  # noqa: BLE001  keep going; the events are redelivered after ack_wait
                    log.exception("processing %d event(s) failed", len(batch))
                    for m in msgs:
                        await m.nak(delay=30)
                    continue
            for m in msgs:
                await m.ack()
            now = time.monotonic()
            if now - last_model > 60:
                last_model = now
                await self.refresh_model()
            if now - last_status > self.cfg.status_interval_s:
                last_status = now
                doc = {"model": self.model, "spent_today": round(self.spent, 4), "budget": self.cfg.max_usd_day,
                       **self.stats}
                log.info("status %s", json.dumps(doc))
                await nc.publish("verify.status", json.dumps(doc).encode())
        await nc.drain()


def main() -> None:
    ap = argparse.ArgumentParser(prog="cvkit-verify", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-c", "--config", default="/etc/cvkit-verify/config.toml")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    asyncio.run(Verifier(load(a.config)).run())


if __name__ == "__main__":
    main()
