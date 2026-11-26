"""Turn per-frame tracked detections into a small number of events.

The Hailo tracker gives every object a stable track id across frames. A raw
detection stream is far too chatty to publish (15 fps x N objects), so this
module keeps one record per track and emits:

- ``start`` once a track of an interesting label has been seen for
  ``min_frames`` frames (filters single-frame false positives), and
- ``end`` once that track has not been seen for ``lost_after_s`` seconds,
  carrying the duration and the best confidence seen.

A track must also have *moved* (its box centre travelled ``min_move`` of the
frame since it was first seen) before it starts an event. A fire extinguisher
YOLO calls a person, or a parked forklift, never moves, so it never becomes an
event however long it is detected.

Nothing here touches GStreamer, Hailo or NATS, so it can be unit-tested on any
machine. Time is passed in explicitly for the same reason.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone

SCHEMA = "cvkit.detect.v1"


@dataclass(frozen=True)
class Detection:
    """One tracked object in one frame. bbox is normalised (x0, y0, x1, y1)."""

    track_id: int
    label: str
    confidence: float
    bbox: tuple[float, float, float, float]


@dataclass
class _Track:
    track_id: int
    label: str
    first_seen: float
    last_seen: float
    frames: int = 1
    max_confidence: float = 0.0
    best_bbox: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 0.0)
    event_id: str | None = None  # set once a start event has been emitted
    origin: tuple[float, float] = (0.0, 0.0)  # box centre when first seen
    moved: float = 0.0  # furthest the centre has been from origin (normalised)
    last_moved: float = -1e18  # last time it moved min_move within the past window_s
    hist: list = field(default_factory=list)  # (t, cx, cy) over the last window_s


def _centre(b: tuple[float, float, float, float]) -> tuple[float, float]:
    return ((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)


@dataclass
class EventPolicy:
    labels: frozenset[str] = frozenset({"person", "car", "truck", "bus", "motorcycle", "bicycle", "dog", "cat"})
    min_confidence: float = 0.5
    min_frames: int = 5
    lost_after_s: float = 3.0
    min_move: float = 0.02  # centre displacement (fraction of frame) before a track counts; 0 disables
    move_window_s: float = 2.0  # "still moving" means min_move within this window


@dataclass
class Event:
    kind: str  # "start" | "end"
    event_id: str
    track_id: int
    label: str
    confidence: float
    max_confidence: float
    bbox: tuple[float, float, float, float]
    frames: int
    first_seen: float
    last_seen: float
    want_snapshot: bool = False

    def to_dict(self, *, site: str, camera: str, edge: str, model: str) -> dict:
        def iso(t: float) -> str:
            return datetime.fromtimestamp(t, tz=timezone.utc).isoformat(timespec="milliseconds")

        return {
            "schema": SCHEMA,
            "event_id": self.event_id,
            "kind": self.kind,
            "site": site,
            "camera": camera,
            "edge": edge,
            "model": model,
            "ts": iso(self.last_seen if self.kind == "end" else self.first_seen),
            "first_seen": iso(self.first_seen),
            "last_seen": iso(self.last_seen),
            "duration_s": round(self.last_seen - self.first_seen, 3),
            "track_id": self.track_id,
            "label": self.label,
            "confidence": round(self.confidence, 3),
            "max_confidence": round(self.max_confidence, 3),
            "bbox": [round(v, 4) for v in self.bbox],
            "frames": self.frames,
        }


@dataclass
class EventTracker:
    """Per-camera state machine. Call ``update`` once per inferred frame."""

    policy: EventPolicy = field(default_factory=EventPolicy)
    _tracks: dict[int, _Track] = field(default_factory=dict)

    def update(self, now: float, detections: list[Detection]) -> list[Event]:
        events: list[Event] = []
        p = self.policy

        for d in detections:
            if d.label not in p.labels or d.confidence < p.min_confidence:
                continue
            t = self._tracks.get(d.track_id)
            if t is None or t.label != d.label:
                # New track, or the tracker re-used an id for a different class.
                if t is not None and t.event_id:
                    events.append(self._end(t))
                t = _Track(d.track_id, d.label, first_seen=now, last_seen=now, frames=0,
                           origin=_centre(d.bbox))
                self._tracks[d.track_id] = t
            t.last_seen = now
            t.frames += 1
            cx, cy = _centre(d.bbox)
            dist = ((cx - t.origin[0]) ** 2 + (cy - t.origin[1]) ** 2) ** 0.5
            t.moved = max(t.moved, dist)
            t.hist.append((now, cx, cy))
            while t.hist and now - t.hist[0][0] > p.move_window_s:
                t.hist.pop(0)
            _, hx, hy = t.hist[0]
            if ((cx - hx) ** 2 + (cy - hy) ** 2) ** 0.5 >= p.min_move:
                t.last_moved = now
            if d.confidence >= t.max_confidence:
                t.max_confidence = d.confidence
                t.best_bbox = d.bbox
            if t.event_id is None and t.frames >= p.min_frames and t.moved >= p.min_move:
                t.event_id = str(uuid.uuid4())
                events.append(
                    Event("start", t.event_id, t.track_id, t.label, d.confidence, t.max_confidence,
                          d.bbox, t.frames, t.first_seen, now, want_snapshot=True)
                )

        for tid in [tid for tid, t in self._tracks.items() if now - t.last_seen > p.lost_after_s]:
            t = self._tracks.pop(tid)
            if t.event_id:
                events.append(self._end(t))
        return events

    def moving(self, now: float) -> bool:
        """True if any track moved within the last move_window_s (keeps the Tier 0 gate open).

        A car that parks stops counting a couple of seconds later, so it cannot
        hold the Hailo forever.
        """
        return any(now - t.last_moved <= self.policy.move_window_s for t in self._tracks.values())

    def flush(self) -> list[Event]:
        """End every open event, e.g. on shutdown or stream loss."""
        events = [self._end(t) for t in self._tracks.values() if t.event_id]
        self._tracks.clear()
        return events

    @staticmethod
    def _end(t: _Track) -> Event:
        return Event("end", t.event_id or "", t.track_id, t.label, t.max_confidence, t.max_confidence,
                     t.best_bbox, t.frames, t.first_seen, t.last_seen)
