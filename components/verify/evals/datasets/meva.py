"""MEVA surveillance frames (Kitware/IARPA, CC BY 4.0) for the Tier 2 benchmark.

  python -m evals.datasets.meva <annotation repo> <pool dir> [--clips 24] [--frames 6] [--seed 7]

MEVA is ground-camera video from 29 fixed and PTZ cameras at the Muscatatuck Urban Training Center,
indoor and outdoor, with consented actors. The Kitware annotations (gitlab.kitware.com/meva/meva-data-repo,
DIVA-phase-2/MEVA/kitware) box every actor and vehicle taking part in an annotated activity; people and cars
just passing by are not always boxed, so build.py only takes real cases from MEVA (a detection matching an
annotated box), never false ones.

One annotated clip per camera, then ``--frames`` frames per clip spread over the clip, each with at least
one annotated person or vehicle. Videos come from the public bucket s3://mevadata-public-01/drops-123-r13/.
"""
from __future__ import annotations

import argparse
import json
import random
import re
import shutil
import subprocess
import tempfile
from collections import defaultdict
from pathlib import Path

from evals.datasets.common import write_pool

CREDIT = {"licence": "https://creativecommons.org/licenses/by/4.0/",
          "author": "Kitware / IARPA DIVA, MEVA dataset (mevadata.org)",
          "title": "MEVA surveillance video frame", "url": "https://mevadata.org/"}
GEOM = re.compile(r"'g0': '(\d+) (\d+) (\d+) (\d+)'.*?'id1': (\d+).*?'ts0': (\d+)")
TYPE = re.compile(r"'cset3': \{'(\w+)'[^}]*\}, 'id1': (\d+)")


def clip_tracks(geom: Path, types: Path) -> tuple[dict[int, str], dict[int, list]]:
    kinds = {int(i): k for k, i in TYPE.findall(types.read_text())}
    frames: dict[int, list] = defaultdict(list)
    for x0, y0, x1, y1, tid, ts in GEOM.findall(geom.read_text()):
        k = kinds.get(int(tid))
        if k in ("person", "vehicle"):
            frames[int(ts)].append((k, [int(x0), int(y0), int(x1), int(y1)]))
    return kinds, frames


def main(argv=None) -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", type=Path)
    ap.add_argument("pool", type=Path)
    ap.add_argument("--clips", type=int, default=24)
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args(argv)
    rnd = random.Random(a.seed)
    base = a.repo / "annotation/DIVA-phase-2/MEVA/kitware"
    by_cam = defaultdict(list)
    for t in sorted(base.rglob("*.types.yml")):
        cam = re.search(r"\.(G\d+)\b", t.name).group(1)
        kinds = {int(i): k for k, i in TYPE.findall(t.read_text())}
        n_veh = sum(k == "vehicle" for k in kinds.values())
        n_per = sum(k == "person" for k in kinds.values())
        if n_veh or n_per:
            by_cam[cam].append(((n_veh > 0, min(n_per, 6), n_veh), t))
    chosen = []
    for cam in sorted(by_cam):
        opts = sorted(by_cam[cam], key=lambda x: x[0], reverse=True)[:4]
        _, t = rnd.choice(opts)
        g = t.with_name(t.name.replace(".types.yml", ".geom.yml"))
        if not g.exists():   # the repo is a blob:none clone; fetch only the geometry we use
            subprocess.run(["git", "-C", str(a.repo), "checkout", "-q", "HEAD", "--",
                            str(g.relative_to(a.repo))], check=False)
        if g.exists():
            _, frames = clip_tracks(g, t)
            if frames:
                chosen.append((cam, g, frames))
    chosen = chosen[: a.clips]
    (a.pool / "pool").mkdir(parents=True, exist_ok=True)
    records = []
    for cam, g, frames in chosen:
        clip = g.name.replace(".geom.yml", "")
        day, hour = clip[:10], clip[11:13]
        key = f"drops-123-r13/{day}/{hour}/{clip}.r13.avi"
        ts = sorted(frames)
        veh = [f for f in ts if any(k == "vehicle" for k, _ in frames[f])]
        step = max(1, len(ts) // a.frames)
        pick = sorted(set((veh[:: max(1, len(veh) // 2)][:2] if veh else []) + ts[::step]))[: a.frames]
        with tempfile.TemporaryDirectory(dir=a.pool) as tmp:
            vid = Path(tmp) / "v.avi"
            r = subprocess.run(["aws", "s3", "cp", "--no-sign-request", "--only-show-errors",
                                f"s3://mevadata-public-01/{key}", str(vid)], capture_output=True, text=True)
            if r.returncode or not vid.exists():
                print(f"skip {clip}: {r.stderr.strip()[:120]}")
                continue
            w, h = map(int, subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                            "stream=width,height", "-of", "csv=p=0", str(vid)],
                                           capture_output=True, text=True).stdout.strip().split(","))
            sel = "+".join(f"eq(n\\,{f})" for f in pick)
            subprocess.run(["ffmpeg", "-v", "error", "-i", str(vid), "-vf", f"select='{sel}'", "-vsync", "0",
                            "-q:v", "3", str(Path(tmp) / "f%03d.jpg")], check=True)
            outs = sorted(Path(tmp).glob("f*.jpg"))
            for f, src in zip(pick, outs):
                rid = f"meva-{cam}-{clip[:19].replace('.', '-')}-{f}"
                shutil.move(src, a.pool / "pool" / f"{rid}.jpg")
                gt = [{"family": k, "label": k, "box": [b[0] / w, b[1] / h, b[2] / w, b[3] / h], "depiction": 0,
                       "group": 0} for k, b in frames[f]]
                records.append({"id": rid, "source": "meva", "file": f"pool/{rid}.jpg", "gt": gt, "bait": [],
                                "absent": [], "scene": [clip.split(".")[3]],
                                "camera": f"MEVA {cam}", "clip": clip, "frame": f, **CREDIT})
        print(f"{cam} {clip}: {len(pick)} frames")
    write_pool(a.pool, records)
    print(f"{len(records)} frames from {len(chosen)} cameras -> {a.pool}")
    (a.pool / "clips.json").write_text(json.dumps([c[1].name for c in chosen], indent=1))


if __name__ == "__main__":
    main()
