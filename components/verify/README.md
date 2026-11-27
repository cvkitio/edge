# cvkit-verify (Tier 2)

Tier 2 of the cvkit pipeline runs on nas01 (k3s, namespace `cvkit`). It checks Tier 1 detections with a
vision model before anyone is alerted, and benchmarks vision models every month to choose the one it uses.

```
Tier 0  every frame, every camera   compressed-domain motion gate (edge Pi, no decode)
Tier 1  only while something moves  Hailo YOLO + tracker + movement filter (edge Pi)
Tier 2  only Tier 1 start events    vision model via OpenRouter (nas01)   <- this component
```

## What it does

1. A durable pull consumer (`verify`) on the hub's `DETECT` stream receives `events.detect.*` start events.
2. Events are collected for `batch_window_s` (4 s). Within a window, events from cameras in the same
   `groups` entry with the same label family become one check, using the highest-scoring snapshot.
   The three AU01 hall cameras see one worker as one check and one alert. A real verdict covers the
   rest of the group. A rejection covers only the event that was checked: the next one is checked too
   (up to `max_checks_per_group`), so a false box with a high score can't hide a real person.
3. The model gets the snapshot (with the detector's box drawn on it) and a zoomed crop of the box, and
   returns `{real, label, people, confidence, description}` (`cvkit_verify/vision.py`).
4. Every original event is published to `events.verified.<site>.<camera>` (schema `cvkit.verified.v1`,
   with `dedupe_of` pointing at the event that was checked). Both subjects are stored in the hub's
   `VERIFIED` stream.
5. When the verdict is real and an `[[alert]]` rule matches (label, cameras, local hours, confidence,
   per-group cooldown), it publishes `alerts.<site>.<camera>` (`cvkit.alert.v1`) and pushes to the
   configured sinks: Telegram photo, ntfy, or a webhook.

Failure behaviour: if the daily budget (`max_usd_day`) is used up, the API fails twice (the model, then
`fallback`), or the snapshot is missing, the event is published with `status = "unverified"`. Rules
with `unverified = true` (the default) still alert, so a real person is never silently dropped.

## Model choice: the monthly benchmark

`evals/` follows the same pattern as Readiday's and helmwatch's evals:

- `evals/run.py` runs labelled cases through the candidate models in `evals/models.json` and scores
  each answer (0.7 verdict, 0.2 label, 0.1 people count). It reports recall on real detections,
  rejection of false alarms, cost per check (as charged by OpenRouter) and latency.
- The **value pick** is the cheapest model within 0.3 quality of the best, with recall of at least 95%,
  median latency of 8 s or less, and at least 98% valid JSON.
- The monthly run has two stages to stay within the eval key's $10 a month (`models.json` `screen` and
  `final`). Every candidate answers a fixed, stratified 100-case sample once. Then up to 8 models within
  0.5 quality of the best, with screen recall of at least 90% and a cost under $1 per 1,000 checks, answer
  every case 3 times, and the pick comes from that final stage. A single pass couldn't separate close models:
  two runs on the same day picked different ones. The budget is 90% of the key's remaining credit, capped at
  `max_usd`. The screen drops the priciest models to fit its half; the final cuts passes, then models.
- `evals/monthly.py` (CronJob `vision-benchmark`, 06:30 Hobart on the 1st of each month) runs the
  benchmark, keeps the record in `/srv/data/cvkit/verify/results/`, and publishes the full page with run
  history to `/srv/data/share/cvkitio/vision-benchmark/` (https://nas01.tail7d57a9.ts.net:8443/cvkitio/vision-benchmark/).
- It also writes the public report (`evals/report.py`) to `/srv/data/cvkit/verify/public/vision-benchmark/`:
  one folder per run with the results page, a method page showing every case (the frame with the
  detector's box, the crop sent with it, the truth and every model's verdict, label, people count,
  description, cost and time) and the case images, plus `manifest.json`. The site is not named and camera
  ids become letters. cvkitio/infra `site/publish.sh` (a daily systemd user timer on nas01, because the pod
  has no AWS credentials) builds cvkit.net from that folder: the run at
  https://cvkit.net/resources/reports/vision-benchmark/, the Resources index and the sitemap.
  It then writes the value pick to NATS KV `cvkit_verify.model`; the service reloads it within a minute.
  Before a run it checks the eval key's remaining credit and drops the most expensive candidates until
  the estimate fits in half of it. It notifies the admin sinks when the pick changes or a run fails.

The cases live outside the repo in `/srv/data/cvkit/verify/cases-v2/` (`cases.json`, `images/`,
`review.json`). There are 293 cases from four sources:

| Source | Real | False | What it adds |
|---|---|---|---|
| Industrial site (5 fisheye cameras) | 53 | 25 | the deployment itself; truth by hand. False cases are boxes drawn on ladders, extinguishers, a hi-vis vest |
| MEVA (CC BY 4.0) | 80 | 0 | another site: 24 surveillance cameras, indoor and outdoor, people and vehicles |
| ENA24 trail cameras (CDLA-Permissive) | 57 | 13 | infrared night and animals; animals the detector called a person |
| Open Images V7 (CC BY 2.0) | 26 | 39 | snow, fog, dark streets; statues, dolls, posters, screens the detector called a person |

For the datasets, every claim is real output of the live Tier 1 model on the Hailo, and truth comes from the
datasets' labels. Every case was then viewed by eye. 15 Open Images cases were excluded and 9 relabelled
(pictures of people marked as real, real animals marked as absent); `review.json` records each one with its
reason. How to rebuild the set is in `evals/datasets/README.md`.

A case looks like this:

```json
{"id": "real-03", "image": "images/real-03.jpg", "camera": "axis-81-2", "kind": "tier1",
 "claim": {"label": "person", "confidence": 0.59, "bbox": [0.59, 0.03, 0.62, 0.12]},
 "truth": {"real": true, "labels": ["person"], "people": 1}}
```

Run it by hand:

```sh
cd components/verify
OPENROUTER_API_KEY=... python -m evals.run --estimate
OPENROUTER_API_KEY=... python -m evals.run [--models a,b] [--limit 20] [--max-usd 1]
```

## Deploy

```sh
components/verify/deploy/deploy.sh          # build, import into k3s, apply k8s.yaml
TELEGRAM_REF=op://Automation/<item>/notesPlain TELEGRAM_CHAT_ID=<id> components/verify/deploy/deploy.sh
```

The image is built locally and imported into k3s (`imagePullPolicy: Never`). Secrets are set one field
at a time from `op read`, so a deploy never drops a field set earlier:

| Secret | Field | Used by |
|---|---|---|
| `cvkit-verify` | `OPENROUTER_API_KEY` | the service (OpenRouter key `cvkitio`, $10/month limit) |
| `cvkit-verify` | `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`, `NTFY_URL`, `ALERT_WEBHOOK_URL` | alert sinks (all optional) |
| `cvkit-verify-eval` | `OPENROUTER_API_KEY` | the benchmark (key `cvkitio-eval`, $10/month limit), kept apart so a run can't use up the service's budget |

The site config (camera groups and alert rules) is the `cvkit-verify` ConfigMap in `deploy/k8s.yaml`.

## Watching it

```sh
nats --js-domain hub sub 'events.verified.>'     # every verdict
nats --js-domain hub sub 'alerts.>'              # alerts
nats sub verify.status                           # spend today, calls, kept/dropped, model in use
kubectl -n cvkit logs deploy/cvkit-verify -f
```

## Not built yet

- OpenRouter's `:batch` model variants cost half as much, but they only work through its batch API,
  not chat completions. They would suit a nightly pass that re-describes the day's events. The live
  path stays synchronous because alerts need answers in seconds.
