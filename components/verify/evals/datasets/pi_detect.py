"""Run the Tier 1 detector over a folder of 640x640 letterboxed JPEGs on the edge Pi (Hailo-8L).

  scp -r seq/ pi@edge:/tmp/seq && scp pi_detect.py pi@edge:/tmp/
  ssh pi@edge '/opt/cvkit/venv/bin/python /tmp/pi_detect.py /tmp/seq /tmp/dets.json'

Same model, NMS score threshold and post-process as cvkit-detect (its Config defaults), through the HailoRT
service, so it shares the device with the running cvkit-detect. Frames are 00000.jpg, 00001.jpg, ... in
order. Output: {"model": ..., "frames": [[{"label", "confidence", "box": [x0, y0, x1, y1]}, ...], ...]} with
boxes normalised to the 640x640 input.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

import hailo  # noqa: E402
from cvkit_detect.config import Config  # noqa: E402


def main() -> None:
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    n = len(sorted(src.glob("*.jpg")))
    cfg = Config(site="bench", cameras=[])
    Gst.init(None)
    desc = (f'multifilesrc location="{src}/%05d.jpg" index=0 stop-index={n - 1} caps="image/jpeg,framerate=10/1" ! '
            "jpegdec ! videoconvert ! video/x-raw,format=RGB,width=640,height=640 ! "
            f'hailonet hef-path="{cfg.hef}" batch-size=1 vdevice-group-id=1 multi-process-service=true '
            f"nms-score-threshold={cfg.nms_score_threshold} ! "
            f'hailofilter so-path="{cfg.postprocess_so}" function-name={cfg.postprocess_fn} qos=false ! '
            "appsink name=sink emit-signals=true sync=false max-buffers=100 drop=false")
    p = Gst.parse_launch(desc)
    frames: list[list[dict]] = []

    def on_sample(sink):
        buf = sink.emit("pull-sample").get_buffer()
        dets = []
        for d in hailo.get_roi_from_buffer(buf).get_objects_typed(hailo.HAILO_DETECTION):
            b = d.get_bbox()
            dets.append({"label": d.get_label(), "confidence": round(d.get_confidence(), 4),
                         "box": [round(b.xmin(), 5), round(b.ymin(), 5), round(b.xmin() + b.width(), 5),
                                 round(b.ymin() + b.height(), 5)]})
        frames.append(dets)
        return Gst.FlowReturn.OK

    p.get_by_name("sink").connect("new-sample", on_sample)
    loop = GLib.MainLoop()
    bus = p.get_bus()
    bus.add_signal_watch()

    def on_msg(_, m):
        if m.type == Gst.MessageType.EOS:
            loop.quit()
        elif m.type == Gst.MessageType.ERROR:
            print("error:", m.parse_error(), file=sys.stderr)
            loop.quit()

    bus.connect("message", on_msg)
    p.set_state(Gst.State.PLAYING)
    loop.run()
    out.write_text(json.dumps({"model": Path(cfg.hef).name, "nms_score_threshold": cfg.nms_score_threshold,
                               "frames": frames}))
    print(f"{len(frames)}/{n} frames, {sum(map(len, frames))} detections -> {out}")
    sys.stdout.flush()
    import os
    os._exit(0 if len(frames) == n else 1)   # skip hailonet teardown (see cvkit_detect/__main__.py)


if __name__ == "__main__":
    main()
