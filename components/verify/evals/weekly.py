"""The weekly Tier 2 vision benchmark: run the eval, keep the record, publish the page, switch the verify
service to the best-value model (nas01 CronJob cvkit/vision-benchmark, Monday morning).

  cvkit-vision-bench                          run, publish, set the model
  cvkit-vision-bench --publish FILE.json      re-publish an existing record without running anything
  cvkit-vision-bench --dry-run                run and publish, but leave the service's model alone

Environment:
  OPENROUTER_API_KEY   the eval key (its own key, so a run can never use up the service's budget)
  CVKIT_CASES          case dir (default /srv/data/cvkit/verify/cases)
  CVKIT_RESULTS        records (default /srv/data/cvkit/verify/results)
  CVKIT_PAGE_DIR       page output (default /srv/data/share/cvkitio/vision-benchmark, the tailnet share)
  NATS_URL             for the KV write (default nats://nats.cvkit:4222)
  TELEGRAM_*/ALERT_WEBHOOK_URL/NTFY_URL   optional: tell the admin when the pick changes or a run fails

The run checks the key's remaining credit first and drops the most expensive candidates until the
estimate fits in half of it.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from pathlib import Path

import requests

from cvkit_verify.alerts import Alerter
from evals import publish, run

log = logging.getLogger("evals.weekly")


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


def main(argv=None) -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--publish", type=Path, help="publish this record and stop")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    page_dir = Path(os.environ.get("CVKIT_PAGE_DIR", "/srv/data/share/cvkitio/vision-benchmark"))
    if a.publish:
        print(publish.write(json.loads(a.publish.read_text()), page_dir))
        return

    cfg = run.load_models()
    models = cfg["candidates"] if cfg.get("weekly", "candidates") == "candidates" else cfg["weekly"]
    cases = run.load_cases(Path(os.environ.get("CVKIT_CASES", "/srv/data/cvkit/verify/cases")))
    repeats = cfg.get("repeats", 1)
    key = run.api_key()
    est = run.estimate(models, len(cases) * repeats)
    credit = remaining_credit(key)
    if credit is not None:
        budget = min(cfg.get("max_usd", 3.0), credit / 2)
        while models and sum(est[m] for m in models) > budget:
            drop = max(models, key=lambda m: est[m])
            log.warning("dropping %s ($%.3f) to fit $%.2f (credit $%.2f)", drop, est[drop], budget, credit)
            models.remove(drop)
        if not models:
            tell_admin("cvkit vision benchmark skipped", f"OpenRouter eval key has ${credit:.2f} left")
            sys.exit(1)
    log.info("running %d models x %d cases x %d (est $%.2f)", len(models), len(cases), repeats,
             sum(est[m] for m in models))
    results = run.run(key, models, cases, repeats, cfg.get("max_usd", 3.0), log=log.info)
    rec = run.record(results, models, cases, repeats)
    out = Path(os.environ.get("CVKIT_RESULTS", "/srv/data/cvkit/verify/results"))
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{rec['stamp']}.json").write_text(json.dumps(rec, indent=1))
    (out / f"{rec['stamp']}.md").write_text(run.markdown(rec))
    print(publish.write(rec, page_dir))
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
