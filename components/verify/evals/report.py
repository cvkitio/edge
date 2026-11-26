"""The public benchmark report for https://cvkit.net/resources/reports/vision-benchmark/.

  write(rec, cases, out_dir)   ->  out_dir/<date>/index.html      results (publish.page, public)
                                   out_dir/<date>/method.html     method and every case: frame, crop, claim,
                                                                  truth and each model's answer
                                   out_dir/<date>/cases/*.jpg     the frames and crops sent to the models
                                   out_dir/manifest.json          every run, newest first (the site build reads it)

The site (cvkitio/infra site/build.py) copies out_dir under /resources/reports/vision-benchmark/ and writes the
series index and the Resources index from manifest.json. Camera ids are replaced by letters and the site is
not named.
"""

from __future__ import annotations

import html
import json
from pathlib import Path

from cvkit_verify import vision
from evals import publish

esc = html.escape
SLUG = "vision-benchmark"
TITLE = "Vision model benchmark"
REPO = "https://github.com/cvkitio/edge"

CSS = publish.CSS + """
nav.crumbs{font-size:13.5px;color:var(--ink2);margin:0 0 18px}nav.crumbs a{color:var(--s1);text-decoration:none}
pre{background:var(--surface);border:1px solid var(--grid);border-radius:8px;padding:12px;white-space:pre-wrap;
font-size:12.5px;max-width:860px}
.filters{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0 16px}
.filters button{font:inherit;font-size:13px;padding:5px 12px;border-radius:16px;border:1px solid var(--grid);
background:var(--surface);color:var(--ink);cursor:pointer}.filters button[aria-pressed=true]{border-color:var(--s1);
box-shadow:inset 0 0 0 1px var(--s1)}
.case{background:var(--surface);border:1px solid var(--grid);border-radius:10px;padding:14px;margin:0 0 14px}
.case h3{font-size:15px;margin:0 0 8px}.case .imgs{display:grid;grid-template-columns:minmax(0,4fr) minmax(0,3fr);
gap:10px}.case img{width:100%;height:auto;border-radius:6px;display:block}
.case figcaption{font-size:12px;color:var(--muted);margin-top:3px}
.facts{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:4px 16px;font-size:13.5px;
margin:10px 0}.facts b{font-weight:600;color:var(--ink)}
.ok{color:var(--good)}.no{color:var(--bad)}td.desc{white-space:normal;text-align:left;min-width:260px;max-width:460px}
details summary{cursor:pointer;color:var(--s1);font-size:13.5px;margin:6px 0}
@media (max-width:640px){.case .imgs{grid-template-columns:minmax(0,1fr)}}
"""


def run_date(rec: dict) -> str:
    return rec["stamp"][:10]


def cam_letters(cases: list[dict]) -> dict[str, str]:
    cams = sorted({c.get("camera", "") for c in cases})
    return {c: f"camera {chr(65 + i)}" for i, c in enumerate(cams)}


def crumbs(*parts: tuple[str, str | None]) -> str:
    bits = [f'<a href="{h}">{esc(t)}</a>' if h else esc(t) for t, h in parts]
    return f'<nav class="crumbs">{" / ".join(bits)} · <a href="{REPO}">code on GitHub</a></nav>'


def mark(ok) -> str:
    return '<span class="ok">✓</span>' if ok else '<span class="no">✗</span>'


def head(title: str, desc: str) -> str:
    return (f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1"><title>{esc(title)}</title>'
            f'<meta name="description" content="{esc(desc)}"><style>{CSS}</style></head><body><main>')


def results_page(rec: dict, date: str) -> str:
    """publish.page(public=True) with this report's breadcrumbs and a link to the method."""
    body = publish.page(rec, [], public=True)
    nav = crumbs(("cvkit.net", "/"), ("Resources", "/resources/"), (TITLE, f"/resources/reports/{SLUG}/"), (date, None))
    link = (f'<p><b><a href="method.html">Method and all {rec["n_cases"]} cases</a></b>: every image sent to the '
            "models, the detector's box, and each model's verdict, label, people count and description.</p>")
    body = body.replace(publish.public_top(), nav, 1)
    return body.replace('<div class="tiles">', link + '<div class="tiles">', 1)


def method_page(rec: dict, cases: list[dict], date: str) -> str:
    letters = cam_letters(cases)
    by_case: dict[str, list[dict]] = {}
    for r in rec["results"]:
        by_case.setdefault(r["case"], []).append(r)
    order = [s["model"] for s in rec["summary"]]
    fam = {}
    for k, v in vision.FAMILY.items():
        fam.setdefault(v, []).append(k)
    fam_rows = "".join(f"<tr><td>{esc(f)}</td><td>{esc(', '.join(sorted(ks)))}</td></tr>" for f, ks in sorted(fam.items()))
    rule = rec["pick"].get("rule", {})
    n_real = rec["n_real"]

    cards = []
    for c in cases:
        rs = sorted(by_case.get(c["id"], []), key=lambda r: order.index(r["model"]) if r["model"] in order else 99)
        t = rec_truth(rs, c)
        right = sum(r["verdict"] for r in rs)
        kind = "real" if t["real"] else "false"
        x0, y0, x1, y1 = c["claim"]["bbox"]
        trs = []
        for r in rs:
            if not r["ok"]:
                trs.append(f'<tr><td>{esc(r["model"])}</td><td colspan="5" class="desc no">no valid answer: '
                           f'{esc(r.get("error", ""))}</td><td>${r["cost"]:.5f}</td><td>{r["seconds"]:.1f}</td></tr>')
                continue
            lab = r.get("answer_label")
            ppl = r.get("answer_people")
            trs.append(
                f'<tr><td>{esc(r["model"])}</td><td>{mark(r["verdict"])} {"real" if r["real"] else "false"}</td>'
                f'<td>{mark(r["label"])} {esc(lab) if lab is not None else "?"}</td>'
                f'<td>{mark(r["people"])} {ppl if ppl is not None else "?"}</td>'
                f'<td>{r["confidence"]:.2f}</td><td class="desc">{esc(r["description"])}</td>'
                f'<td>${r["cost"]:.5f}</td><td>{r["seconds"]:.1f}</td></tr>')
        cards.append(f"""<article class="case" data-kind="{kind}" data-wrong="{len(rs) - right}" id="{esc(c['id'])}">
<h3>{esc(c['id'])} · {right}/{len(rs)} models right</h3>
<div class="imgs"><figure style="margin:0"><img loading="lazy" src="cases/{esc(c['id'])}.jpg" width="960" height="720"
alt="Frame {esc(c['id'])} with the detector's {esc(c['claim']['label'])} box drawn on it"><figcaption>Image 1: the frame as the detector
saved it, box drawn by the detector</figcaption></figure>
<figure style="margin:0"><img loading="lazy" src="cases/{esc(c['id'])}-crop.jpg" width="448" height="448"
alt="Zoomed crop around the box in {esc(c['id'])}"><figcaption>Image 2: the 448 px crop sent with it</figcaption></figure></div>
<div class="facts"><span>Detector said <b>{esc(c['claim']['label'])}</b> at {c['claim']['confidence']:.2f}</span>
<span>Box x0 {x0:.3f} y0 {y0:.3f} x1 {x1:.3f} y1 {y1:.3f}</span><span>{esc(letters.get(c.get('camera', ''), ''))}</span>
<span>Truth: <b>{'real' if t['real'] else 'false alarm'}</b></span><span>Labels accepted: <b>{esc(', '.join(t['labels']))}</b></span>
<span>People in frame: <b>{t['people']}</b></span></div>
<details><summary>All {len(rs)} answers</summary><div class="card"><table><thead><tr><th>model</th><th>verdict</th>
<th>label</th><th>people</th><th>conf.</th><th style="text-align:left">description</th><th>cost</th><th>s</th></tr></thead>
<tbody>{''.join(trs)}</tbody></table></div></details></article>""")

    prompt_text = ("Camera <camera>. The detector says: <label> (score <score>) at box x0=… y0=… x1=… y1=… "
                   "(fractions of the frame). Image 1 is the full frame, image 2 the zoomed crop.")
    no_answers = not any("answer_label" in r for r in rec["results"])
    note = ("<p><b>This run predates storing each answer's label and people count</b>, so those columns show "
            "only right or wrong.</p>") if no_answers else ""
    nav = crumbs(("cvkit.net", "/"), ("Resources", "/resources/"), (TITLE, f"/resources/reports/{SLUG}/"),
                 (date, "./"), ("method", None))
    return head(f"{TITLE}: method and cases, {date}",
                f"How cvkit ranks vision models for checking security-camera detections, with all {rec['n_cases']} "
                "test images and every model's answer.") + f"""{nav}
<h1>Method and all cases</h1>
<p class="sub">Run {esc(date)} · {rec['n_cases']} cases ({n_real} real, {rec['n_cases'] - n_real} false alarms) ·
{len(order)} models · {rec['repeats']} pass · cost ${rec['spent']:.2f} · <a href="./">results</a></p>
{note}
<h2>What is being tested</h2>
<p>cvkit runs a cheap object detector on the camera network (a Hailo-8L on a Raspberry Pi 5) and only sends a
detection on when something moves. Before anyone is alerted, a vision model looks at the detection and says
whether it is real. That check is Tier 2. This benchmark measures which model does it best for the money.</p>
<p>Each case is one detection: the frame the detector saved, with its box drawn on, and the claim (label, score,
box). The model gets exactly what the live service sends: the frame, a zoomed crop around the box, and one line
of text. Answers are compared with labels assigned to each case by hand.</p>

<h2>The cases</h2>
<ul>
<li><b>{n_real} real</b> (truth: real) and <b>{rec['n_cases'] - n_real} false</b> cases, all from five fixed
fisheye cameras at one industrial site, in daylight. Camera ids are shown as letters.</li>
<li>The real cases are detector events from a 30-minute shadow run: one worker walking, cycling and pushing a pallet
jack inside a shed, and trucks passing on the road outside. Several cases show the same moment from different
cameras. One detector event is wrong (a "truck" box on a row of tanks) and its truth is false.</li>
<li>The false cases are boxes drawn the way the detector draws them, on objects in empty frames that commonly fool
detectors: ladders, extinguishers, bins, tanks, a hi-vis vest on a hook, a container. One, a "car" box on an empty
flatbed trailer, is labelled real, because a trailer is a vehicle under the prompt's rules.</li>
<li>Truth is three fields: <code>real</code>, the <code>labels</code> accepted as the main object in the box, and the
number of <code>people</code> in the whole frame. They were assigned while building the set by looking at each frame
and crop.</li>
</ul>
<p><b>Limits.</b> One site, five cameras, daylight only, mostly one person; no night, rain, glare, animals or
crowds. The false cases are constructed, not detector mistakes caught in the wild. Each model answered each case
once, so run-to-run variation is not measured. Treat the ranking as a first cut until the set covers more sites
and conditions.</p>

<h2>What the model is sent</h2>
<p>A system prompt, then a user message with one line of text and two images: the full frame (960×720 here) and a
448×448 crop centred on the box, 2.5 times the box's larger side and at least 15% of the frame. Temperature 0. Calls
go through OpenRouter's chat completions API; cost is what OpenRouter reports for the call.</p>
<pre>{esc(vision.SYSTEM)}</pre>
<pre>{esc(prompt_text)}</pre>

<h2>How answers are scored</h2>
<ul>
<li>The first JSON object in the reply is read. No JSON, or a field of the wrong type, makes the answer invalid and
scores 0.</li>
<li>The label is lower-cased and mapped to a family (table below); anything else counts as <code>none</code>.</li>
<li><b>verdict</b> = the answer's <code>real</code> equals the truth. <b>label</b> = the answer's label is one of the
accepted labels. <b>people</b> = the count matches exactly.</li>
<li><b>quality</b> = 10 × (0.7 × verdict + 0.2 × label + 0.1 × people). The verdict carries most of the weight
because it decides whether anyone is alerted.</li>
<li><b>recall</b> = share of real cases confirmed. <b>false rejected</b> = share of false cases rejected. A missed
intruder costs more than an extra alert, so recall is a hard requirement below.</li>
</ul>
<div class="card"><table><thead><tr><th>family</th><th style="text-align:left">labels mapped to it</th></tr></thead>
<tbody>{fam_rows}</tbody></table></div>

<h2>How the pick is made</h2>
<p>Best quality is the top-scoring model. The <b>value pick</b> is the cheapest model whose quality is within
{rule.get('tolerance')} of the best, with recall of at least {publish.pct(rule.get('min_recall'))}, median latency of
{rule.get('max_p50_s')} s or less and at least 98% valid answers. <b>Fastest good</b> is the quickest model that
meets the same bar. The verify service switches to the value pick after each monthly run, provided its recall is at
least 90% and at least 95% of its answers are valid.</p>

<h2>All {rec['n_cases']} cases</h2>
<div class="filters" role="group" aria-label="Filter cases">
<button data-f="all" aria-pressed="true">All</button><button data-f="real" aria-pressed="false">Real</button>
<button data-f="false" aria-pressed="false">False alarms</button><button data-f="wrong" aria-pressed="false">Any model wrong</button></div>
{''.join(cards)}
</main>
<script>
document.querySelectorAll('.filters button').forEach(b=>b.addEventListener('click',()=>{{
 document.querySelectorAll('.filters button').forEach(x=>x.setAttribute('aria-pressed',x===b));
 const f=b.dataset.f;document.querySelectorAll('.case').forEach(c=>{{
  c.hidden=!(f==='all'||c.dataset.kind===f||(f==='wrong'&&+c.dataset.wrong>0))}})}}));
</script></body></html>"""


def rec_truth(rs: list[dict], case: dict) -> dict:
    """The truth the run was scored against (stored with each result), else the case's current truth."""
    return rs[0]["truth"] if rs else case["truth"]


def headline(rec: dict) -> str:
    by = {r["model"]: r for r in rec["summary"]}
    v = by.get(rec["pick"].get("value"), {})
    if not v:
        return f"{len(rec['summary'])} models"
    return (f"Best value: {v['model']}, quality {v['quality']} with {publish.pct(v['recall'])} recall at "
            f"${v['cost'] * 1000:.2f} per 1,000 checks ({len(rec['summary'])} models, {rec['n_cases']} cases)")


def write(rec: dict, cases: list[dict], out_dir: Path) -> Path:
    date = run_date(rec)
    d = out_dir / date
    (d / "cases").mkdir(parents=True, exist_ok=True)
    for c in cases:
        (d / "cases" / f"{c['id']}.jpg").write_bytes(c["jpeg"])
        (d / "cases" / f"{c['id']}-crop.jpg").write_bytes(vision.crop(c["jpeg"], c["claim"]["bbox"]))
    (d / "index.html").write_text(results_page(rec, date))
    (d / "method.html").write_text(method_page(rec, cases, date))
    mp = out_dir / "manifest.json"
    runs = json.loads(mp.read_text())["runs"] if mp.exists() else []
    runs = [r for r in runs if r["date"] != date]
    p = rec["pick"]
    runs.append({"date": date, "stamp": rec["stamp"], "path": f"{date}/", "headline": headline(rec),
                 "value": p.get("value"), "best": p.get("best"), "fast": p.get("fast"),
                 "n_models": len(rec["summary"]), "n_cases": rec["n_cases"], "spent": rec["spent"]})
    runs.sort(key=lambda r: r["date"], reverse=True)
    manifest = {"slug": SLUG, "title": TITLE, "cadence": "monthly",
                "description": ("Which vision model best checks security-camera detections before an alert, for the "
                                "money: accuracy, recall on real events, false alarms rejected, cost and latency."),
                "runs": runs}
    tmp = mp.with_suffix(".part")
    tmp.write_text(json.dumps(manifest, indent=1))
    tmp.replace(mp)
    return d
