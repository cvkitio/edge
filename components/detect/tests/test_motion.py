"""Tier 0 gate, using the real emd inspector compiled from components/emd."""

import random
import shutil
import subprocess
from pathlib import Path

import pytest

from cvkit_detect.motion import GateConfig, Inspector, MotionGate, load_library

HERE = Path(__file__).resolve().parents[1]
# In the repo: components/emd. On an edge box: the copy deploy/sync.sh puts in native/.
EMD = next((d for d in (HERE.parent / "emd", HERE / "native") if (d / "include").exists()), HERE.parent / "emd")


@pytest.fixture(scope="session")
def lib(tmp_path_factory):
    src = next((p for p in (EMD / "src" / "emd_inspector.c", EMD / "emd_inspector.c") if p.exists()),
               EMD / "src" / "emd_inspector.c")
    compiler = shutil.which("gcc") or shutil.which("clang")
    if not src.exists() or not compiler:
        pytest.skip("needs components/emd and a C compiler")
    so = tmp_path_factory.mktemp("lib") / "libemd_inspector.so"
    subprocess.run([compiler, "-O2", "-shared", "-fPIC", "-I", str(EMD / "include"), str(src), "-lm", "-o", str(so)],
                   check=True)
    return load_library(str(so))


def stream(gate, t0, seconds, size, fps=10, gop=10, jitter=10, rng=random.Random(1)):
    """Feed a synthetic Axis-like stream: big keyframe every gop, tiny P-frames."""
    out = []
    for i in range(int(seconds * fps)):
        key = i % gop == 0
        n = 20000 if key else max(20, int(size + rng.uniform(-jitter, jitter)))
        out.append((t0 + i / fps, gate.feed(t0 + i / fps, n, key)))
    return [(t, ev) for t, ev in out if ev]


def test_struct_layout_matches_c(lib):
    insp = Inspector(GateConfig(), lib)
    for i in range(50):
        insp.process(100, i % 10 == 0)
    assert 90 < insp.baseline < 110  # bpf_slow seeded from P-frames only, read back through ctypes


def test_static_scene_stays_closed(lib):
    g = MotionGate(GateConfig(), lib)
    assert stream(g, 0, 120, size=90) == []
    assert not g.is_open


def test_motion_opens_and_closes_after_hold(lib):
    g = MotionGate(GateConfig(hold_s=4.0), lib)
    stream(g, 0, 60, size=90)  # learn the baseline
    evs = stream(g, 60, 5, size=3000)  # someone walks through: P-frames jump to ~3 KB
    assert evs and evs[0][1] == "open" and evs[0][0] < 60.3  # within a few frames
    evs = stream(g, 65, 15, size=90)
    assert [e for _, e in evs] == ["close"]
    assert 65 + 4 <= evs[0][0] <= 65 + 4 + 2.5  # hold_s after the inspector goes quiet
    assert g.opens == 1 and 5 < g.total_open(80) < 12


def test_tier1_feedback_holds_gate_open(lib):
    g = MotionGate(GateConfig(hold_s=2.0), lib)
    stream(g, 0, 60, size=90)
    stream(g, 60, 1, size=3000)
    for i in range(100):  # bytes back to normal, but YOLO still sees a moving track
        t = 61 + i / 10
        g.tier1_moving(t)
        assert g.feed(t, 90, i % 10 == 0) is None
    assert g.is_open


def test_permanent_scene_change_closes_after_max_open(lib):
    g = MotionGate(GateConfig(hold_s=2.0, max_open_s=30), lib)
    stream(g, 0, 60, size=90)
    evs = stream(g, 60, 60, size=3000)  # lights on: frames stay big for good
    assert [e for _, e in evs][:2] == ["open", "close"] and evs[1][0] <= 60 + 31


def test_busy_scene_still_opens_on_floor(lib):
    # The sample clip: P-frames ~800 B between people, 5-15 KB while someone walks.
    g = MotionGate(GateConfig(hold_s=2.0), lib)
    rng = random.Random(2)
    evs = []
    t = 0.0
    for cycle in range(6):
        for size, secs in ((800, 4), (rng.uniform(5000, 15000), 6)):
            evs += stream(g, t, secs, size=size, jitter=size * 0.3, rng=rng)
            t += secs
    kinds = [e for _, e in evs]
    assert kinds.count("open") >= 5 and kinds.count("open") - kinds.count("close") in (0, 1)
