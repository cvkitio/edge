import asyncio
import datetime as dt
import io
import json

from PIL import Image

from cvkit_verify import alerts, service, vision
from evals import run


def jpeg(w=960, h=720):
    buf = io.BytesIO()
    Image.new("RGB", (w, h), (120, 120, 120)).save(buf, "JPEG")
    return buf.getvalue()


def test_parse_variants():
    v = vision.parse("m", 'Sure: {"real": true, "label": "Truck", "people": 2, "confidence": 0.9, "description": "x"}')
    assert v.ok and v.real and v.label == "vehicle" and v.people == 2
    v = vision.parse("m", '```json\n{"real": "false", "label": "ladder"}\n```')
    assert v.ok and not v.real and v.label == "none"
    assert not vision.parse("m", "I cannot tell").ok
    assert not vision.parse("m", '{"real": true, "people": "lots"}').ok


def test_crop_square_and_clamped():
    c = Image.open(io.BytesIO(vision.crop(jpeg(), [0.97, 0.0, 1.0, 0.05])))
    assert c.size == (448, 448)


def test_messages_have_two_images():
    m = vision.messages(jpeg(), vision.Claim("person", 0.6, [0.1, 0.1, 0.2, 0.3], "cam"))
    assert [p["type"] for p in m[1]["content"]] == ["text", "image_url", "image_url"]


def test_score_and_pick():
    case = {"truth": {"real": True, "labels": ["person"], "people": 1}}
    good = vision.Verdict("a", True, real=True, label="person", people=1)
    assert run.score(case, good)["quality"] == 10.0
    assert run.score(case, vision.Verdict("a", False))["quality"] == 0
    assert run.score(case, vision.Verdict("a", True, real=False, label="none"))["quality"] == 0.0
    rows = [
        {"model": "big", "quality": 9.8, "recall": 1.0, "p50_s": 4, "valid": 1, "cost": 0.004},
        {"model": "cheap", "quality": 9.6, "recall": 0.96, "p50_s": 3, "valid": 1, "cost": 0.0003},
        {"model": "cheaper-misses", "quality": 9.6, "recall": 0.9, "p50_s": 2, "valid": 1, "cost": 0.0001},
        {"model": "slow", "quality": 9.7, "recall": 1.0, "p50_s": 30, "valid": 1, "cost": 0.0002},
    ]
    p = run.pick(rows)
    assert p["best"] == "big" and p["value"] == "cheap" and p["fast"] == "cheap"


def test_rule_hours_wrap_midnight():
    r = alerts.Rule(hours="18:00-06:30")
    assert r.in_hours(dt.datetime(2026, 1, 1, 23, 0)) and r.in_hours(dt.datetime(2026, 1, 1, 5, 0))
    assert not r.in_hours(dt.datetime(2026, 1, 1, 12, 0))


def test_cooldown_per_group():
    a = alerts.Alerter([alerts.Rule(labels=["person"], cooldown_s=300)])
    assert a.due("axis-81-2", "au01.hall", "person", 0.9, True)
    assert not a.due("axis-81-3", "au01.hall", "person", 0.9, True)  # same group: cooled down
    assert a.due("axis-82-1", "au01.axis-82-1", "person", 0.9, True)
    assert not a.due("axis-82-2", "au01.axis-82-2", "vehicle", 0.9, True)  # no rule for vehicles


class FakeMsg:
    def __init__(self, data):
        self.data = data


class FakeJS:
    def __init__(self):
        self.published = []

    async def get_last_msg(self, stream, subject, direct=False):
        return FakeMsg(jpeg())

    async def publish(self, subject, data, headers=None):
        self.published.append((subject, json.loads(data)))


def start(cam, label, conf, eid):
    return {"kind": "start", "site": "au01", "camera": cam, "label": label, "max_confidence": conf,
            "bbox": [0.1, 0.1, 0.2, 0.3], "event_id": eid, "ts": "2026-11-26T00:11:27.182+00:00",
            "snapshot": {"subject": f"snapshots.au01.{cam}.{eid}"}}


def test_dedupe_one_call_per_group_and_alert(monkeypatch):
    cfg = service.Config(groups={"hall": ["axis-81-2", "axis-81-3", "axis-81-12"]},
                         rules=[alerts.Rule(labels=["person"])])
    v = service.Verifier(cfg)
    v.key = "k"
    calls = []

    def fake_verify(key, model, jpg, claim, **kw):
        calls.append(claim)
        return vision.Verdict(model, True, real=claim.label == "person", label="person" if claim.label == "person" else "none",
                              people=1, confidence=0.9, description="a worker walks", cost=0.0003, seconds=1.0)

    monkeypatch.setattr(vision, "verify", fake_verify)
    monkeypatch.setattr(alerts.Alerter, "push", lambda self, *a: [])
    js = FakeJS()
    batch = [start("axis-81-2", "person", 0.6, "e1"), start("axis-81-3", "person", 0.8, "e2"),
             start("axis-81-12", "person", 0.7, "e3"), start("axis-82-2", "dog", 0.5, "e4")]
    asyncio.run(v.process(js, batch))
    assert len(calls) == 2                                  # hall persons share one call
    assert max(c.confidence for c in calls if c.label == "person") == 0.8
    verified = [d for s, d in js.published if s.startswith("events.verified.")]
    assert len(verified) == 4
    assert {d["dedupe_of"] for d in verified if d["camera"] in ("axis-81-2", "axis-81-12")} == {"e2"}
    assert next(d for d in verified if d["event_id"] == "e4")["real"] is False
    alerts_ = [d for s, d in js.published if s.startswith("alerts.")]
    assert len(alerts_) == 1 and alerts_[0]["cameras"] == ["axis-81-12", "axis-81-2", "axis-81-3"]


def test_over_budget_publishes_unverified_and_still_alerts(monkeypatch):
    cfg = service.Config(max_usd_day=0.0, rules=[alerts.Rule(labels=["person"])])
    v = service.Verifier(cfg)
    v.key = "k"
    monkeypatch.setattr(vision, "verify", lambda *a, **k: (_ for _ in ()).throw(AssertionError("no call")))
    monkeypatch.setattr(alerts.Alerter, "push", lambda self, *a: [])
    js = FakeJS()
    asyncio.run(v.process(js, [start("axis-81-2", "person", 0.6, "e1")]))
    ver = [d for s, d in js.published if s.startswith("events.verified.")][0]
    assert ver["status"] == "unverified" and ver["reason"] == "daily budget used" and ver["real"] is None
    al = [d for s, d in js.published if s.startswith("alerts.")]
    assert al and al[0]["status"] == "unverified"


def test_rejected_rep_does_not_hide_a_real_event(monkeypatch):
    v = service.Verifier(service.Config(rules=[alerts.Rule(labels=["person"])]))
    v.key = "k"
    seen = []

    def fake_verify(key, model, jpg, claim, **kw):
        seen.append(claim.confidence)
        real = claim.confidence < 0.6          # the 0.64 box is a vest; the 0.55 one is a person
        return vision.Verdict(model, True, real=real, label="person" if real else "none", confidence=0.9,
                              description="x", cost=0.0002)

    monkeypatch.setattr(vision, "verify", fake_verify)
    monkeypatch.setattr(alerts.Alerter, "push", lambda self, *a: [])
    js = FakeJS()
    asyncio.run(v.process(js, [start("axis-82-2", "person", 0.64, "vest"), start("axis-82-2", "person", 0.55, "man")]))
    assert seen == [0.64, 0.55]
    ver = {d["event_id"]: d for s, d in js.published if s.startswith("events.verified.")}
    assert ver["vest"]["real"] is False and ver["man"]["real"] is True and ver["man"]["dedupe_of"] is None
    assert any(s.startswith("alerts.") for s, _ in js.published)


def test_check_limit_marks_rest_unverified(monkeypatch):
    v = service.Verifier(service.Config(max_checks_per_group=2, rules=[alerts.Rule(labels=["person"])]))
    v.key = "k"
    monkeypatch.setattr(vision, "verify", lambda *a, **k: vision.Verdict("m", True, real=False, label="none"))
    monkeypatch.setattr(alerts.Alerter, "push", lambda self, *a: [])
    js = FakeJS()
    asyncio.run(v.process(js, [start("c", "person", 0.9 - i / 10, f"e{i}") for i in range(4)]))
    ver = {d["event_id"]: d for s, d in js.published if s.startswith("events.verified.")}
    assert [ver[f"e{i}"]["status"] for i in range(4)] == ["verified", "verified", "unverified", "unverified"]
    assert ver["e3"]["reason"] == "check limit" and ver["e3"]["dedupe_of"] == "e2"


def test_run_keeps_answer_fields(monkeypatch):
    monkeypatch.setattr(vision, "verify", lambda *a, **k: vision.Verdict("m", True, real=True, label="vehicle", people=3))
    case = {"id": "c", "jpeg": b"", "camera": "x", "claim": {"label": "car", "confidence": 0.5, "bbox": [0, 0, 1, 1]},
            "truth": {"real": True, "labels": ["vehicle"], "people": 0}}
    r = run.run("k", ["m"], [case], log=lambda *a: None)[0]
    assert r["answer_label"] == "vehicle" and r["answer_people"] == 3 and r["label"] == 1 and r["people"] == 0



def test_public_report_hides_site(tmp_path):
    from evals import publish, report
    rows = [{"model": "a/m", "quality": 9.8, "accuracy": 0.99, "recall": 1.0, "rejection": 0.96, "label": 0.9,
             "people": 0.8, "valid": 1.0, "cost": 0.0001, "p50_s": 4.0, "p90_s": 6.0}]
    case = {"id": "real-01", "camera": "axis-81-2", "jpeg": jpeg(), "claim": {"label": "person", "confidence": 0.6,
            "bbox": [0.1, 0.1, 0.2, 0.3]}, "truth": {"real": True, "labels": ["person"], "people": 1}}
    res = {"model": "a/m", "case": "real-01", "truth": case["truth"], "ok": True, "real": True, "answer_label": "person",
           "answer_people": 1, "confidence": 0.9, "description": "a worker walks past", "cost": 0.0001,
           "seconds": 2.0, "verdict": 1, "label": 1, "people": 1, "quality": 10.0}
    rec = {"stamp": "2026-12-01-0630", "n_cases": 1, "n_real": 1, "spent": 2.4, "repeats": 1, "summary": rows,
           "results": [res], "pick": {"best": "a/m", "value": "a/m", "fast": "a/m", "rule": run.pick(rows)["rule"]}}
    priv = publish.page(rec, [{"stamp": "x", "value": "a/m", "best": "a/m", "n_models": 1, "spent": 1}])
    assert "AU01" in priv and "Runs" in priv
    d = report.write(rec, [case], tmp_path)
    assert d.name == "2026-12-01" and (d / "cases" / "real-01-crop.jpg").exists()
    for page in ("index.html", "method.html"):
        text = (d / page).read_text()
        for word in ("AU01", "au01", "axis-81", "NATS", "tank row", "tree/main"):
            assert word not in text, (page, word)
    method = (d / "method.html").read_text()
    assert "a worker walks past" in method and "camera A" in method and 'src="cases/real-01.jpg"' in method
    assert "method.html" in (d / "index.html").read_text()
    m = json.loads((tmp_path / "manifest.json").read_text())
    assert m["runs"][0]["path"] == "2026-12-01/" and "a/m" in m["runs"][0]["headline"]


def test_two_stage_screens_then_repeats_cheap_finalists(monkeypatch):
    from evals import monthly
    price = {"cheap-good": 0.0001, "cheap-bad": 0.0001, "pricey-good": 0.004, "mid-good": 0.0003}

    def fake_verify(key, model, jpg, claim, **kw):
        real = claim.label == "person"
        ans = real if model != "cheap-bad" else not real
        return vision.Verdict(model, True, real=ans, label="person" if ans else "none", people=1,
                              cost=price[model], seconds=1.0)

    monkeypatch.setattr(vision, "verify", fake_verify)
    monkeypatch.setattr(run, "estimate", lambda models, n: {m: price[m] * n for m in models})
    cases = [{"id": f"c{i}", "jpeg": b"", "camera": "x", "source": "s" if i % 2 else "t",
              "claim": {"label": "person" if i % 3 else "dog", "confidence": 0.5, "bbox": [0, 0, 1, 1]},
              "truth": {"real": bool(i % 3), "labels": ["person"] if i % 3 else ["none"], "people": None}}
             for i in range(30)]
    cfg = {"screen": {"cases": 10}, "final": {"models": 2, "passes": 3, "max_cost_per_1k": 1.0}}
    rec = monthly.two_stage("k", list(price), cases, cfg, budget=10.0)
    assert rec["screen"]["n_cases"] == 10 and len(rec["screen"]["summary"]) == 4
    assert rec["models"] == ["cheap-good", "mid-good"]          # pricey excluded by cost, bad by quality
    assert rec["repeats"] == 3 and len(rec["results"]) == 2 * 30 * 3
    assert rec["pick"]["value"] == "cheap-good"
    assert {r["source"] for r in rec["results"]} == {"s", "t"}
    assert rec["summary"][0]["people"] is None and "by_source" in rec["summary"][0]
