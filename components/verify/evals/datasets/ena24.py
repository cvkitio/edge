"""ENA24-detection camera-trap images (LILA BC, CDLA-Permissive-1.0) for the Tier 2 benchmark.

  python -m evals.datasets.ena24 <parquet dir> <pool dir> [--n 300] [--seed 7]

Motion-triggered trail cameras in Eastern North America: 23 classes of animal plus vehicles, day and
infrared night frames, every animal and vehicle boxed. LILA removed all images that contained a person, so
any "person" the detector reports here is a false alarm, and an animal called a person is the failure mode
it tests. The parquet shards are the lila-bc-community/ena24-detection copy on Hugging Face.
"""
from __future__ import annotations

import argparse
import io
import json
import random
from pathlib import Path

import pyarrow.parquet as pq
from PIL import Image, ImageStat

from evals.datasets.common import write_pool

CREDIT = {"licence": "https://cdla.dev/permissive-1-0/",
          "author": "Yousif et al., ENA24-detection, via LILA BC",
          "title": "ENA24-detection camera trap image", "url": "https://lila.science/datasets/ena24detection"}


def is_ir(img: Image.Image) -> bool:
    """Infrared night frames are monochrome: almost no difference between the colour channels."""
    small = img.convert("RGB").resize((64, 48))
    r, g, b = (ImageStat.Stat(small.getchannel(c)).mean[0] for c in "RGB")
    return max(abs(r - g), abs(g - b), abs(r - b)) < 3


def main(argv=None) -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw", type=Path)
    ap.add_argument("pool", type=Path)
    ap.add_argument("--n", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args(argv)
    rows, names = [], None
    for f in sorted(a.raw.glob("*.parquet")):
        pf = pq.ParquetFile(f)
        if names is None:
            feats = json.loads(pf.schema_arrow.metadata[b"huggingface"])["info"]["features"]
            names = feats["objects"]["category"]["feature"]["names"]
        for rg in range(pf.num_row_groups):
            rows += pf.read_row_group(rg).to_pylist()
    rnd = random.Random(a.seed)
    vehicle = names.index("Vehicle")
    veh = [r for r in rows if vehicle in r["objects"]["category"]]
    other = [r for r in rows if vehicle not in r["objects"]["category"]]
    pick = veh[: a.n // 4] + rnd.sample(other, min(len(other), a.n - min(len(veh), a.n // 4)))
    (a.pool / "pool").mkdir(parents=True, exist_ok=True)
    records = []
    for r in pick:
        img = Image.open(io.BytesIO(r["image"]["bytes"])).convert("RGB")
        w, h = img.size
        gt = []
        for (x, y, bw, bh), c in zip(r["objects"]["bbox"], r["objects"]["category"]):
            gt.append({"family": "vehicle" if c == vehicle else "animal", "label": names[c],
                       "box": [x / w, y / h, (x + bw) / w, (y + bh) / h], "depiction": 0, "group": 0})
        if w > 1280:
            img = img.resize((1280, round(h * 1280 / w)), Image.BILINEAR)
        rid = f"ena24-{r['image_id']}"
        img.save(a.pool / "pool" / f"{rid}.jpg", quality=90)
        records.append({"id": rid, "source": "ena24", "file": f"pool/{rid}.jpg", "gt": gt, "bait": [],
                        "absent": ["person"], "scene": ["night (infrared)"] if is_ir(img) else ["day"],
                        "camera": "trail camera", **CREDIT})
    write_pool(a.pool, records)
    print(f"{len(records)} images ({sum('night (infrared)' in r['scene'] for r in records)} infrared night, "
          f"{len(veh[: a.n // 4])} with vehicles) -> {a.pool}")


if __name__ == "__main__":
    main()
