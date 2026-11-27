"""Shared pieces for building Tier 2 benchmark cases from public datasets.

The flow (evals/datasets/README.md):

  1. <source>.py select   picks images from a dataset and writes a pool: pool.json + pool/<id>.jpg, with the
                          dataset's own boxes (ground truth), what it says is absent, and the licence.
  2. letterbox            every pool image is letterboxed to 640x640 exactly as cvkit-detect does on the Pi
                          (videoscale with pixel-aspect-ratio=1/1 adds borders), into one numbered sequence.
  3. pi_detect.py         runs on the edge Pi: the live Tier 1 model (YOLOv8s HEF on the Hailo-8L, same NMS
                          threshold and post-process) over the sequence, so claims are real detector output.
  4. build.py             matches each detection to the ground truth and keeps the ones whose truth is
                          certain: real (box matches a real object of that family) or false (no such object
                          there, by the dataset's verified labels). Unclear ones are dropped, not guessed.
"""
from __future__ import annotations

import io
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

NET = 640

# COCO classes the Tier 1 model emits, by family (the same families vision.FAMILY uses).
COCO_FAMILY = {"person": "person", "bicycle": "bicycle", "car": "vehicle", "motorcycle": "vehicle", "bus": "vehicle",
               "truck": "vehicle",
               "bird": "animal", "cat": "animal", "dog": "animal", "horse": "animal", "sheep": "animal",
               "cow": "animal", "elephant": "animal", "bear": "animal", "zebra": "animal", "giraffe": "animal"}


def letterbox_geometry(w: int, h: int) -> tuple[float, float, float, float]:
    """(pad_x, pad_y, content_w, content_h) in 640-space, as GStreamer videoscale add-borders lays it out."""
    s = min(NET / w, NET / h)
    cw, ch = w * s, h * s
    return (NET - cw) / 2, (NET - ch) / 2, cw, ch


def letterbox(img: Image.Image) -> Image.Image:
    px, py, cw, ch = letterbox_geometry(*img.size)
    out = Image.new("RGB", (NET, NET), (0, 0, 0))
    out.paste(img.convert("RGB").resize((round(cw), round(ch)), Image.BILINEAR), (round(px), round(py)))
    return out


def unletterbox(box, w: int, h: int) -> list[float]:
    """A box normalised to the 640x640 input -> normalised to the original image."""
    px, py, cw, ch = letterbox_geometry(w, h)
    c = lambda v: min(1.0, max(0.0, v))  # noqa: E731
    x0, y0, x1, y1 = box
    return [round(c((x0 * NET - px) / cw), 4), round(c((y0 * NET - py) / ch), 4),
            round(c((x1 * NET - px) / cw), 4), round(c((y1 * NET - py) / ch), 4)]


def iou(a, b) -> float:
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def inside(a, b, frac: float = 0.8) -> bool:
    """True when at least ``frac`` of box a lies inside box b (for group-of boxes)."""
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    area = (a[2] - a[0]) * (a[3] - a[1])
    return area > 0 and ix * iy / area >= frac


def snapshot(img: Image.Image, box, label: str, score: float, width: int = 960) -> bytes:
    """The frame as Tier 1 saves it: scaled to ``width``, the detector's box and 'label score' drawn in yellow."""
    img = img.convert("RGB")
    if img.width != width:
        img = img.resize((width, round(img.height * width / img.width)), Image.BILINEAR)
    d = ImageDraw.Draw(img)
    w, h = img.size
    x0, y0, x1, y1 = box[0] * w, box[1] * h, box[2] * w, box[3] * h
    d.rectangle([x0, y0, x1, y1], outline=(255, 210, 0), width=2)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 16)
    except OSError:
        font = ImageFont.load_default()
    d.text((x0, max(0, y0 - 18)), f"{label} {score:.2f}", fill=(255, 210, 0), font=font)
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=88)
    return buf.getvalue()


def write_pool(out: Path, records: list[dict]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "pool.json").write_text(json.dumps({"records": records}, indent=1))
