"""Alert rules and sinks.

Every alert is published to NATS (``alerts.<site>.<camera>``, stored in the hub's VERIFIED stream), so
anything can subscribe. On top of that, optional push sinks, each enabled by its environment variables:

  Telegram   TELEGRAM_BOT_TOKEN + TELEGRAM_CHAT_ID    photo with a caption
  Webhook    ALERT_WEBHOOK_URL                         JSON POST (Slack/Teams/ntfy-style receivers)
  ntfy       NTFY_URL (e.g. https://ntfy.sh/<topic>)   photo attachment with a title

Rules decide which verified detections become alerts (config [[alert]] tables):

  [[alert]]
  name = "person after hours"
  labels = ["person"]                 # verifier families: person, vehicle, bicycle, animal
  cameras = ["*"]                     # names or fnmatch patterns
  hours = "18:00-06:30"               # local time window, optional (wraps midnight)
  min_confidence = 0.6                # verifier confidence
  cooldown_s = 300                    # per rule and camera group
"""

from __future__ import annotations

import datetime as dt
import fnmatch
import logging
import os
import time
from dataclasses import dataclass, field

import requests

log = logging.getLogger("cvkit_verify.alerts")


@dataclass
class Rule:
    name: str = "detection"
    labels: list[str] = field(default_factory=lambda: ["person"])
    cameras: list[str] = field(default_factory=lambda: ["*"])
    hours: str = ""
    min_confidence: float = 0.0
    cooldown_s: float = 300.0
    unverified: bool = True       # also alert when verification was impossible (budget, API down)

    def in_hours(self, now: dt.datetime) -> bool:
        if not self.hours:
            return True
        a, b = (dt.time.fromisoformat(x.strip()) for x in self.hours.split("-"))
        t = now.time()
        return a <= t < b if a <= b else (t >= a or t < b)

    def matches(self, camera: str, label: str, confidence: float, verified: bool, now: dt.datetime) -> bool:
        if not verified and not self.unverified:
            return False
        return (label in self.labels and any(fnmatch.fnmatch(camera, p) for p in self.cameras)
                and (not verified or confidence >= self.min_confidence) and self.in_hours(now))


class Alerter:
    def __init__(self, rules: list[Rule], tz: str = ""):
        self.rules = rules
        self.tz = dt.timezone.utc
        if tz:
            from zoneinfo import ZoneInfo
            self.tz = ZoneInfo(tz)
        self.last: dict[tuple[str, str], float] = {}
        self.sent = 0
        self.session = requests.Session()

    def due(self, camera: str, group: str, label: str, confidence: float, verified: bool) -> Rule | None:
        now = dt.datetime.now(self.tz)
        for r in self.rules:
            if r.matches(camera, label, confidence, verified, now):
                k = (r.name, group)
                if time.time() - self.last.get(k, 0) < r.cooldown_s:
                    return None
                self.last[k] = time.time()
                return r
        return None

    def push(self, title: str, text: str, jpeg: bytes | None, doc: dict) -> list[str]:
        """Send to every configured sink; returns the ones that worked. Never raises."""
        ok = []
        tok, chat = os.environ.get("TELEGRAM_BOT_TOKEN"), os.environ.get("TELEGRAM_CHAT_ID")
        if tok and chat:
            try:
                base = f"https://api.telegram.org/bot{tok}"
                caption = f"{title}\n{text}"[:1000]
                if jpeg:
                    r = self.session.post(f"{base}/sendPhoto", data={"chat_id": chat, "caption": caption},
                                          files={"photo": ("snapshot.jpg", jpeg, "image/jpeg")}, timeout=20)
                else:
                    r = self.session.post(f"{base}/sendMessage", data={"chat_id": chat, "text": caption}, timeout=20)
                (ok.append("telegram") if r.ok else log.warning("telegram: HTTP %s", r.status_code))
            except requests.RequestException as ex:
                log.warning("telegram: %s", type(ex).__name__)
        url = os.environ.get("ALERT_WEBHOOK_URL")
        if url:
            try:
                r = self.session.post(url, json={"text": f"{title}: {text}", **doc}, timeout=15)
                (ok.append("webhook") if r.ok else log.warning("webhook: HTTP %s", r.status_code))
            except requests.RequestException as ex:
                log.warning("webhook: %s", type(ex).__name__)
        ntfy = os.environ.get("NTFY_URL")
        if ntfy:
            try:
                h = {"Title": title.encode("ascii", "replace").decode(), "Message": text[:400].replace("\n", " "),
                     "Tags": "rotating_light"}
                r = self.session.put(ntfy, data=jpeg or text.encode(), headers={**h, **({"Filename": "snapshot.jpg"} if jpeg else {})},
                                     timeout=20)
                (ok.append("ntfy") if r.ok else log.warning("ntfy: HTTP %s", r.status_code))
            except requests.RequestException as ex:
                log.warning("ntfy: %s", type(ex).__name__)
        self.sent += 1
        return ok
