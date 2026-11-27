# detect — on-device detection on a Hailo

Runs a tiered pipeline on a Raspberry Pi 5 with a Hailo AI HAT: a near-free
motion check on every camera's compressed stream, YOLO on the Hailo only where
something moves, and events (with snapshots) on NATS JetStream for heavier
processing downstream.

## Tiers: cheap first, expensive only when it's worth it

| Tier | Where | What | Cost per camera | Runs |
|------|-------|------|-----------------|------|
| 0 | edge, CPU | cvkit emd inspector + quiet-floor rule on encoded frame sizes (no decode) | µs per frame | always, every camera |
| 1 | edge, Hailo | decode, YOLO, tracker, "has it moved" filter, snapshot | ~17 ms Hailo + decode per frame | only while the Tier 0 gate is open |
| 2 | nas01 / cloud | vision model on snapshots or clips (describe, verify, classify) | seconds and cents per event | only on Tier 1 events, batched |

**Tier 0** (`motion.py`) reuses `components/emd/src/emd_inspector.c` unchanged,
built as `libemd_inspector.so` and called through ctypes. A static scene from a
modern encoder is a stream of tiny P-frames (40–100 bytes on an Axis
sub-stream with Zipstream), and movement makes them jump. Two signals open the
gate: the inspector's burst detector (z-score against its running mean), and a
quiet-floor rule (P-frame > 2× the scene's quiet floor and > floor + 100 B).
The floor is needed because the inspector's mean and variance soak up
sustained motion within a second or two, and a busy scene never looks unusual
to it. The gate stays open while either signal continues and while Tier 1 is
tracking something that moves, closes `hold_s` after that, and never stays
open longer than `max_open_s`.

**Tier 1** (`pipeline.py`) is a second GStreamer pipeline fed through an
`appsrc`. When the gate opens, the encoded frames since the last keyframe are
replayed so decoding starts at once (ask the camera for a 1 s keyframe
interval). A closed gate costs no decode and no Hailo time. Events need a
track that has *moved* (`min_move`), so a fire extinguisher or a parked
forklift that YOLO mislabels never becomes an event.

**Tier 2** is `components/verify` on nas01: it reads `DETECT` start events and
their snapshots, merges cameras that overlap, asks a vision model whether each
detection is real, publishes `events.verified.<site>.<camera>` and raises
alerts. Only events that pass Tier 1 reach it, so cost scales with activity,
not with camera count. See its README.

### Tuning Tier 0

1. Run with `[gate] mode = "shadow"` and `trace_dir` set. Tier 1 then sees every
   frame (ground truth), and Tier 0 logs one CSV row per encoded frame.
2. Export the run's `events.detect` messages to JSON.
3. `PYTHONPATH=. python tools/gate_replay.py --traces <dir> --events events.json
   --grid floor_ratio=1.5,2,3 floor_min_bytes=50,100,200`
   reports recall (moving objects the gate would have passed to Tier 1), duty
   (share of camera-time the Hailo runs) and opens per hour for each setting.
   Override per camera with `gate = { ... }` in its `[[camera]]` block.

```
camera ──RTSP sub-stream──▶ cvkit-edge-01 (Pi 5 + Hailo-8L)
                              decode → YOLOv8s on Hailo → hailotracker → EventTracker
                              │
                              ▼ nats://127.0.0.1:4222
                            nats-server (leaf node, JetStream domain edge01)
                              stream EDGE_DETECT: events.detect.>, snapshots.>
                              │ leaf link, outbound only
                              ▼ 10.3.0.33:7422
                            NATS hub on nas01 k3s (namespace cvkit, domain hub)
                              stream DETECT  ◀── sources EDGE_DETECT from each edge
                              │
                              ▼ durable consumers (examples/consume.py)
```

**Why a leaf node with its own JetStream domain.** The edge box only dials
out, so it works behind NAT or a firewall. Events are written to disk on the
box first. When the link to the hub drops, nothing is lost: the hub's `DETECT`
stream sources `EDGE_DETECT` and catches up when the link returns. Live
subscribers on the hub still see each event the moment it is published.

## Events

`events.detect.<site>.<camera>`, JSON, schema `cvkit.detect.v1`:

```json
{
  "schema": "cvkit.detect.v1", "event_id": "4f0c…", "kind": "start",
  "site": "home", "camera": "driveway", "edge": "cvkit-edge-01", "model": "yolov8s_h8l",
  "ts": "2026-11-24T01:02:30.274+00:00", "first_seen": "…", "last_seen": "…", "duration_s": 0.4,
  "track_id": 1, "label": "person", "confidence": 0.89, "max_confidence": 0.89,
  "bbox": [0.41, 0.22, 0.52, 0.71], "frames": 5,
  "snapshot": {"subject": "snapshots.home.driveway.4f0c…", "bytes": 61234}
}
```

- `start` is sent once a track of a label in `events.labels` has persisted for
  `min_frames` frames above `min_confidence`; it carries a JPEG snapshot with
  the box drawn, on its own subject `snapshots.<site>.<camera>.<event_id>`.
- `end` is sent when that track has been unseen for `lost_after_s`, with the
  duration and best confidence. Start and end share `event_id`.
- `bbox` is normalised `[x0, y0, x1, y1]` on the original frame.
- Every message carries `Nats-Msg-Id`, so retries never duplicate.

`events.motion.<site>.<camera>` (schema `cvkit.motion.v1`) records each Tier 0
gate `open`/`close`, with the z-score, floor and open duration. These are
useful on their own, for example to cut clips without running any model.

`edge.status.<site>.<edge>` (not stored) is a heartbeat every 30 s with
per-camera Tier 0 fps, Tier 1 fps, gate state and duty, frame and event
counts, restarts and the last error.

## Install on an edge box

Prerequisites (already done on cvkit-edge-01): Raspberry Pi OS trixie,
`hailo-all`, kernel held at 6.12 (the Hailo dropped off PCIe on 6.18), and
`nats-server` configured as a leaf node of the hub. The leaf config is
`/etc/nats-server.conf` (root:nats 0640, it holds the leaf password from the
`nats-credentials` Secret in namespace `cvkit`):

```
server_name: cvkit-edge-01
listen: 127.0.0.1:4222
jetstream { domain: edge01, store_dir: /var/lib/nats-server, max_file_store: 8GB }
leafnodes { remotes = [ { urls: ["nats-leaf://leaf:<password>@10.3.0.33:7422"] } ] }
```

Then, from the repo:

```sh
components/detect/deploy/sync.sh pi@10.3.0.51   # detect + the emd inspector source
ssh pi@10.3.0.51 'sudo bash cvkit-detect/deploy/install.sh'
```

This creates the `cvkit` user, a venv at `/opt/cvkit/venv` (using the OS
GStreamer, Hailo and OpenCV bindings), a sample clip, `/etc/cvkit/detect.toml`
from `config.example.toml` (only if absent: it holds camera credentials), and
the `cvkit-detect` systemd unit. Logs: `journalctl -u cvkit-detect -f`.

Once per new edge box, add its domain to the hub stream:

```sh
NATS_URL=nats://127.0.0.1:4222 bash deploy/hub-streams.sh edge01 [edge02 …]
```

## Throughput

Measured on cvkit-edge-01 against AU01 Axis sub-streams (26 Nov 2026):

- Hailo-8L, YOLOv8s 640×640: about 59 fps on the chip. Running YOLO on every
  frame, that is about 5 cameras at 10 fps.
- Gated (this design): Tier 0 handled 80 streams on one Pi, peaking at 800
  frames/s, with about 10% CPU and about 1 GB RSS (the shared Tier 1 pool keeps
  memory flat). The cameras ran out first: each Axis serves about 110–120
  frames/s across all its sessions.
- The edge ceiling is therefore the Hailo's share of active time: about
  59 / (10 fps × average gate duty). That is about 35 cameras at 17% duty (a
  hall with a worker active) and over 100 at 5%. Bursts beyond `slots`
  simultaneously active cameras queue and rotate every `slot_max_s`.
- Prefer H.265 sub-streams when Tier 1 decode load matters (hardware decode on
  the Pi 5); Tier 0 doesn't decode at all.

## Tests

```sh
cd components/detect && python -m pytest
```

`tests/` covers the event state machine (including the movement filter), the
letterbox maths and the Tier 0 gate. The gate tests compile the real
`emd_inspector.c` with gcc. The pipeline itself is tested on the box against
the sample clip and live cameras.
