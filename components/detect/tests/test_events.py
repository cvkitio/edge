from cvkit_detect.events import Detection, EventPolicy, EventTracker

BOX = (0.1, 0.1, 0.3, 0.5)


def det(tid=1, label="person", conf=0.8):
    return Detection(tid, label, conf, BOX)


def test_start_needs_min_frames_and_end_after_lost():
    tr = EventTracker(EventPolicy(min_frames=3, lost_after_s=2.0, min_move=0))
    assert tr.update(0.0, [det()]) == []
    assert tr.update(0.1, [det()]) == []
    (start,) = tr.update(0.2, [det(conf=0.9)])
    assert start.kind == "start" and start.want_snapshot and start.frames == 3
    assert tr.update(0.3, [det()]) == []  # no duplicate start
    assert tr.update(2.0, []) == []  # not lost yet
    (end,) = tr.update(2.5, [])
    assert end.kind == "end" and end.event_id == start.event_id
    assert end.max_confidence == 0.9 and round(end.last_seen - end.first_seen, 3) == 0.3


def test_ignores_uninteresting_labels_and_low_confidence():
    tr = EventTracker(EventPolicy(min_frames=1, min_move=0))
    assert tr.update(0.0, [det(label="chair"), det(tid=2, conf=0.2)]) == []


def test_short_blip_never_emits():
    tr = EventTracker(EventPolicy(min_frames=5, lost_after_s=1.0, min_move=0))
    tr.update(0.0, [det()])
    tr.update(0.1, [det()])
    assert tr.update(5.0, []) == []


def test_reused_track_id_with_new_label_ends_old_event():
    tr = EventTracker(EventPolicy(min_frames=1, min_move=0))
    (s1,) = tr.update(0.0, [det(label="person")])
    evs = tr.update(0.1, [det(label="dog")])
    assert [e.kind for e in evs] == ["end", "start"]
    assert evs[0].event_id == s1.event_id and evs[1].label == "dog"


def test_flush_ends_open_events_and_payload_shape():
    tr = EventTracker(EventPolicy(min_frames=1, min_move=0))
    (s,) = tr.update(1700000000.0, [det()])
    (e,) = tr.flush()
    d = e.to_dict(site="home", camera="drive", edge="cvkit-edge-01", model="yolov8s_h8l")
    assert d["schema"] == "cvkit.detect.v1" and d["kind"] == "end" and d["event_id"] == s.event_id
    assert d["ts"].endswith("+00:00") and d["bbox"] == list(BOX)


def moved(tid, x, label="person", conf=0.8):
    return Detection(tid, label, conf, (x, 0.1, x + 0.2, 0.5))


def test_static_object_never_starts_an_event():
    # A fire extinguisher detected as a person for a minute, box jittering a little.
    tr = EventTracker(EventPolicy(min_frames=3, min_move=0.02))
    for i in range(600):
        assert tr.update(i * 0.1, [moved(1, 0.4 + 0.002 * (i % 3))]) == []
    assert not tr.moving(60.0)


def test_moving_object_starts_once_it_has_moved():
    tr = EventTracker(EventPolicy(min_frames=3, min_move=0.02))
    evs = []
    for i in range(10):
        evs += tr.update(i * 0.1, [moved(1, 0.1 + 0.006 * i)])  # walks 0.006/frame
    (start,) = evs
    assert start.kind == "start" and start.frames == 5  # 0.02 reached on the 5th frame
    assert tr.moving(1.0)


def test_parked_object_stops_counting_as_moving():
    tr = EventTracker(EventPolicy(min_frames=1, min_move=0.02, move_window_s=2.0))
    for i in range(20):  # drives in
        tr.update(i * 0.1, [moved(1, 0.1 + 0.02 * i)])
    for i in range(20, 60):  # parks
        tr.update(i * 0.1, [moved(1, 0.48)])
    assert tr.moving(3.0) and not tr.moving(5.9)
