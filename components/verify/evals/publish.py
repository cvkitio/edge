"""The vision benchmark page, from eval records (evals/run.py record()).

  <out>/index.html          latest run (the page people link to)
  <out>/<stamp>.html        each run, kept
  <out>/history.json        picks and spend per run (for the trend table)

What's on it: the pick (best value, best, fastest), a quality-vs-cost scatter, the leaderboard, the trend of
picks over runs and the method. Case images are not published (they are real site footage).
"""

from __future__ import annotations

import html
import json
import math
from pathlib import Path

esc = html.escape

CSS = """
:root{--page:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#8a8984;--grid:#e6e5e0;
--s1:#2a78d6;--s2:#eb6834;--good:#0ca30c;--bad:#d03b3b}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--page:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;
--ink2:#c3c2b7;--muted:#8f8e86;--grid:#2c2c2a;--s1:#3987e5;--s2:#d95926}}
:root[data-theme=dark]{--page:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--muted:#8f8e86;--grid:#2c2c2a;
--s1:#3987e5;--s2:#d95926}
*{box-sizing:border-box}body{margin:0;background:var(--page);color:var(--ink);
font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:1080px;margin:0 auto;padding:24px 16px 64px}h1{font-size:26px;margin:0 0 4px}
h2{font-size:18px;margin:36px 0 10px}.sub{color:var(--ink2);margin:0 0 20px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
.tile{background:var(--surface);border:1px solid var(--grid);border-radius:10px;padding:14px 16px}
.tile .k{color:var(--ink2);font-size:13px}.tile .v{font-size:19px;font-weight:600;margin:2px 0;word-break:break-word}
.tile .d{color:var(--muted);font-size:13px}
.card{background:var(--surface);border:1px solid var(--grid);border-radius:10px;padding:12px;overflow-x:auto}
table{border-collapse:collapse;width:100%;font-size:13.5px}th,td{padding:6px 8px;text-align:right;
border-bottom:1px solid var(--grid);white-space:nowrap}th{color:var(--ink2);font-weight:500}
td:first-child,th:first-child{text-align:left}tr.pick td:first-child{font-weight:600}
.tag{display:inline-block;font-size:11px;border:1px solid var(--s2);color:var(--ink);border-radius:9px;
padding:0 6px;margin-left:6px}
svg text{fill:var(--ink2);font-size:11px}.ax{stroke:var(--grid)}.dot{stroke:var(--surface);stroke-width:2}
.legend{display:flex;gap:16px;color:var(--ink2);font-size:13px;margin:4px 0 6px}
.legend i{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;vertical-align:-1px}
#tip{position:fixed;pointer-events:none;background:var(--surface);border:1px solid var(--grid);border-radius:8px;
padding:6px 9px;font-size:12.5px;display:none;box-shadow:0 2px 8px rgba(0,0,0,.15)}
p,li{color:var(--ink2);max-width:760px}code{font-size:13px}
"""


def pct(x):
    return "" if x is None else f"{x:.0%}"


def scatter(rows: list[dict], picks: set[str]) -> str:
    w, h, l, r, t, b = 900, 360, 48, 16, 12, 40
    pts = [x for x in rows if x["cost"] > 0]
    if not pts:
        return ""
    lo = math.log10(min(x["cost"] * 1000 for x in pts)) - 0.15
    hi = math.log10(max(x["cost"] * 1000 for x in pts)) + 0.15
    qmin = max(0, math.floor(min(x["quality"] for x in pts)) - 0.5)
    X = lambda c: l + (math.log10(c * 1000) - lo) / (hi - lo) * (w - l - r)  # noqa: E731
    Y = lambda q: t + (10 - q) / (10 - qmin) * (h - t - b)  # noqa: E731
    out = [f'<svg viewBox="0 0 {w} {h}" width="100%" role="img" aria-label="Quality against cost per 1000 checks">']
    q = math.ceil(qmin)
    while q <= 10:
        out.append(f'<line class="ax" x1="{l}" x2="{w - r}" y1="{Y(q):.1f}" y2="{Y(q):.1f}"/>'
                   f'<text x="{l - 8}" y="{Y(q) + 4:.1f}" text-anchor="end">{q}</text>')
        q += 1
    for e in range(math.floor(lo), math.ceil(hi) + 1):
        for m in (1, 3):
            v = m * 10 ** e
            if lo <= math.log10(v) <= hi:
                x = X(v / 1000)
                out.append(f'<text x="{x:.1f}" y="{h - b + 18}" text-anchor="middle">${v:g}</text>')
    out.append(f'<text x="{(l + w) / 2}" y="{h - 4}" text-anchor="middle">cost per 1,000 checks (log scale)</text>')
    out.append(f'<text x="12" y="{t + 6}" transform="rotate(-90 12 {t + 6})" text-anchor="end">quality /10</text>')
    for x in sorted(pts, key=lambda x: x["model"] in picks):
        pk = x["model"] in picks
        cx, cy = X(x["cost"]), Y(x["quality"])
        tip = (f"{x['model']}|quality {x['quality']} · accuracy {pct(x['accuracy'])}|recall {pct(x['recall'])} · "
               f"false rejected {pct(x['rejection'])}|${x['cost'] * 1000:.2f} per 1k · p50 {x['p50_s']} s")
        out.append(f'<circle class="dot" cx="{cx:.1f}" cy="{cy:.1f}" r="{7 if pk else 5}" '
                   f'fill="var(--{"s2" if pk else "s1"})" data-tip="{esc(tip)}"/>'
                   f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="14" fill="transparent" data-tip="{esc(tip)}"/>')
        if pk:
            anchor = "end" if cx > w * 0.75 else "start"
            dx = -12 if anchor == "end" else 12
            out.append(f'<text x="{cx + dx:.1f}" y="{cy - 9:.1f}" text-anchor="{anchor}" '
                       f'style="fill:var(--ink)">{esc(x["model"].split("/")[-1])}</text>')
    out.append("</svg>")
    return "".join(out)


def page(rec: dict, history: list[dict]) -> str:
    rows, p = rec["summary"], rec["pick"]
    by = {x["model"]: x for x in rows}
    picks = {p.get("value"), p.get("best"), p.get("fast")}

    def tile(k, m, d):
        x = by.get(m, {})
        return (f'<div class="tile"><div class="k">{k}</div><div class="v">{esc(m or "-")}</div>'
                f'<div class="d">{d(x) if x else ""}</div></div>')

    v = by.get(p.get("value"), {})
    per_day = v.get("cost", 0) * 1000
    tiles = "".join([
        tile("Best value (in use)", p.get("value"),
             lambda x: f"quality {x['quality']} · recall {pct(x['recall'])} · ${x['cost'] * 1000:.2f}/1k · {x['p50_s']} s"),
        tile("Best quality", p.get("best"), lambda x: f"quality {x['quality']} · ${x['cost'] * 1000:.2f}/1k"),
        tile("Fastest good", p.get("fast"), lambda x: f"p50 {x['p50_s']} s · ${x['cost'] * 1000:.2f}/1k"),
    ])
    trs = []
    for x in rows:
        tags = "".join(f'<span class="tag">{t}</span>' for t, k in (("value", "value"), ("best", "best"), ("fast", "fast"))
                       if p.get(k) == x["model"])
        trs.append(f'<tr class="{"pick" if x["model"] in picks else ""}"><td>{esc(x["model"])}{tags}</td>'
                   f'<td>{x["quality"]}</td><td>{pct(x["accuracy"])}</td><td>{pct(x["recall"])}</td>'
                   f'<td>{pct(x["rejection"])}</td><td>{pct(x["label"])}</td><td>{pct(x["valid"])}</td>'
                   f'<td>${x["cost"] * 1000:.2f}</td><td>{x["p50_s"]}</td><td>{x["p90_s"]}</td></tr>')
    hist = "".join(f'<tr><td>{esc(h["stamp"])}</td><td>{esc(h.get("value") or "")}</td><td>{esc(h.get("best") or "")}</td>'
                   f'<td>{h.get("n_models")}</td><td>${h.get("spent", 0):.2f}</td></tr>' for h in reversed(history[-20:]))
    rule = p.get("rule", {})
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Vision Model Benchmark</title>
<style>{CSS}</style></head><body><main>
<h1>Tier 2 vision model benchmark</h1>
<p class="sub">cvkit verify · {esc(rec['stamp'])} · {rec['n_cases']} detections ({rec['n_real']} real,
{rec['n_cases'] - rec['n_real']} false alarms) · {len(rows)} models · run cost ${rec['spent']:.2f}</p>
<div class="tiles">{tiles}</div>
<p>At the value pick's price, 1,000 verified detections a day costs about ${per_day:.2f} a day.</p>
<h2>Quality against cost</h2>
<div class="legend"><span><i style="background:var(--s2)"></i>picked</span><span><i style="background:var(--s1)"></i>other models</span></div>
<div class="card">{scatter(rows, picks)}</div>
<h2>Leaderboard</h2>
<div class="card"><table><thead><tr><th>model</th><th>quality</th><th>accuracy</th><th>recall</th>
<th>false rejected</th><th>label</th><th>valid JSON</th><th>$/1k checks</th><th>p50 s</th><th>p90 s</th></tr></thead>
<tbody>{''.join(trs)}</tbody></table></div>
<h2>Runs</h2>
<div class="card"><table><thead><tr><th>run</th><th>value pick</th><th>best</th><th>models</th><th>cost</th></tr></thead>
<tbody>{hist}</tbody></table></div>
<h2>Method</h2>
<ul>
<li>Each case is a Tier 1 start event from the AU01 cameras: the edge snapshot with the detector's box drawn on it,
plus the claim (label, score, box). It is sent exactly as the verify service sends it: the frame and a zoomed crop around the box.</li>
<li>Real cases are the worker walking, cycling and pushing a pallet jack, and trucks passing on the road, from a 30-minute
shadow run (one was a "truck" box on a tank row, which should be rejected). False cases are detector-style boxes drawn
on static objects in empty frames: ladders, extinguishers, bins, tanks, a hi-vis vest on a hook, a container (a box on an empty trailer counts as a real vehicle).</li>
<li>Quality = 10 × (0.7 × verdict right + 0.2 × label right + 0.1 × people count right); 0 when no valid JSON came back.
Recall = real detections confirmed; false rejected = false alarms rejected. Cost is what OpenRouter charged.</li>
<li>Value pick: the cheapest model within {rule.get('tolerance')} of the best quality, with recall ≥ {pct(rule.get('min_recall'))},
median latency ≤ {rule.get('max_p50_s')} s and valid JSON ≥ 98%. The verify service switches to it automatically (NATS KV <code>cvkit_verify.model</code>).</li>
</ul>
</main><div id="tip"></div>
<script>
const tip=document.getElementById('tip');
document.querySelectorAll('[data-tip]').forEach(el=>{{
 el.addEventListener('mousemove',e=>{{tip.innerHTML=el.dataset.tip.split('|').map((s,i)=>i?s:'<b>'+s+'</b>').join('<br>');
  tip.style.display='block';tip.style.left=Math.min(e.clientX+14,innerWidth-260)+'px';tip.style.top=(e.clientY+14)+'px'}});
 el.addEventListener('mouseleave',()=>tip.style.display='none')}});
</script></body></html>"""


def write(rec: dict, out: Path) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    hp = out / "history.json"
    history = json.loads(hp.read_text()) if hp.exists() else []
    history = [h for h in history if h["stamp"] != rec["stamp"]]
    history.append({"stamp": rec["stamp"], "value": rec["pick"].get("value"), "best": rec["pick"].get("best"),
                    "n_models": len(rec["summary"]), "spent": rec["spent"]})
    hp.write_text(json.dumps(history, indent=1))
    html_ = page(rec, history)
    (out / f"{rec['stamp']}.html").write_text(html_)
    (out / "index.html").write_text(html_)
    return out / "index.html"
