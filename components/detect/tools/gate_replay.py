"""Tune the Tier 0 gate offline against what Tier 1 (YOLO) saw.

Run cvkit-detect with ``[gate] mode = "shadow"`` and ``trace_dir`` set: Tier 1
then sees every frame (ground truth), and Tier 0 writes one CSV row per
encoded frame (t, bytes, keyframe, z, gate). Pull the moving-object events
(examples/collect or the hub) into a JSON list, then replay the traces through
MotionGate with different settings:

    python tools/gate_replay.py --traces traces/ --events events.json \\
        --grid floor_ratio=2,3,4 floor_min_bytes=150,300,600 motion_z_high=3,4,6

For each setting it reports, over all cameras:

- recall: share of YOLO moving-object events the gate would have let Tier 1 see
  (open for at least min_overlap_s of the object's time on screen);
- duty: share of camera-time the gate is open, i.e. Tier 1 cost relative to
  running YOLO on everything;
- opens/h and the median gate-open delay relative to YOLO's first sighting.

Tier 1 feedback is simulated: while the gate is open during a ground-truth
event, the tracker would report movement and hold the gate.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import statistics as st
from collections import defaultdict
from dataclasses import replace
from datetime import datetime
from pathlib import Path

from cvkit_detect.motion import GateConfig, MotionGate, load_library


def load_traces(d: Path) -> dict[str, list[tuple[float, int, bool]]]:
    out = {}
    for f in sorted(d.glob("*.csv")):
        with f.open() as fh:
            out[f.stem] = [(float(r[0]), int(r[1]), r[2] == "1") for r in csv.reader(fh) if len(r) >= 3]
    return out


def load_truth(path: Path) -> dict[str, list[tuple[float, float]]]:
    """Moving-object intervals per camera from start/end events."""
    data = json.loads(path.read_text())
    events = data["events"] if isinstance(data, dict) else data
    ts = lambda s: datetime.fromisoformat(s).timestamp()  # noqa: E731
    spans: dict[str, dict[str, list[float]]] = defaultdict(dict)
    for e in events:
        if e.get("schema", "cvkit.detect.v1") != "cvkit.detect.v1":
            continue
        a, b = ts(e["first_seen"]), ts(e["last_seen"])
        cur = spans[e["camera"]].setdefault(e["event_id"], [a, b])
        cur[0], cur[1] = min(cur[0], a), max(cur[1], b)
    return {cam: sorted((a, b) for a, b in ev.values()) for cam, ev in spans.items()}


def simulate(trace, truth, cfg: GateConfig, lib):
    g = MotionGate(cfg, lib)
    opens, intervals, start = [], [], None
    ti = 0
    for t, n, key in trace:
        while ti < len(truth) and truth[ti][1] < t:
            ti += 1
        if g.is_open and ti < len(truth) and truth[ti][0] <= t <= truth[ti][1]:
            g.tier1_moving(t)
        ev = g.feed(t, n, key)
        if ev == "open":
            start = t
            opens.append(t)
        elif ev == "close":
            intervals.append((start, t))
            start = None
    if start is not None:
        intervals.append((start, trace[-1][0]))
    return intervals, opens


def overlap(a0, a1, ivs):
    return sum(max(0.0, min(a1, b1) - max(a0, b0)) for b0, b1 in ivs)


def evaluate(traces, truth, cfg, lib, min_overlap_s=1.0):
    total_t = open_t = 0.0
    hits = misses = nopens = 0
    delays = []
    per_cam = {}
    for cam, tr in traces.items():
        if len(tr) < 2:
            continue
        gt = [iv for iv in truth.get(cam, []) if tr[0][0] <= iv[0] <= tr[-1][0]]
        ivs, opens = simulate(tr, gt, cfg, lib)
        dur = tr[-1][0] - tr[0][0]
        o = sum(b - a for a, b in ivs)
        total_t += dur
        open_t += o
        nopens += len(opens)
        h = 0
        for a, b in gt:
            need = min(min_overlap_s, max(b - a, 0.1))
            if overlap(a, b, ivs) >= need:
                h += 1
                first = min((s for s, e in ivs if e >= a), default=None)
                if first is not None:
                    delays.append(first - a)
            else:
                misses += 1
        hits += h
        per_cam[cam] = {"duty": o / dur if dur else 0, "events": len(gt), "hit": h, "opens": len(opens)}
    n = hits + misses
    return {
        "recall": hits / n if n else float("nan"),
        "events": n,
        "duty": open_t / total_t if total_t else 0,
        "opens_per_h": nopens / (total_t / 3600) if total_t else 0,
        "delay_p50": st.median(delays) if delays else float("nan"),
        "per_cam": per_cam,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--traces", type=Path, required=True)
    ap.add_argument("--events", type=Path, required=True)
    ap.add_argument("--lib", help="path to libemd_inspector.so")
    ap.add_argument("--grid", nargs="*", default=[], help="key=v1,v2 ... (GateConfig fields)")
    ap.add_argument("--min-overlap", type=float, default=1.0)
    ap.add_argument("--json", type=Path, help="write all results here")
    args = ap.parse_args()

    lib = load_library(args.lib)
    traces, truth = load_traces(args.traces), load_truth(args.events)
    keys, values = [], []
    for g in args.grid:
        k, v = g.split("=")
        keys.append(k)
        values.append([type(getattr(GateConfig(), k))(x) for x in v.split(",")])
    results = []
    for combo in itertools.product(*values) if keys else [()]:
        cfg = replace(GateConfig(), **dict(zip(keys, combo)))
        r = evaluate(traces, truth, cfg, lib, args.min_overlap)
        r["params"] = dict(zip(keys, combo))
        results.append(r)
    results.sort(key=lambda r: (-r["recall"], r["duty"]))
    print(f"{'params':60} recall  events  duty   opens/h  delay")
    for r in results[:25]:
        print(f"{json.dumps(r['params']):60} {r['recall']:.3f}  {r['events']:5d}  {r['duty']:.3f}  "
              f"{r['opens_per_h']:7.1f}  {r['delay_p50']:+.2f}s")
    if args.json:
        args.json.write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
