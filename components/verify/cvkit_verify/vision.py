"""Ask a vision model whether a Tier 1 detection is real (OpenRouter, OpenAI-compatible API).

Shared by the verify service and the benchmark, so the benchmark measures exactly
what production sends: the snapshot (the detector's box is drawn on it) plus a
zoomed crop around the box, which is what lets a model judge a person 30 m away
in a 960 px fisheye frame.
"""

from __future__ import annotations

import base64
import io
import json
import re
import time
from dataclasses import asdict, dataclass, field

import requests
from PIL import Image

API = "https://openrouter.ai/api/v1/chat/completions"
LABELS = ("person", "vehicle", "bicycle", "animal", "none")
# YOLO/COCO classes -> the families the verifier answers in.
FAMILY = {"person": "person", "car": "vehicle", "truck": "vehicle", "bus": "vehicle", "motorcycle": "vehicle",
          "bicycle": "bicycle", "dog": "animal", "cat": "animal", "bird": "animal", "horse": "animal"}

SYSTEM = """You check detections from a security camera's on-device object detector before anyone is alerted.

You get two images: the full camera frame with the detector's box drawn on it (yellow, labelled with the class
and score), and a zoomed crop around that box. Decide whether the box really contains the claimed kind of object.

- Families: person; vehicle (car, truck, bus, van, forklift, motorcycle); bicycle; animal.
- "real" is true when the box holds a real object of the claimed family, even if the exact type differs
  (a truck labelled "car" is real). A person riding a bicycle makes both "person" and "bicycle" real.
- Cameras are fisheye and objects can be small and far away; look at the crop.
- Common false alarms: hi-vis clothing on a hook, ladders, fire extinguishers, bins, tanks, pallets, signs,
  shadows, reflections and screens.
- "label" is what is mainly inside the box: person, vehicle, bicycle, animal or none.
- "people" counts the people visible anywhere in the full frame.

Return ONLY JSON:
{"real": true|false, "label": "person|vehicle|bicycle|animal|none", "people": <int>,
 "confidence": <0..1>, "description": "<one short sentence on what is happening>"}"""


@dataclass
class Claim:
    label: str
    confidence: float
    bbox: list[float]  # normalised x0, y0, x1, y1
    camera: str = ""


@dataclass
class Verdict:
    model: str
    ok: bool                    # a valid answer came back
    real: bool = False
    label: str = "none"
    people: int = 0
    confidence: float = 0.0
    description: str = ""
    cost: float = 0.0           # USD, as charged by OpenRouter
    seconds: float = 0.0
    tokens_in: int = 0
    tokens_out: int = 0
    error: str = ""
    raw: str = field(default="", repr=False)

    def to_dict(self) -> dict:
        d = asdict(self)
        d.pop("raw")
        return d


def crop(jpeg: bytes, bbox: list[float], out: int = 448) -> bytes:
    """A square crop around the box, 2.5x its size (at least 15% of the frame), scaled to ``out`` px."""
    im = Image.open(io.BytesIO(jpeg)).convert("RGB")
    w, h = im.size
    x0, y0, x1, y1 = bbox
    cx, cy = (x0 + x1) / 2 * w, (y0 + y1) / 2 * h
    s = max((x1 - x0) * w, (y1 - y0) * h) * 2.5
    s = max(s, 0.15 * max(w, h))
    box = (int(cx - s / 2), int(cy - s / 2), int(cx + s / 2), int(cy + s / 2))
    c = im.crop(box).resize((out, out), Image.LANCZOS)
    buf = io.BytesIO()
    c.save(buf, "JPEG", quality=85)
    return buf.getvalue()


def _data_url(jpeg: bytes) -> str:
    return "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()


def messages(jpeg: bytes, claim: Claim) -> list[dict]:
    text = (f"Camera {claim.camera or 'unknown'}. The detector says: {claim.label} "
            f"(score {claim.confidence:.2f}) at box x0={claim.bbox[0]:.3f} y0={claim.bbox[1]:.3f} "
            f"x1={claim.bbox[2]:.3f} y1={claim.bbox[3]:.3f} (fractions of the frame). "
            "Image 1 is the full frame, image 2 the zoomed crop.")
    return [
        {"role": "system", "content": SYSTEM},
        {"role": "user", "content": [
            {"type": "text", "text": text},
            {"type": "image_url", "image_url": {"url": _data_url(jpeg)}},
            {"type": "image_url", "image_url": {"url": _data_url(crop(jpeg, claim.bbox))}},
        ]},
    ]


def parse(model: str, text: str) -> Verdict:
    m = re.search(r"\{.*\}", text or "", re.S)
    if not m:
        return Verdict(model, False, error="no JSON in answer", raw=text or "")
    try:
        d = json.loads(m.group(0))
        label = str(d.get("label", "none")).lower().strip()
        label = FAMILY.get(label, label)
        if label not in LABELS:
            label = "none"
        real = d.get("real")
        if isinstance(real, str):
            real = real.strip().lower() in ("true", "yes", "1")
        return Verdict(model, True, real=bool(real), label=label, people=int(d.get("people") or 0),
                       confidence=float(d.get("confidence") or 0), description=str(d.get("description", ""))[:300],
                       raw=text)
    except (ValueError, TypeError) as ex:
        return Verdict(model, False, error=f"bad JSON: {ex}", raw=text)


def verify(key: str, model: str, jpeg: bytes, claim: Claim, timeout: float = 60, max_tokens: int = 4000,
           session: requests.Session | None = None) -> Verdict:
    """One verification call. Never raises: failures come back as ok=False with ``error`` set."""
    body = {"model": model, "messages": messages(jpeg, claim), "max_tokens": max_tokens, "temperature": 0,
            "usage": {"include": True}}
    t0 = time.monotonic()
    try:
        r = (session or requests).post(API, json=body, timeout=timeout, headers={
            "Authorization": f"Bearer {key}", "X-Title": "cvkit-verify",
            "HTTP-Referer": "https://github.com/cvkitio/edge"})
        secs = time.monotonic() - t0
        if r.status_code != 200:
            return Verdict(model, False, seconds=secs, error=f"HTTP {r.status_code}: {r.text[:200]}")
        j = r.json()
        if "error" in j:
            return Verdict(model, False, seconds=secs, error=str(j["error"])[:200])
        msg = j["choices"][0]["message"]
        v = parse(model, msg.get("content") or "")
        u = j.get("usage") or {}
        v.cost = float(u.get("cost") or 0)
        v.tokens_in, v.tokens_out = int(u.get("prompt_tokens") or 0), int(u.get("completion_tokens") or 0)
        v.seconds = secs
        return v
    except (requests.RequestException, ValueError, KeyError, IndexError) as ex:
        return Verdict(model, False, seconds=time.monotonic() - t0, error=f"{type(ex).__name__}: {ex}"[:200])
