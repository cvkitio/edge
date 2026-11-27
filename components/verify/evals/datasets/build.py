"""Turn dataset pools plus real Tier 1 detections into benchmark cases.

  python -m evals.datasets.build seq   <seq dir> <pool dir>...            letterbox every pool image, in order
  python -m evals.datasets.build cases <cases dir> <dets.json> <seq dir> [--take source=N ...] [--seed 7]

`seq` writes <seq dir>/00000.jpg ... and index.json (which pool image each frame is). Run pi_detect.py on
the edge Pi over that folder, then `cases` matches every detection to the pool's ground truth:

  real   the box overlaps (IoU >= 0.5) a real object of the claimed family, or sits inside a group-of box
  false  the dataset says no such object is there: a human-verified negative label (Open Images), a
         depiction or bait object under the box (statue, doll, poster), or an exhaustively labelled frame
         with nothing of that family near the box (ENA24)
  drop   anything else, e.g. a person in a MEVA frame whose annotations only cover the actors of an activity

At most one case per image. `--take` sets how many cases each source contributes, half real and half false
where it can. Cases are appended to <cases dir>/cases.json (existing ids are kept) with the snapshot drawn
the way Tier 1 draws it, and with source, licence and attribution for the public method page.
"""
from __future__ import annotations

import argparse
import json
import random
from pathlib import Path

from PIL import Image

from evals.datasets.common import COCO_FAMILY, inside, iou, letterbox, snapshot, unletterbox


def seq(out: Path, pools: list[Path]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    index = []
    for pool in pools:
        for r in json.loads((pool / "pool.json").read_text())["records"]:
            img = Image.open(pool / r["file"])
            letterbox(img).save(out / f"{len(index):05d}.jpg", quality=92)
            index.append({"pool": str(pool), "id": r["id"], "size": list(img.size)})
    (out / "index.json").write_text(json.dumps(index))
    print(f"{len(index)} frames -> {out}")


def judge(rec: dict, fam: str, box: list[float]) -> tuple[str, list[str], int | None]:
    """('real'|'false'|'drop', accepted labels, people in frame or None)."""
    gt = rec.get("gt", [])
    real_gt = [g for g in gt if not g.get("depiction")]
    people = None
    persons = [g for g in real_gt if g["family"] == "person"]
    if rec["source"] == "ena24":
        people = 0                                     # LILA removed every image with a human in it
    elif rec["source"] == "openimages":
        if any(g.get("group") for g in persons):
            people = None
        elif persons:
            people = len(persons)
        elif "person" in rec.get("absent", []):
            people = 0
    under = sorted({g["family"] for g in real_gt if iou(box, g["box"]) >= 0.5} - {fam})
    same = [g for g in real_gt if g["family"] == fam]
    if any(iou(box, g["box"]) >= 0.5 or (g.get("group") and inside(box, g["box"])) for g in same):
        labels = [fam] + [u for u in under if {fam, u} == {"person", "bicycle"}]
        return "real", labels, people
    near = any(iou(box, g["box"]) >= 0.1 or inside(box, g["box"], 0.5) for g in same)
    if near:
        return "drop", [], people
    bait = [g for g in gt if g.get("depiction") and iou(box, g["box"]) >= 0.5] + \
           [b for b in rec.get("bait", []) if iou(box, b["box"]) >= 0.5]
    exhaustive = rec["source"] == "ena24" or fam in rec.get("absent", [])
    if bait or exhaustive:
        return "false", (under or ["none"]), people
    return "drop", [], people


def cases(out: Path, dets: Path, seqdir: Path, take: dict[str, int], seed: int) -> None:
    index = json.loads((seqdir / "index.json").read_text())
    frames = json.loads(dets.read_text())["frames"]
    if len(frames) < len(index):   # the Hailo pipeline can drop the last frame at end of stream
        print(f"warning: {len(index) - len(frames)} frame(s) missing at the end; using the first {len(frames)}")
        index = index[: len(frames)]
    assert len(frames) == len(index), (len(frames), len(index))
    pools: dict[str, dict] = {}
    cands: dict[str, dict[str, list]] = {}
    for meta, fd in zip(index, frames):
        pool = pools.setdefault(meta["pool"], {r["id"]: r for r in
                                               json.loads((Path(meta["pool"]) / "pool.json").read_text())["records"]})
        rec = pool[meta["id"]]
        w, h = meta["size"]
        per_image = {"real": [], "false": []}
        for d in fd:
            fam = COCO_FAMILY.get(d["label"])
            if not fam:
                continue
            box = unletterbox(d["box"], w, h)
            if (box[2] - box[0]) * (box[3] - box[1]) < 0.0004:
                continue                               # under ~0.02 x 0.02 of the frame: Tier 1 ignores these too
            kind, labels, people = judge(rec, fam, box)
            if kind != "drop":
                per_image[kind].append((d, box, labels, people))
        for kind, lst in per_image.items():
            if lst:   # one case per image and kind: the highest-scoring detection
                d, box, labels, people = max(lst, key=lambda x: x[0]["confidence"])
                cands.setdefault(rec["source"], {"real": [], "false": []})[kind].append((meta, rec, d, box, labels, people))

    rnd = random.Random(seed)
    cj = out / "cases.json"
    doc = json.loads(cj.read_text()) if cj.exists() else {"cases": []}
    have = {c["id"] for c in doc["cases"]}
    if (out / "review.json").exists():   # never re-add a case the visual review excluded
        have |= set(json.loads((out / "review.json").read_text()).get("exclude", {}))
    (out / "images").mkdir(parents=True, exist_ok=True)
    added = 0
    for source, n in take.items():
        pool = cands.get(source, {"real": [], "false": []})
        used: set[str] = set()
        nf = min(len(pool["false"]), n // 2)
        picks = [("false", x) for x in rnd.sample(pool["false"], nf)]
        used |= {x[1]["id"] for _, x in picks}
        realpool = [x for x in pool["real"] if x[1]["id"] not in used]
        picks += [("real", x) for x in rnd.sample(realpool, min(len(realpool), n - nf))]
        print(f"{source}: {len(pool['real'])} real and {len(pool['false'])} false candidates; took "
              f"{sum(k == 'real' for k, _ in picks)} real, {sum(k == 'false' for k, _ in picks)} false")
        for kind, (meta, rec, d, box, labels, people) in picks:
            cid = f"{rec['id']}-{kind}"
            if cid in have:
                continue
            img = Image.open(Path(meta["pool"]) / rec["file"])
            (out / "images" / f"{cid}.jpg").write_bytes(snapshot(img, box, d["label"], d["confidence"]))
            doc["cases"].append({
                "id": cid, "image": f"images/{cid}.jpg", "camera": rec.get("camera", source), "kind": "tier1",
                "source": source, "scene": rec.get("scene", []),
                "claim": {"label": d["label"], "confidence": d["confidence"], "bbox": box},
                "truth": {"real": kind == "real", "labels": labels, "people": people},
                "credit": {k: rec[k] for k in ("licence", "author", "title", "url") if rec.get(k)}})
            have.add(cid)
            added += 1
    cj.write_text(json.dumps(doc, indent=1))
    print(f"added {added} cases -> {cj} ({len(doc['cases'])} in total)")


def review(out: Path) -> None:
    """Apply <cases dir>/review.json (written after looking at every dataset case): drop the excluded cases and
    set the corrected truth on relabelled ones. Each entry keeps its reason; the method page reports both."""
    rv = json.loads((out / "review.json").read_text())
    cj = out / "cases.json"
    doc = json.loads(cj.read_text())
    kept = []
    for c in doc["cases"]:
        if c["id"] in rv.get("exclude", {}):
            continue
        fix = rv.get("relabel", {}).get(c["id"])
        if fix:
            c["truth"] = {**c["truth"], "real": fix["real"], "labels": fix["labels"]}
            c["review"] = fix["reason"]
        kept.append(c)
    print(f"review: {len(doc['cases']) - len(kept)} excluded, {len(rv.get('relabel', {}))} relabelled, {len(kept)} kept")
    doc["cases"] = kept
    doc["review"] = {"reviewed": rv.get("reviewed"), "excluded": len(rv.get("exclude", {})),
                     "relabelled": len(rv.get("relabel", {}))}
    cj.write_text(json.dumps(doc, indent=1))


def main(argv=None) -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("seq")
    s.add_argument("out", type=Path)
    s.add_argument("pools", type=Path, nargs="+")
    c = sub.add_parser("cases")
    c.add_argument("out", type=Path)
    c.add_argument("dets", type=Path)
    c.add_argument("seq", type=Path)
    c.add_argument("--take", nargs="*", default=[], help="source=N")
    c.add_argument("--seed", type=int, default=7)
    r = sub.add_parser("review")
    r.add_argument("out", type=Path)
    a = ap.parse_args(argv)
    if a.cmd == "review":
        review(a.out)
    elif a.cmd == "seq":
        seq(a.out, a.pools)
    else:
        cases(a.out, a.dets, a.seq, {k: int(v) for k, v in (t.split("=") for t in a.take)}, a.seed)


if __name__ == "__main__":
    main()
