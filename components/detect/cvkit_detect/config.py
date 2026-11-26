"""TOML configuration. See config.example.toml for every key."""

from __future__ import annotations

import os
import socket
import tomllib
from dataclasses import dataclass, field, replace
from pathlib import Path

from .events import EventPolicy
from .motion import GateConfig

POSTPROCESS_DIR = "/usr/lib/aarch64-linux-gnu/hailo/tappas/post_processes"


@dataclass
class Camera:
    name: str
    url: str  # rtsp://... or a file path (looped, for testing)
    fps: int = 10
    loop: bool = True  # files only: rebuild and replay at end of file
    codec: str = "h264"  # h264 | h265: what the camera sends (Tier 0 parses it without decoding)
    gate: dict = field(default_factory=dict)  # per-camera overrides of [gate]

    def __post_init__(self):
        if self.codec not in ("h264", "h265"):
            raise ValueError(f"camera {self.name}: codec must be h264 or h265")

    def gate_config(self, base: GateConfig) -> GateConfig:
        return replace(base, **self.gate) if self.gate else base

    @property
    def is_file(self) -> bool:
        return not self.url.startswith(("rtsp://", "rtsps://"))


@dataclass
class Config:
    site: str
    cameras: list[Camera]
    edge: str = field(default_factory=socket.gethostname)
    nats_url: str = "nats://127.0.0.1:4222"
    stream: str = "EDGE_DETECT"
    stream_max_age_days: float = 7
    stream_max_bytes: int = 4 * 1024**3
    hef: str = "/usr/share/hailo-models/yolov8s_h8l.hef"
    postprocess_so: str = f"{POSTPROCESS_DIR}/libyolo_hailortpp_post.so"
    postprocess_fn: str = "filter"
    nms_score_threshold: float = 0.3
    snapshot_width: int = 960
    snapshot_quality: int = 80
    tier1_slots: int = 6      # decode+Hailo pipelines shared by all cameras (per codec)
    slot_max_s: float = 15.0  # a camera holding a slot this long yields it if another is waiting
    status_interval_s: float = 30
    policy: EventPolicy = field(default_factory=EventPolicy)
    gate: GateConfig = field(default_factory=GateConfig)

    @property
    def model(self) -> str:
        return Path(self.hef).stem


def load(path: str | os.PathLike) -> Config:
    raw = tomllib.loads(Path(path).read_text())
    cams = [Camera(**c) for c in raw.pop("camera", [])]
    if not cams:
        raise ValueError(f"{path}: at least one [[camera]] is required")
    if len({c.name for c in cams}) != len(cams):
        raise ValueError(f"{path}: camera names must be unique")
    pol = raw.pop("events", {})
    if "labels" in pol:
        pol["labels"] = frozenset(pol["labels"])
    nats = raw.pop("nats", {})
    model = raw.pop("model", {})
    if "slots" in model:
        model["tier1_slots"] = model.pop("slots")
    if "slot_max_s" in model:
        model["slot_max_s"] = model.pop("slot_max_s")
    snap = raw.pop("snapshot", {})
    gate = GateConfig(**raw.pop("gate", {}))
    cfg = Config(
        site=raw.pop("site"),
        cameras=cams,
        policy=EventPolicy(**pol),
        gate=gate,
        **{k: v for k, v in raw.items()},
        **{f"nats_{k}" if k == "url" else k: v for k, v in nats.items()},
        **model,
        **{f"snapshot_{k}": v for k, v in snap.items()},
    )
    for c in cams:
        c.gate_config(gate)  # validate overrides now, not when the camera starts
        if not c.name.replace("-", "").replace("_", "").isalnum():
            raise ValueError(f"camera name {c.name!r}: use letters, digits, - and _ (it becomes a NATS subject token)")
    return cfg
