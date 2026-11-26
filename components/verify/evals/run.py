"""Benchmark vision models on Tier 2 verification and rank them on accuracy per dollar
(same shape as Readiday's and helmwatch's evals/run.py).

  OPENROUTER_API_KEY=... python -m evals.run --cases /srv/data/cvkit/verify/cases [--models a,b]
      [--repeats 1] [--max-usd 3] [--workers 8] [--limit 20] [--estimate] [--out evals/results]

Each case is a Tier 1 start event: the snapshot with the detector's box drawn on it and the claim (label,
score, box), sent exactly as the service sends it (``vision.verify``). Truth per case: is the claim real,
which labels describe the box, how many people are in the frame. Scored:

  verdict  answer.real == truth.real          (what decides whether anyone is alerted)
  label    answer.label in truth.labels
  people   answer.people == truth.people
  quality = 10 * (0.7 * verdict + 0.2 * label + 0.1 * people); 0 when no valid JSON came back.

Recall (real detections confirmed) and false-alarm rejection (synthetic false positives rejected) are
reported separately, because missing a real intruder costs more than an extra alert. Cost per call is what
OpenRouter charged (usage.cost); seconds are wall clock.

The best-value model is the cheapest one whose quality is within 0.3 of the best, whose recall is at least
0.95, and whose median latency is under --max-p50-s (alerts need to be quick).

Cases live outside the repo (they are real site images): <cases>/cases.json + <cases>/images/.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime as dt
import json
import os
import statistics
import subprocess
import sys
import threading
from pathlib import Path

import requests

from cvkit_verify import vision

HERE = Path(__file__).parent


def load_models() -> dict:
    return json.loads((HERE / "models.json").read_text())


def api_key() -> str:
    """OPENROUTER_API_KEY, else read the 1Password ref in CVKIT_OPENROUTER_REF at the point of use."""
    k = os.environ.get("OPENROUTER_API_KEY", "")
    if not k and os.environ.get("CVKIT_OPENROUTER_REF"):
        k = subprocess.run(["op", "read", os.environ["CVKIT_OPENROUTER_REF"]], capture_output=True,
                           text=True, check=True).stdout.strip()
    if not k:
        sys.exit("set OPENROUTER_API_KEY or CVKIT_OPENROUTER_REF")
    return k


def load_cases(d: Path, limit: int | None = None) -> list[dict]:
    cases = json.loads((d / "cases.json").read_text())["cases"]
    for c in cases:
        c["jpeg"] = (d / c["image"]).read_bytes()
    if limit:  # keep the real/false mix when trimming
        real = [c for c in cases if c["truth"]["real"]]
        fake = [c for c in cases if not c["truth"]["real"]]
        k = max(1, round(limit * len(real) / len(cases)))
        cases = real[:: max(1, len(real) // k)][:k] + fake[: limit - k]
    return cases


def score(case: dict, v: vision.Verdict) -> dict:
    t = case["truth"]
    if not v.ok:
        return {"verdict": 0, "label": 0, "people": 0, "quality": 0.0}
    s = {"verdict": int(v.real == t["real"]), "label": int(v.label in t["labels"]),
         "people": int(v.people == t["people"])}
    s["quality"] = round(10 * (0.7 * s["verdict"] + 0.2 * s["label"] + 0.1 * s["people"]), 2)
    return s


def pricing() -> dict[str, tuple[float, float]]:
    try:
        data = requests.get("https://openrouter.ai/api/v1/models", timeout=30).json()["data"]
        return {m["id"]: (float(m["pricing"]["prompt"]), float(m["pricing"]["completion"])) for m in data}
    except Exception:  # noqa: BLE001
        return {}


def estimate(models: list[str], n_calls: int) -> dict[str, float]:
    """Rough USD per model: ~2.2k prompt tokens (two images) and ~250 completion tokens a call."""
    p = pricing()
    return {m: round(n_calls * (2200 * p.get(m, (0, 0))[0] + 250 * p.get(m, (0, 0))[1]), 4) for m in models}


def run(key: str, models: list[str], cases: list[dict], repeats: int = 1, max_usd: float = 3.0,
        workers: int = 8, log=print) -> list[dict]:
    spent = [0.0]
    lock = threading.Lock()
    results: list[dict] = []
    session = requests.Session()

    def one(model: str, case: dict, rep: int):
        with lock:
            if spent[0] >= max_usd:
                return None
        claim = vision.Claim(case["claim"]["label"], case["claim"]["confidence"], case["claim"]["bbox"],
                             case.get("camera", ""))
        v = vision.verify(key, model, case["jpeg"], claim, session=session)
        with lock:
            spent[0] += v.cost
        # The model's answer fields are kept as answer_* so the scores (label, people) don't overwrite them.
        ans = {(f"answer_{k}" if k in ("label", "people") else k): x for k, x in v.to_dict().items()}
        return {"model": model, "case": case["id"], "kind": case.get("kind", ""), "repeat": rep,
                "truth": case["truth"], **ans, **score(case, v)}

    jobs = [(m, c, r) for r in range(repeats) for c in cases for m in models]
    with cf.ThreadPoolExecutor(workers) as ex:
        for i, res in enumerate(ex.map(lambda j: one(*j), jobs), 1):
            if res:
                results.append(res)
            if i % 50 == 0 or i == len(jobs):
                log(f"{i}/{len(jobs)} calls, ${spent[0]:.3f}")
    if spent[0] >= max_usd:
        log(f"stopped at the ${max_usd} cap; later calls were skipped")
    return results


def summary(results: list[dict], models: list[str]) -> list[dict]:
    rows = []
    for m in models:
        rs = [r for r in results if r["model"] == m]
        if not rs:
            continue
        real = [r for r in rs if r["truth"]["real"]]
        fake = [r for r in rs if not r["truth"]["real"]]
        secs = [r["seconds"] for r in rs if r["ok"]]
        rows.append({
            "model": m, "calls": len(rs),
            "quality": round(statistics.mean(r["quality"] for r in rs), 2),
            "accuracy": round(statistics.mean(r["verdict"] for r in rs), 3),
            "recall": round(statistics.mean(r["verdict"] for r in real), 3) if real else None,
            "rejection": round(statistics.mean(r["verdict"] for r in fake), 3) if fake else None,
            "label": round(statistics.mean(r["label"] for r in rs), 3),
            "people": round(statistics.mean(r["people"] for r in rs), 3),
            "valid": round(sum(r["ok"] for r in rs) / len(rs), 3),
            "cost": round(statistics.mean(r["cost"] for r in rs), 6),       # USD per verification
            "p50_s": round(statistics.median(secs), 2) if secs else None,
            "p90_s": round(sorted(secs)[int(0.9 * (len(secs) - 1))], 2) if secs else None,
            "tokens_out": round(statistics.mean(r["tokens_out"] for r in rs)),
        })
    rows.sort(key=lambda r: (-r["quality"], r["cost"]))
    return rows


def pick(rows: list[dict], tolerance: float = 0.3, min_recall: float = 0.95, max_p50_s: float = 8.0) -> dict:
    """best = top quality; value = cheapest within ``tolerance`` of it that is quick and misses little."""
    if not rows:
        return {}
    best = rows[0]
    ok = [r for r in rows if r["quality"] >= best["quality"] - tolerance and (r["recall"] or 0) >= min_recall
          and r["p50_s"] is not None and r["p50_s"] <= max_p50_s and r["valid"] >= 0.98]
    value = min(ok, key=lambda r: r["cost"]) if ok else best
    fast = min(ok, key=lambda r: r["p50_s"]) if ok else best
    return {"best": best["model"], "value": value["model"], "fast": fast["model"],
            "rule": {"tolerance": tolerance, "min_recall": min_recall, "max_p50_s": max_p50_s}}


def markdown(rec: dict) -> str:
    rows, p = rec["summary"], rec["pick"]
    out = [f"# Tier 2 vision benchmark {rec['stamp']}", "",
           f"{rec['n_cases']} cases ({rec['n_real']} real, {rec['n_cases'] - rec['n_real']} false) x "
           f"{rec['repeats']} repeat(s); total ${rec['spent']:.3f}.", "",
           f"Best: **{p.get('best')}**. Best value: **{p.get('value')}**. Fastest good: **{p.get('fast')}**.", "",
           "| model | quality | accuracy | recall | false rejected | label | people | valid | $/1k checks | p50 s |",
           "|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        out.append(f"| {r['model']} | {r['quality']} | {r['accuracy']:.0%} | {r['recall']:.0%} | "
                   f"{r['rejection']:.0%} | {r['label']:.0%} | {r['people']:.0%} | {r['valid']:.0%} | "
                   f"{r['cost'] * 1000:.2f} | {r['p50_s']} |")
    misses = [r for r in rec["results"] if r["ok"] and not r["verdict"]]
    if misses:
        out += ["", "## Wrong verdicts", "", "| model | case | truth | answer | description |", "|---|---|---|---|---|"]
        for r in sorted(misses, key=lambda r: (r["model"], r["case"])):
            out.append(f"| {r['model']} | {r['case']} | {'real' if r['truth']['real'] else 'false'} | "
                       f"{r.get('answer_label', '?')} | {r['description'][:90]} |")
    return "\n".join(out) + "\n"


def record(results, models, cases, repeats) -> dict:
    rows = summary(results, models)
    return {"stamp": dt.datetime.now().strftime("%Y-%m-%d-%H%M"), "models": models, "repeats": repeats,
            "n_cases": len(cases), "n_real": sum(c["truth"]["real"] for c in cases),
            "spent": round(sum(r["cost"] for r in results), 4), "summary": rows, "pick": pick(rows),
            "results": results}


def main(argv=None):
    cfg = load_models()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cases", type=Path, default=Path(os.environ.get("CVKIT_CASES", "/srv/data/cvkit/verify/cases")))
    ap.add_argument("--models", help="comma separated (default: models.json candidates)")
    ap.add_argument("--repeats", type=int, default=cfg.get("repeats", 1))
    ap.add_argument("--max-usd", type=float, default=cfg.get("max_usd", 3.0))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--limit", type=int, help="use a subset of N cases (keeps the real/false mix)")
    ap.add_argument("--estimate", action="store_true", help="print the expected cost and stop")
    ap.add_argument("--out", type=Path, default=HERE / "results")
    a = ap.parse_args(argv)
    models = a.models.split(",") if a.models else cfg["candidates"]
    cases = load_cases(a.cases, a.limit)
    est = estimate(models, len(cases) * a.repeats)
    if a.estimate:
        for m, usd in sorted(est.items(), key=lambda x: x[1]):
            print(f"{m:50} ${usd:.3f}")
        print(f"{'total':50} ${sum(est.values()):.2f} for {len(cases)} cases x {a.repeats}")
        return None
    results = run(api_key(), models, cases, a.repeats, a.max_usd, a.workers)
    rec = record(results, models, cases, a.repeats)
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / f"{rec['stamp']}.json").write_text(json.dumps(rec, indent=1))
    (a.out / f"{rec['stamp']}.md").write_text(markdown(rec))
    print(markdown(rec).split("## Wrong")[0])
    return rec


if __name__ == "__main__":
    main()
