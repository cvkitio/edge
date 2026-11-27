"""The monthly Tier 2 vision benchmark: run the eval, keep the record, publish the page, switch the verify
service to the best-value model (nas01 CronJob cvkit/vision-benchmark, 06:30 Hobart on the 1st).

  cvkit-vision-bench                          run, publish, set the model
  cvkit-vision-bench --publish FILE.json      re-publish an existing record without running anything
  cvkit-vision-bench --dry-run                run and publish, but leave the service's model alone

Environment:
  OPENROUTER_API_KEY   the eval key (its own key, so a run can never use up the service's budget)
  CVKIT_CASES          case dir (default /srv/data/cvkit/verify/cases)
  CVKIT_RESULTS        records (default /srv/data/cvkit/verify/results)
  CVKIT_PAGE_DIR       page output (default /srv/data/share/cvkitio/vision-benchmark, the tailnet share)
  CVKIT_PUBLIC_DIR     the public report for https://cvkit.net/resources/reports/vision-benchmark/ (evals/report.py:
                       results, method, every case image; cvkitio/infra site/publish.sh uploads it). Unset: none.
  NATS_URL             for the KV write (default nats://nats.cvkit:4222)
  TELEGRAM_*/ALERT_WEBHOOK_URL/NTFY_URL   optional: tell the admin when the pick changes or a run fails

Two stages, to fit the eval key's $10 a month: every candidate screens one pass of a fixed, stratified
sample (models.json "screen"), then up to "final.models" models that are close to the best, miss little and
cost under "final.max_cost_per_1k" run "final.passes" passes over every case. The pick is made from the
final stage. The budget is the smaller of "max_usd" and 90% of the key's remaining credit; stage 1 drops the
priciest models to fit its share, stage 2 cuts passes, then models.
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import logging
import os
import random
import sys
from pathlib import Path

import requests

from cvkit_verify.alerts import Alerter
from evals import publish, report, run

log = logging.getLogger("evals.monthly")


def remaining_credit(key: str) -> float | None:
    try:
        d = requests.get("https://openrouter.ai/api/v1/key", headers={"Authorization": f"Bearer {key}"},
                         timeout=20).json()["data"]
        return None if d.get("limit_remaining") is None else float(d["limit_remaining"])
    except Exception:  # noqa: BLE001
        return None


def tell_admin(title: str, text: str) -> None:
    sinks = Alerter([]).push(title, text, None, {"schema": "cvkit.benchmark.v1"})
    log.info("admin notice (%s): %s: %s", ",".join(sinks) or "log only", title, text)


async def set_model(model: str) -> str | None:
    import nats
    nc = await nats.connect(os.environ.get("NATS_URL", "nats://nats.cvkit:4222"), name="cvkit-vision-bench")
    js = nc.jetstream()
    try:
        kv = await js.key_value("cvkit_verify")
    except Exception:  # noqa: BLE001
        kv = await js.create_key_value(bucket="cvkit_verify", history=10)
    try:
        old = (await kv.get("model")).value.decode()
    except Exception:  # noqa: BLE001
        old = None
    if old != model:
        await kv.put("model", model.encode())
    await nc.drain()
    return old


def screening_sample(cases: list[dict], n: int, seed: int = 7) -> list[dict]:
    """A fixed sample, stratified by source and by real/false, so month-to-month screens are comparable."""
    if n >= len(cases):
        return list(cases)
    groups: dict[tuple, list] = {}
    for c in cases:
        groups.setdefault((c.get("source") or "site", c["truth"]["real"]), []).append(c)
    rnd = random.Random(seed)
    out = []
    for k in sorted(groups):
        g = sorted(groups[k], key=lambda c: c["id"])
        out += rnd.sample(g, max(1, round(n * len(g) / len(cases))))
    return sorted(out, key=lambda c: c["id"])[:n]


def two_stage(key: str, models: list[str], cases: list[dict], cfg: dict, budget: float) -> dict | None:
    """Stage 1: every model, one pass, a screening sample. Stage 2: the cheap models close to the best, several
    passes over every case. The pick comes from stage 2; stage 1 is kept in the record as ``screen``."""
    sc, fc = cfg.get("screen", {}), cfg.get("final", {})
    sample = screening_sample(cases, sc.get("cases", 100))
    est = run.estimate(models, len(sample))
    stage1_budget = budget * sc.get("budget_share", 0.5)
    while models and sum(est[m] for m in models) > stage1_budget:
        drop = max(models, key=lambda m: est[m])
        log.warning("screen: dropping %s ($%.3f) to fit $%.2f", drop, est[drop], stage1_budget)
        models.remove(drop)
    if not models:
        return None
    log.info("screen: %d models x %d cases (est $%.2f, budget $%.2f)", len(models), len(sample),
             sum(est[m] for m in models), budget)
    res1 = run.run(key, models, sample, 1, stage1_budget, workers=16, log=log.info)
    rows1 = run.summary(res1, models)
    spent1 = sum(r["cost"] for r in res1)
    out = Path(os.environ.get("CVKIT_RESULTS", "/srv/data/cvkit/verify/results"))
    out.mkdir(parents=True, exist_ok=True)
    (out / f"screen-{dt.datetime.now():%Y-%m-%d-%H%M}.json").write_text(
        json.dumps({"summary": rows1, "results": res1, "spent": spent1}, indent=1))   # kept if stage 2 fails
    best = rows1[0]["quality"] if rows1 else 0
    ok = [r for r in rows1 if r["quality"] >= best - fc.get("within", 0.5) and (r["recall"] or 0) >= fc.get("min_recall", 0.9)
          and r["valid"] >= 0.95 and r["cost"] * 1000 <= fc.get("max_cost_per_1k", 1.0)]
    finalists = [r["model"] for r in sorted(ok, key=lambda r: r["cost"])[: fc.get("models", 8)]]
    if not finalists:
        finalists = [min(rows1, key=lambda r: r["cost"])["model"]] if rows1 else []
    left = budget - spent1
    passes = fc.get("passes", 3)
    per_pass = sum(next(r["cost"] for r in rows1 if r["model"] == m) for m in finalists) * len(cases)
    while passes > 1 and per_pass * passes > left:
        passes -= 1
    while len(finalists) > 1 and per_pass * passes > left:
        finalists.pop()                       # the most expensive of the cheap ones
        per_pass = sum(next(r["cost"] for r in rows1 if r["model"] == m) for m in finalists) * len(cases)
    log.info("final: %d models x %d cases x %d passes (est $%.2f, left $%.2f): %s", len(finalists), len(cases),
             passes, per_pass * passes, left, ", ".join(finalists))
    res2 = run.run(key, finalists, cases, passes, left, workers=16, log=log.info)
    rec = run.record(res2, finalists, cases, passes)
    rec["screen"] = {"n_cases": len(sample), "case_ids": [c["id"] for c in sample], "summary": rows1,
                     "spent": round(spent1, 4), "results": res1,
                     "rule": {k: fc.get(k, d) for k, d in (("within", 0.5), ("min_recall", 0.9),
                                                            ("max_cost_per_1k", 1.0), ("models", 8))}}
    rec["spent"] = round(rec["spent"] + spent1, 4)
    return rec


def main(argv=None) -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--publish", type=Path, help="publish this record and stop")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--final-only", help="comma-separated models: skip the screen and run only the final stage "
                                         "(when the month's credit can't cover a screen)")
    ap.add_argument("--passes", type=int, help="final-only: passes to run (the spend cap still applies)")
    a = ap.parse_args(argv)
    page_dir = Path(os.environ.get("CVKIT_PAGE_DIR", "/srv/data/share/cvkitio/vision-benchmark"))
    public = os.environ.get("CVKIT_PUBLIC_DIR")
    cases_dir = Path(os.environ.get("CVKIT_CASES", "/srv/data/cvkit/verify/cases"))
    review = json.loads((cases_dir / "cases.json").read_text()).get("review")
    if a.publish:
        rec = json.loads(a.publish.read_text())
        print(publish.write(rec, page_dir))
        if public:
            ids = {r["case"] for r in rec["results"]}
            print(report.write(rec, [c for c in run.load_cases(cases_dir) if c["id"] in ids], Path(public), review))
        return

    cfg = run.load_models()
    models = list(cfg["candidates"] if cfg.get("models", "candidates") == "candidates" else cfg["models"])
    cases = run.load_cases(cases_dir)
    key = run.api_key()
    credit = remaining_credit(key)
    budget = cfg.get("max_usd", 9.0) if credit is None else min(cfg.get("max_usd", 9.0), credit * 0.9)
    if a.final_only:
        finalists = a.final_only.split(",")
        passes = a.passes or cfg.get("final", {}).get("passes", 3)
        est = run.estimate(finalists, len(cases))
        while not a.passes and passes > 1 and sum(est.values()) * 1.5 * passes > budget:   # estimates run ~40% low
            passes -= 1
        log.info("final only: %d models x %d cases x %d passes (budget $%.2f)", len(finalists), len(cases), passes, budget)
        rec = run.record(run.run(key, finalists, cases, passes, budget, workers=16, log=log.info),
                         finalists, cases, passes)
        rec["screen"] = {"skipped": "this month's credit could not cover a screen; finalists chosen from the "
                                    "previous runs", "summary": [], "results": []}
    else:
        rec = two_stage(key, models, cases, cfg, budget)
    if rec is None:
        tell_admin("cvkit vision benchmark skipped", f"OpenRouter eval key has ${credit or 0:.2f} left")
        sys.exit(1)
    out = Path(os.environ.get("CVKIT_RESULTS", "/srv/data/cvkit/verify/results"))
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{rec['stamp']}.json").write_text(json.dumps(rec, indent=1))
    (out / f"{rec['stamp']}.md").write_text(run.markdown(rec))
    print(publish.write(rec, page_dir))
    if public:
        print(report.write(rec, cases, Path(public), review))
    value = rec["pick"].get("value")
    vrow = next((r for r in rec["summary"] if r["model"] == value), None)
    if not vrow or (vrow["recall"] or 0) < 0.9 or vrow["valid"] < 0.95:
        tell_admin("cvkit vision benchmark: no safe pick", f"value pick {value} did not meet the bar; model unchanged")
        return
    if a.dry_run:
        log.info("dry run: would set model %s", value)
        return
    old = asyncio.run(set_model(value))
    if old != value:
        tell_admin("cvkit verify model changed",
                   f"{old or '(none)'} -> {value}: quality {vrow['quality']}, recall {vrow['recall']:.0%}, "
                   f"${vrow['cost'] * 1000:.2f}/1k checks. Run cost ${rec['spent']:.2f}.")
    log.info("model %s (was %s)", value, old)


if __name__ == "__main__":
    main()
