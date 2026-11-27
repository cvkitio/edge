"""Open Images V7 validation images for the Tier 2 benchmark.

  python -m evals.datasets.openimages <raw dir> <pool dir> [--per-pool 250] [--seed 7]

Raw dir holds the official CSVs (validation-annotations-bbox.csv, oidv7-val-annotations-human-imagelabels.csv,
oidv7-class-descriptions.csv, validation-images-with-rotation.csv). Every validation image is CC BY 2.0;
each record keeps its author, title and Flickr page for attribution.

Two pools:
- scene: human-verified Snow, Fog, Mist, Rain, Darkness, Street light, Street, Alley, Parking lot, Parking,
  Driveway, Yard, Backyard, Warehouse or Security: outdoor and low-light views like a camera's.
- bait: boxes of sculptures, dolls, toys and posters, or a person box flagged IsDepiction: things a detector
  calls a person or a vehicle that are not one.
The boxes and the human-verified negative labels are what build.py uses to decide each detection's truth.
"""
from __future__ import annotations

import argparse
import csv
import random
import subprocess
from collections import defaultdict
from pathlib import Path

from evals.datasets.common import write_pool

FAMILY = {"Person": "person", "Man": "person", "Woman": "person", "Boy": "person", "Girl": "person",
          "Car": "vehicle", "Truck": "vehicle", "Bus": "vehicle", "Van": "vehicle", "Motorcycle": "vehicle",
          "Land vehicle": "vehicle", "Vehicle": "vehicle", "Taxi": "vehicle", "Ambulance": "vehicle",
          "Bicycle": "bicycle", "Dog": "animal", "Cat": "animal", "Horse": "animal", "Deer": "animal", "Fox": "animal",
          "Animal": "animal", "Cattle": "animal", "Sheep": "animal", "Bird": "animal", "Bear": "animal"}
BAIT = {"Sculpture", "Doll", "Toy", "Poster", "Mannequin", "Billboard", "Picture frame", "Television"}
SCENE = {"Snow", "Fog", "Mist", "Rain", "Darkness", "Street light", "Street", "Alley", "Parking lot", "Parking",
         "Driveway", "Yard", "Backyard", "Warehouse", "Security"}
# A family is verified absent when every one of these labels is a human-verified negative for the image.
NEG_KEYS = {"person": {"Person"}, "vehicle": {"Car", "Vehicle", "Land vehicle"}, "bicycle": {"Bicycle"},
            "animal": {"Animal"}}


def main(argv=None) -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw", type=Path)
    ap.add_argument("pool", type=Path)
    ap.add_argument("--per-pool", type=int, default=250)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args(argv)
    names = {r[0]: r[1] for r in csv.reader(open(a.raw / "oidv7-class-descriptions.csv"))}

    boxes = defaultdict(list)
    bait = defaultdict(list)
    for r in csv.DictReader(open(a.raw / "validation-annotations-bbox.csv")):
        n = names.get(r["LabelName"], "")
        box = [float(r["XMin"]), float(r["YMin"]), float(r["XMax"]), float(r["YMax"])]
        if n in FAMILY:
            boxes[r["ImageID"]].append({"family": FAMILY[n], "label": n, "box": box,
                                        "depiction": int(r["IsDepiction"]), "group": int(r["IsGroupOf"])})
        elif n in BAIT:
            bait[r["ImageID"]].append({"label": n, "box": box})

    pos, neg = defaultdict(set), defaultdict(set)
    for r in csv.DictReader(open(a.raw / "oidv7-val-annotations-human-imagelabels.csv")):
        n = names.get(r["LabelName"], "")
        (pos if r["Confidence"] == "1" else neg)[r["ImageID"]].add(n)

    meta = {r["ImageID"]: r for r in csv.DictReader(open(a.raw / "validation-images-with-rotation.csv"))}
    rnd = random.Random(a.seed)
    scene = sorted(i for i in meta if pos[i] & SCENE)
    baitimgs = sorted(i for i in meta if bait[i] or any(b["depiction"] for b in boxes[i]))
    pick = {"scene": rnd.sample(scene, min(a.per_pool, len(scene))),
            "bait": rnd.sample([i for i in baitimgs if i not in scene], min(a.per_pool, len(baitimgs)))}

    out = a.pool
    (out / "pool").mkdir(parents=True, exist_ok=True)
    records = []
    for kind, ids in pick.items():
        for i in ids:
            m = meta[i]
            if m.get("Rotation") not in ("", "0", "0.0", None):
                continue                       # rotated originals: boxes refer to the rotated image
            absent = [f for f, keys in NEG_KEYS.items() if keys <= neg[i]]
            records.append({"id": f"oi-{i}", "source": "openimages", "pool": kind, "file": f"pool/oi-{i}.jpg",
                            "gt": boxes[i], "bait": bait[i], "absent": absent,
                            "scene": sorted(pos[i] & SCENE), "licence": m["License"], "author": m["Author"],
                            "title": m["Title"], "url": m["OriginalLandingURL"]})
    todo = [r for r in records if not (out / r["file"]).exists()]
    if todo:
        args = "\n".join(f"s3://open-images-dataset/validation/{r['id'][3:]}.jpg" for r in todo)
        subprocess.run(f"xargs -P 16 -I{{}} aws s3 cp --no-sign-request --only-show-errors {{}} {out / 'pool'}/",
                       input=args, text=True, shell=True, check=False)
        for r in todo:
            src = out / "pool" / f"{r['id'][3:]}.jpg"
            if src.exists():
                src.rename(out / r["file"])
    records = [r for r in records if (out / r["file"]).exists()]
    write_pool(out, records)
    print(f"{len(records)} images ({sum(r['pool'] == 'scene' for r in records)} scene, "
          f"{sum(r['pool'] == 'bait' for r in records)} bait) -> {out}")


if __name__ == "__main__":
    main()
