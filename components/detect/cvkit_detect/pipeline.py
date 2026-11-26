"""Per camera: Tier 0 watches the compressed stream; Tier 1 decodes and runs YOLO only on motion.

    Tier 0 (always on, no decode)                     Tier 1 (only while the gate is open)
    rtspsrc ! depay ! parse ! appsink  ──AUs──▶ gate ──▶ appsrc ! parse ! decode ! letterbox 640
         │ per AU: bytes + keyframe flag          │           ! hailonet ! hailofilter ! hailotracker
         └──▶ emd inspector (motion.py) ──────────┘           ! appsink ──▶ EventTracker
                                                                   │ moving tracks keep the gate open
                                                                   ▼
                                                              events + snapshots

Tier 0 costs a few microseconds per frame, so it runs on every camera all the
time. Tier 1 is a small shared pool of decode+Hailo pipelines ("slots", sized
to what the Hailo can process, not to the number of cameras): when a camera's
gate opens it borrows a slot, the encoded frames since its last keyframe are
replayed so the decoder can start straight away (ask the camera for a short
keyframe interval, e.g. Axis videokeyframeinterval=10 at 10 fps), and live
frames follow until the gate closes and the slot goes back to the pool. If
every slot is busy the camera queues; a camera that has held a slot for
slot_max_s yields it to a waiting one. A closed gate costs no decode and no
Hailo time, and a camera costs only its Tier 0 ingest.

Gate modes: "on" gates Tier 1; "shadow" computes and reports the gate but feeds
Tier 1 everything (for tuning against what YOLO sees); "off" ignores it.

Frames are letterboxed into the network's 640x640 input (videoscale keeps the
aspect ratio, which is what YOLO was trained on); geometry.unletterbox maps
boxes back onto the original frame and snapshots crop the padding off. Every
hailonet shares vdevice-group-id=1, so all cameras time-share the one Hailo
through HailoRT's scheduler.

Threads: each camera's Tier 0 appsink callback runs on its ingest streaming
thread and pushes into its slot's appsrc; each slot's appsink callback runs on
that slot's streaming thread and hands detections to the owning camera. A
slot drops frames timestamped before the current owner's first frame, so
frames still in flight from the previous owner are never misattributed. The
EventTracker is touched from both sides, so each camera has a lock; the pool
has its own. Results go to asyncio with call_soon_threadsafe.
"""

from __future__ import annotations

import asyncio
import logging
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

import cv2
import gi
import hailo
import numpy as np

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

from .config import Camera, Config  # noqa: E402
from .events import Detection, Event, EventTracker  # noqa: E402
from .geometry import NET_H, NET_W, letterbox_geometry, unletterbox  # noqa: E402
from .motion import MotionGate  # noqa: E402

log = logging.getLogger(__name__)

# Pipelines whose set_state(NULL) never returned. Their threads still hold the
# Hailo, so the process must exit without joining them (see __main__.main).
abandoned = 0


@dataclass
class Result:
    camera: str
    events: list[Event] = field(default_factory=list)
    snapshots: dict[str, bytes] = field(default_factory=dict)  # event_id -> JPEG
    motion: list[dict] = field(default_factory=list)  # Tier 0 gate open/close


@dataclass
class CameraStats:
    frames: int = 0          # Tier 0: encoded frames seen
    tier1_frames: int = 0    # frames through the Hailo
    detections: int = 0
    events: int = 0
    restarts: int = 0
    last_frame: float = 0.0
    source_size: tuple[int, int] | None = None
    state: str = "starting"
    last_error: str = ""
    bytes_in: int = 0


def _caps(codec: str) -> str:
    return f"video/x-{codec},stream-format=byte-stream,alignment=au"


def ingest_description(cam: Camera) -> str:
    parse = f"{cam.codec}parse config-interval=-1 ! {_caps(cam.codec)}"
    if cam.is_file:
        demux = "matroskademux" if cam.url.endswith((".mkv", ".webm")) else "qtdemux"
        # sync=true paces the file to real time; nothing downstream can stall it.
        return (f'filesrc location="{cam.url}" ! {demux} ! {parse} ! '
                "appsink name=ingest emit-signals=true sync=true max-buffers=30 drop=false")
    return (f'rtspsrc location="{cam.url}" protocols=tcp latency=200 ! rtp{cam.codec}depay ! {parse} ! '
            "appsink name=ingest emit-signals=true sync=false max-buffers=30 drop=false")


def tier1_description(cfg: Config, codec: str, fps: int) -> str:
    return (
        f'appsrc name=src is-live=true format=time do-timestamp=true caps="{_caps(codec)}" '
        "max-bytes=8000000 block=false ! "
        f"{codec}parse ! decodebin ! videoconvert name=conv ! "
        # A file plays at its own rate; drop down to the configured fps.
        f"videorate drop-only=true max-rate={fps} ! "
        f"videoscale ! video/x-raw,format=RGB,width={NET_W},height={NET_H},pixel-aspect-ratio=1/1 ! "
        "queue leaky=downstream max-size-buffers=3 ! "
        f'hailonet hef-path="{cfg.hef}" batch-size=1 vdevice-group-id=1 force-writable=true '
        f"nms-score-threshold={cfg.nms_score_threshold} ! "
        "queue leaky=no max-size-buffers=3 ! "
        f'hailofilter so-path="{cfg.postprocess_so}" function-name={cfg.postprocess_fn} qos=false ! '
        "hailotracker class-id=-1 keep-tracked-frames=3 keep-new-frames=3 keep-lost-frames=10 ! "
        "appsink name=sink emit-signals=true max-buffers=2 drop=true sync=false"
    )


async def _stop_pipeline(p: Gst.Pipeline | None, what: str) -> None:
    # set_state(NULL) can block for a long time after a HailoRT error, so
    # keep it off the event loop and give up waiting after a few seconds.
    if p is None:
        return
    try:
        await asyncio.wait_for(asyncio.to_thread(p.set_state, Gst.State.NULL), timeout=5)
    except asyncio.TimeoutError:
        global abandoned
        abandoned += 1
        log.error("%s: pipeline did not stop within 5s; abandoning it", what)


class Tier1Slot:
    """One decode + Hailo pipeline, lent to one camera at a time."""

    def __init__(self, cfg: Config, index: int, codec: str, fps: int):
        self.cfg, self.index, self.codec, self.fps = cfg, index, codec, fps
        self.name = f"tier1-{codec}-{index}"
        self.owner: CameraPipeline | None = None
        self.since = 0.0          # wall time the current owner got the slot
        self.session_rt = 0       # running time of the owner's first frame; older frames are dropped
        self.pipeline: Gst.Pipeline | None = None
        self.appsrc = None
        self.frames = 0

    def build(self) -> None:
        self.pipeline = Gst.parse_launch(tier1_description(self.cfg, self.codec, self.fps))
        self.pipeline.get_by_name("sink").connect("new-sample", self._on_sample)
        self.appsrc = self.pipeline.get_by_name("src")
        self.pipeline.set_state(Gst.State.PLAYING)

    def running_time(self) -> int:
        clock = self.pipeline.get_clock() if self.pipeline else None
        return clock.get_time() - self.pipeline.get_base_time() if clock else 0

    def push(self, au: bytes, key: bool) -> bool:
        b = Gst.Buffer.new_wrapped(au)
        if not key:
            b.set_flags(Gst.BufferFlags.DELTA_UNIT)
        return self.appsrc.emit("push-buffer", b) == Gst.FlowReturn.OK

    def _on_sample(self, sink) -> Gst.FlowReturn:
        sample = sink.emit("pull-sample")
        owner = self.owner
        if sample is None or owner is None:
            return Gst.FlowReturn.OK
        buf = sample.get_buffer()
        if buf.pts != Gst.CLOCK_TIME_NONE and buf.pts < self.session_rt:
            return Gst.FlowReturn.OK  # still in flight from the previous owner
        self.frames += 1
        caps = self.pipeline.get_by_name("conv").get_static_pad("src").get_current_caps()
        size = None
        if caps:
            st = caps.get_structure(0)
            size = (st.get_value("width"), st.get_value("height"))
        owner.on_tier1_frame(buf, size)
        return Gst.FlowReturn.OK


class Tier1Pool:
    """Shares a few Tier 1 slots among many cameras, per codec."""

    def __init__(self, cfg: Config, cameras: list[Camera]):
        self.cfg = cfg
        self.lock = threading.Lock()
        codecs = sorted({c.codec for c in cameras})
        fps = max((c.fps for c in cameras), default=10)
        # Shadow/off cameras want Tier 1 all the time, so give them a slot each.
        always = sum(1 for c in cameras if c.gate_config(cfg.gate).mode != "on")
        n = max(cfg.tier1_slots, always)
        self.slots = {codec: [Tier1Slot(cfg, i, codec, fps) for i in range(n)] for codec in codecs}
        self.waiting: list[CameraPipeline] = []
        self.denied = 0  # times a camera had to wait for a slot

    def all_slots(self) -> list[Tier1Slot]:
        return [s for v in self.slots.values() for s in v]

    def acquire(self, cam: "CameraPipeline") -> Tier1Slot | None:
        with self.lock:
            for slot in self.slots[cam.cam.codec]:
                if slot.owner is None and slot.pipeline is not None:
                    self._give(slot, cam)
                    if cam in self.waiting:
                        self.waiting.remove(cam)
                    return slot
            if cam not in self.waiting:
                self.waiting.append(cam)
                self.denied += 1
            return None

    def release(self, slot: Tier1Slot, cam: "CameraPipeline") -> None:
        with self.lock:
            if slot.owner is cam:
                slot.owner = None
            if cam in self.waiting:
                self.waiting.remove(cam)

    def leave_queue(self, cam: "CameraPipeline") -> None:
        with self.lock:
            if cam in self.waiting:
                self.waiting.remove(cam)

    def should_yield(self, slot: Tier1Slot, now: float) -> bool:
        """Someone is waiting for this codec and the owner has had its turn."""
        if now - slot.since < self.cfg.slot_max_s:
            return False
        with self.lock:
            return any(c.cam.codec == slot.codec for c in self.waiting)

    @staticmethod
    def _give(slot: Tier1Slot, cam: "CameraPipeline") -> None:
        slot.owner = cam
        slot.since = time.time()
        slot.session_rt = slot.running_time()

    async def run(self, stopping: Callable[[], bool]) -> None:
        """Build the slots and rebuild any that error. Owners are released on error."""
        for slot in self.all_slots():
            slot.build()
        want = Gst.MessageType.ERROR | Gst.MessageType.WARNING
        while not stopping():
            for slot in self.all_slots():
                msg = slot.pipeline.get_bus().timed_pop_filtered(0, want) if slot.pipeline else None
                if msg is None:
                    continue
                err, dbg = msg.parse_error() if msg.type == Gst.MessageType.ERROR else msg.parse_warning()
                if msg.type == Gst.MessageType.WARNING:
                    log.warning("%s: %s", slot.name, err.message)
                    continue
                log.error("%s: %s (%s); rebuilding", slot.name, err.message, dbg)
                owner = slot.owner
                with self.lock:
                    slot.owner = None
                p, slot.pipeline = slot.pipeline, None
                if owner:
                    owner.slot_lost(slot, err.message)
                await _stop_pipeline(p, slot.name)
                await asyncio.sleep(1)
                slot.build()
            await asyncio.sleep(0.1)

    async def stop(self) -> None:
        for slot in self.all_slots():
            p, slot.pipeline = slot.pipeline, None
            await _stop_pipeline(p, slot.name)


class CameraPipeline:
    """Tier 0 ingest for one camera, borrowing a Tier 1 slot while its gate is open."""

    def __init__(self, cfg: Config, cam: Camera, loop: asyncio.AbstractEventLoop,
                 on_result: Callable[[Result], None], pool: Tier1Pool):
        self.cfg, self.cam, self.loop, self.on_result, self.pool = cfg, cam, loop, on_result, pool
        self.gcfg = cam.gate_config(cfg.gate)
        self.tracker = EventTracker(cfg.policy)
        self.lock = threading.Lock()
        self.stats = CameraStats()
        self.gate = MotionGate(self.gcfg)
        self.ingest: Gst.Pipeline | None = None
        self.slot: Tier1Slot | None = None
        self._gop: list[tuple[bytes, bool]] = []  # encoded frames since the last keyframe
        self._need_key = True   # next frame into the slot must start at a keyframe
        self._stopping = False
        self.waits = 0          # frames the gate was open but no slot was free
        self._trace = None
        if self.gcfg.trace_dir:
            os.makedirs(self.gcfg.trace_dir, exist_ok=True)
            self._trace = open(os.path.join(self.gcfg.trace_dir, f"{cam.name}.csv"), "a", buffering=1 << 16)

    # -- lifecycle ---------------------------------------------------------
    def _build(self) -> None:
        self.ingest = Gst.parse_launch(ingest_description(self.cam))
        self.ingest.get_by_name("ingest").connect("new-sample", self._on_au)
        self._gop, self._need_key = [], True
        self.gate = MotionGate(self.gcfg)  # re-learn the baseline after a reconnect

    def _drop_slot(self) -> None:
        slot, self.slot = self.slot, None
        if slot:
            self.pool.release(slot, self)
        self.pool.leave_queue(self)

    async def _teardown(self) -> None:
        ingest, self.ingest = self.ingest, None
        await _stop_pipeline(ingest, self.cam.name)
        self._drop_slot()
        if self.gate.is_open:
            self.gate._close(time.time())
            self._motion("close", time.time())

    async def run(self) -> None:
        """Run until stop(); rebuild with backoff on error (camera reboot, network blip)."""
        backoff = 1.0
        while not self._stopping:
            self._build()
            self.ingest.set_state(Gst.State.PLAYING)
            self.stats.state = "playing"
            ok = await self._watch_bus()
            await self._teardown()
            with self.lock:
                self._emit(self.tracker.flush(), None)
            if self._stopping:
                break
            self.stats.restarts += 1
            self.stats.state = "reconnecting"
            backoff = 1.0 if ok else min(backoff * 2, 30.0)
            log.warning("%s: restarting in %.0fs", self.cam.name, backoff)
            await asyncio.sleep(backoff)
        self.stats.state = "stopped"
        if self._trace:
            self._trace.close()

    def stop(self) -> None:
        self._stopping = True

    async def _watch_bus(self) -> bool:
        """Poll the ingest bus from asyncio (no GLib main loop needed). Returns True on clean EOS."""
        bus = self.ingest.get_bus()
        want = Gst.MessageType.ERROR | Gst.MessageType.EOS | Gst.MessageType.WARNING
        last_frames, stall_since = -1, time.monotonic()
        while not self._stopping:
            msg = bus.timed_pop_filtered(0, want)
            if msg is None:
                # Watchdog: an RTSP source can go silent without erroring.
                if self.stats.frames != last_frames:
                    last_frames, stall_since = self.stats.frames, time.monotonic()
                elif time.monotonic() - stall_since > 20:
                    self.stats.last_error = "no frames for 20s"
                    log.error("%s: %s", self.cam.name, self.stats.last_error)
                    return False
                await asyncio.sleep(0.1)
                continue
            if msg.type == Gst.MessageType.EOS:
                # A file loops by rebuilding the ingest pipeline.
                if self.cam.is_file and not self.cam.loop:
                    self._stopping = True
                return True
            err, dbg = msg.parse_error() if msg.type == Gst.MessageType.ERROR else msg.parse_warning()
            if msg.type == Gst.MessageType.WARNING:
                log.warning("%s: %s", self.cam.name, err.message)
                continue
            self.stats.last_error = err.message
            log.error("%s: %s (%s)", self.cam.name, err.message, dbg)
            return False
        return True

    def slot_lost(self, slot: Tier1Slot, why: str) -> None:
        """Called by the pool when this camera's slot pipeline failed."""
        if self.slot is slot:
            self.slot = None
            self.stats.last_error = f"{slot.name}: {why}"

    # -- Tier 0: ingest streaming thread -----------------------------------
    def _on_au(self, sink) -> Gst.FlowReturn:
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buf = sample.get_buffer()
        now = time.time()
        key = not buf.has_flags(Gst.BufferFlags.DELTA_UNIT)
        n = buf.get_size()
        self.stats.frames += 1
        self.stats.bytes_in += n
        self.stats.last_frame = now

        transition = self.gate.feed(now, n, key)
        if transition:
            self._motion(transition, now)
        if self._trace:
            self._trace.write(f"{now:.3f},{n},{int(key)},{self.gate.last_z:.2f},{int(self.gate.is_open)}\n")

        ok, info = buf.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        try:
            au = bytes(info.data)
        finally:
            buf.unmap(info)
        if key:
            self._gop = [(au, True)]
        elif self._gop:
            self._gop.append((au, False))
            if len(self._gop) > 300:  # camera with a very long GOP: don't grow without bound
                self._gop = []

        want = self.gate.is_open or self.gcfg.mode != "on"
        if want and self.slot is not None and self.pool.should_yield(self.slot, now):
            self._drop_slot()  # had a long turn and others are queued: go to the back
        if want and self.slot is None:
            self.slot = self.pool.acquire(self)
            if self.slot is None:
                self.waits += 1
            else:
                self._need_key = True
                for frame, k in self._gop[:-1]:  # replay from the last keyframe
                    self._push(frame, k)
        if want and self.slot is not None:
            self._push(au, key)
        elif not want and self.slot is not None:
            self._drop_slot()
        elif not want:
            self.pool.leave_queue(self)
        if self.slot is None:
            # Nothing reaches Tier 1, so age tracks out from here (ends open events).
            with self.lock:
                self._emit(self.tracker.update(now, []), None)
        return Gst.FlowReturn.OK

    def _push(self, au: bytes, key: bool) -> None:
        slot = self.slot
        if slot is None:
            return
        if self._need_key:
            if not key:
                return
            self._need_key = False
        if not slot.push(au, key):
            self._need_key = True

    def _motion(self, kind: str, now: float) -> None:
        g = self.gate
        doc = {"kind": kind, "ts": now, "z": round(g.last_z, 2), "floor": round(g.floor, 1),
               "open_s": round(now - g.opened_at, 2) if kind == "close" else 0.0, "mode": self.gcfg.mode,
               "reason": g.last_reason if kind == "open" else ""}
        log.debug("%s motion %s %s", self.cam.name, kind, doc["reason"])
        self.loop.call_soon_threadsafe(self.on_result, Result(self.cam.name, motion=[doc]))

    # -- Tier 1: called on a slot's streaming thread -----------------------
    def on_tier1_frame(self, buf: Gst.Buffer, size: tuple[int, int] | None) -> None:
        now = time.time()
        self.stats.tier1_frames += 1
        if size:
            self.stats.source_size = size
        dets = []
        for d in hailo.get_roi_from_buffer(buf).get_objects_typed(hailo.HAILO_DETECTION):
            ids = d.get_objects_typed(hailo.HAILO_UNIQUE_ID)
            if not ids:
                continue
            b = d.get_bbox()
            box = unletterbox((b.xmin(), b.ymin(), b.xmin() + b.width(), b.ymin() + b.height()), size)
            dets.append(Detection(ids[0].get_id(), d.get_label(), d.get_confidence(), box))
        self.stats.detections += len(dets)

        with self.lock:
            events = self.tracker.update(now, dets)
            if self.tracker.moving(now):
                self.gate.tier1_moving(now)
            if events:
                frame = None
                if any(e.want_snapshot for e in events):
                    ok, info = buf.map(Gst.MapFlags.READ)
                    if ok:
                        try:
                            frame = np.ndarray((NET_H, NET_W, 3), np.uint8, info.data).copy()
                        finally:
                            buf.unmap(info)
                self._emit(events, frame)

    def _emit(self, events: list[Event], frame: np.ndarray | None) -> None:
        if not events:
            return
        self.stats.events += len(events)
        snaps = {}
        if frame is not None:
            for e in events:
                if e.want_snapshot:
                    snaps[e.event_id] = self._snapshot(frame, e)
        self.loop.call_soon_threadsafe(self.on_result, Result(self.cam.name, events, snaps))

    def _snapshot(self, frame: np.ndarray, e: Event) -> bytes:
        sw, sh = self.stats.source_size or (NET_W, NET_H)
        px, py, cw, ch = letterbox_geometry(self.stats.source_size)
        frame = frame[round(py):round(py + ch), round(px):round(px + cw)]  # drop the padding
        w = self.cfg.snapshot_width
        h = max(1, round(w * sh / sw))
        img = cv2.cvtColor(cv2.resize(frame, (w, h), interpolation=cv2.INTER_AREA), cv2.COLOR_RGB2BGR)
        x0, y0, x1, y1 = e.bbox
        p0, p1 = (int(x0 * w), int(y0 * h)), (int(x1 * w), int(y1 * h))
        cv2.rectangle(img, p0, p1, (0, 200, 255), 2)
        label = f"{e.label} {e.confidence:.2f}"
        (tw, th), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
        tx = min(max(0, p0[0]), w - tw - 2)  # keep the label inside the frame
        ty = p0[1] - 6 if p0[1] - 6 >= th + 2 else min(h - 4, p1[1] + th + 6)
        cv2.putText(img, label, (tx, ty), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 200, 255), 2)
        ok, jpg = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, self.cfg.snapshot_quality])
        return jpg.tobytes() if ok else b""
