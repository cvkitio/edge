"""Tier 0: motion gating on the compressed bitstream, before anything is decoded.

This reuses cvkit's edge-motion-detector inspector (components/emd/src/
emd_inspector.c) unchanged, built as a small shared library and called through
ctypes. It looks only at the size and type of each encoded access unit: a
static scene from a modern encoder is a stream of tiny P-frames (tens to a few
hundred bytes on an Axis sub-stream), and anything moving makes them jump. The
cost is a few microseconds per frame, against tens of milliseconds to decode
and run a frame through YOLO, so every camera can be watched all the time
while the Hailo only sees cameras where something is happening.

MotionGate turns the inspector's per-frame verdicts into an open/closed gate
for Tier 1 (decode + Hailo):

- opens when the inspector raises MOTION (a burst against its running mean),
  or when on_threshold P-frames in a row sit well above the scene's quiet
  floor;
- stays open while either keeps signalling, and while Tier 1 is still
  tracking something that moves;
- closes hold_s after the last of those, or after max_open_s regardless (a
  permanent scene change such as lights coming on must not hold the Hailo).
"""

from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass
from pathlib import Path

LIB_ENV = "CVKIT_EMD_INSPECTOR"
LIB_PATHS = ("/opt/cvkit/lib/libemd_inspector.so",)


class _State(ctypes.Structure):
    _fields_ = [("bpf_ewma", ctypes.c_double), ("bpf_slow", ctypes.c_double),
                ("bpf_vslow", ctypes.c_double), ("bpf_var", ctypes.c_double),
                ("since_kf", ctypes.c_uint32), ("consecutive_above", ctypes.c_uint8),
                ("consecutive_below", ctypes.c_uint8), ("fsm", ctypes.c_int),
                ("gradual_above_frames", ctypes.c_uint32)]


class _Cfg(ctypes.Structure):
    _fields_ = [("motion_z_high", ctypes.c_double), ("intra_ratio_high", ctypes.c_double),
                ("on_threshold", ctypes.c_uint8), ("off_threshold", ctypes.c_uint8),
                ("bpf_floor", ctypes.c_double), ("min_bytes_threshold", ctypes.c_uint32),
                ("configured_periodic_kf", ctypes.c_bool), ("gradual_enabled", ctypes.c_bool),
                ("gradual_threshold", ctypes.c_double), ("gradual_window_frames", ctypes.c_uint32)]


class _Input(ctypes.Structure):
    _fields_ = [("pts_90khz", ctypes.c_uint64), ("mono_ns", ctypes.c_uint64),
                ("byte_count", ctypes.c_size_t), ("is_keyframe", ctypes.c_bool),
                ("is_intra", ctypes.c_bool), ("intra_ratio_proxy", ctypes.c_double),
                ("mb_skip_run", ctypes.c_uint32), ("slice_qp_delta", ctypes.c_int32)]


class _Result(ctypes.Structure):
    _fields_ = [("event", ctypes.c_int), ("z_score", ctypes.c_double),
                ("intra_ratio", ctypes.c_double), ("reason", ctypes.c_char * 128),
                ("state_changed", ctypes.c_bool), ("fsm_before", ctypes.c_uint8),
                ("fsm_after", ctypes.c_uint8)]


EVENT_MOTION = 1
FSM_IDLE, FSM_ACTIVE, FSM_COOLDOWN = 0, 1, 2

_lib = None


def load_library(path: str | None = None) -> ctypes.CDLL:
    global _lib
    if _lib is not None and path is None:
        return _lib
    candidates = [path] if path else [os.environ.get(LIB_ENV), *LIB_PATHS,
                                      str(Path(__file__).with_name("libemd_inspector.so"))]
    for c in candidates:
        if c and Path(c).exists():
            lib = ctypes.CDLL(c)
            lib.emd_inspector_process.restype = ctypes.c_bool
            lib.emd_inspector_process.argtypes = [ctypes.POINTER(_State), ctypes.POINTER(_Cfg),
                                                  ctypes.POINTER(_Input), ctypes.POINTER(_Result)]
            if path is None:
                _lib = lib
            return lib
    raise FileNotFoundError(f"libemd_inspector.so not found (set {LIB_ENV}); tried {candidates}")


@dataclass
class GateConfig:
    mode: str = "on"               # on: gate Tier 1 | shadow: compute gate, run Tier 1 always | off
    motion_z_high: float = 4.0     # z-score of AU bytes vs slow baseline that counts as motion
    intra_ratio_high: float = 1e9  # bytes / baseline; disabled by default (noisy on tiny baselines)
    on_threshold: int = 2          # signalling frames in a row to open
    off_threshold: int = 15        # quiet frames in a row before the inspector leaves ACTIVE
    bpf_floor: float = 100.0       # minimum std-dev in bytes, so a near-empty stream isn't hair-trigger
    min_bytes: int = 0             # ignore AUs smaller than this
    hold_s: float = 3.0            # keep Tier 1 running this long after the last motion
    floor_ratio: float = 2.0       # P-frames above ratio x quiet floor count as motion...
    floor_min_bytes: int = 100     # ...if also above floor + this many bytes (0 disables the floor rule)
    floor_rise: float = 0.002      # per-frame rise rate of the floor (fall rate is 0.3)
    max_open_s: float = 120.0      # close (and re-learn the baseline) after this long open
    unexpected_idr: bool = False   # treat back-to-back IDRs as motion (off: Axis inserts them)
    trace_dir: str = ""            # if set, append per-frame t,bytes,key,z,gate CSV per camera

    def __post_init__(self):
        if self.mode not in ("on", "shadow", "off"):
            raise ValueError(f"gate.mode must be on, shadow or off, not {self.mode!r}")


class Inspector:
    """One camera's emd inspector state."""

    def __init__(self, cfg: GateConfig, lib: ctypes.CDLL | None = None):
        self.lib = lib or load_library()
        self.state = _State()
        self.cfg = _Cfg()
        self.lib.emd_inspector_default_cfg(ctypes.byref(self.cfg))
        self.cfg.motion_z_high = cfg.motion_z_high
        self.cfg.intra_ratio_high = cfg.intra_ratio_high
        self.cfg.on_threshold = cfg.on_threshold
        self.cfg.off_threshold = min(cfg.off_threshold, 255)
        self.cfg.bpf_floor = cfg.bpf_floor
        self.cfg.min_bytes_threshold = cfg.min_bytes
        self.cfg.configured_periodic_kf = not cfg.unexpected_idr
        self.lib.emd_inspector_init(ctypes.byref(self.state), ctypes.byref(self.cfg))
        self._in = _Input()
        self._res = _Result()

    def process(self, nbytes: int, keyframe: bool) -> tuple[bool, bool, float]:
        """Returns (motion_event_raised, frame_signals_motion, z)."""
        self._in.byte_count = nbytes
        self._in.is_keyframe = keyframe
        self._in.is_intra = keyframe
        slow = self.state.bpf_slow
        self._in.intra_ratio_proxy = nbytes / slow if slow > 0 and not keyframe else 0.0
        raised = self.lib.emd_inspector_process(ctypes.byref(self.state), ctypes.byref(self.cfg),
                                                ctypes.byref(self._in), ctypes.byref(self._res))
        signal = self._res.reason not in (b"none", b"")
        return bool(raised and self._res.event == EVENT_MOTION), signal, self._res.z_score

    def reset(self) -> None:
        self.lib.emd_inspector_reset(ctypes.byref(self.state))

    @property
    def active(self) -> bool:
        return self.state.fsm == FSM_ACTIVE

    @property
    def baseline(self) -> float:
        return self.state.bpf_slow


class MotionGate:
    """Open/closed decision for Tier 1, driven by the inspector and Tier 1 feedback.

    Why two Tier 0 signals: the inspector's baseline is a mean and its variance
    soaks up whatever it sees, which is right for spotting a sudden burst but
    means a scene that is busy most of the time (or one long walk-through)
    stops looking like motion within a second or two. The quiet floor tracks
    the *smallest* frames the scene produces when nothing happens (falls fast,
    rises slowly, frozen while open), so sustained motion keeps standing out.
    """

    def __init__(self, cfg: GateConfig, lib: ctypes.CDLL | None = None):
        self.cfg = cfg
        self.insp = Inspector(cfg, lib)
        self.is_open = False
        self.opened_at = 0.0
        self.last_motion = -1e18
        self.last_tier1 = -1e18
        self.opens = 0
        self.open_time = 0.0  # cumulative seconds open (closed intervals only)
        self.last_z = 0.0
        self.last_reason = ""
        self.floor = 0.0  # quiet-scene P-frame size
        self._above = 0   # consecutive P-frames above the floor threshold

    def feed(self, now: float, nbytes: int, keyframe: bool) -> str | None:
        """Process one access unit. Returns "open", "close" or None."""
        c = self.cfg
        raised, signal, z = self.insp.process(nbytes, keyframe)
        self.last_z = z
        above = False
        if not keyframe:
            if self.floor <= 0:
                self.floor = float(nbytes)
            above = (c.floor_min_bytes > 0
                     and nbytes > max(c.floor_ratio * self.floor, self.floor + c.floor_min_bytes))
            if not self.is_open:  # frozen while open, so long motion can't raise it
                if nbytes < self.floor:
                    self.floor += 0.3 * (nbytes - self.floor)
                else:
                    self.floor += c.floor_rise * (nbytes - self.floor)
            self._above = self._above + 1 if above else 0
        if signal or raised or above:
            self.last_motion = now
        if not self.is_open:
            floor_open = self._above >= c.on_threshold
            if raised or floor_open:
                self.is_open, self.opened_at = True, now
                self.opens += 1
                self.last_reason = f"z={z:.1f}" if raised else f"floor x{nbytes / max(self.floor, 1):.1f}"
                return "open"
            return None
        if now - self.opened_at > c.max_open_s:
            self._close(now)
            self.insp.reset()
            self.floor = 0.0  # re-learn: the scene may have changed for good
            return "close"
        if self.insp.active:
            return None
        if now - max(self.last_motion, self.last_tier1) > c.hold_s:
            self._close(now)
            return "close"
        return None

    def _close(self, now: float) -> None:
        self.is_open = False
        self.open_time += now - self.opened_at

    def tier1_moving(self, now: float) -> None:
        """Tier 1 is tracking something that moves: keep the gate open."""
        self.last_tier1 = now

    def total_open(self, now: float) -> float:
        return self.open_time + (now - self.opened_at if self.is_open else 0.0)
