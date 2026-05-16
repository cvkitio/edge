#!/usr/bin/env python3
"""
generate_config.py - Generate emd-agent config from discovered cameras

Usage:
    ./discover_axis_cameras.sh cameras.txt root password > cameras.json
    ./generate_config.py cameras.json password > agent.toml
"""

import json
import sys
from urllib.parse import quote

def generate_toml(cameras_json, password):
    with open(cameras_json) as f:
        data = json.load(f)

    cameras = data['cameras']
    password_enc = quote(password, safe='')

    print("""# emd-agent configuration - Auto-generated
# Generated from Axis camera discovery

[agent]
mode              = "single"
instance_id       = "au01-0"
data_dir          = "/var/lib/emd-agent"
sd_notify         = false

[publisher]
default           = "nats"
parallel_mqtt     = true

[publisher.nats]
url               = "nats://nats.default.svc.cluster.local:4222"
creds_file        = "/etc/emd-agent/nats.creds"

[publisher.mqtt]
url               = "mqtt://mosquitto.default.svc.cluster.local:1883"
client_id_prefix  = "emd-au01-0"
qos               = 1

[storage.primary]
backend           = "s3"
endpoint          = "http://minio.default.svc.cluster.local:9000"
region            = "us-west-2"
bucket            = "emd-fragments"
credentials       = "instance"

[outbox]
path              = "/var/lib/emd-agent/outbox.db"
max_bytes         = 1_073_741_824

[runtime]
log_level         = "info"
clip_root         = "/var/lib/emd-agent/clips"
inflight_root     = "/var/lib/emd-agent/inflight"

[recording]
container         = "mpegts"
fsync_policy      = "on_close"

[disk]
max_bytes_per_camera = 2_000_000_000
retention_days       = 7
""")

    # Generate camera entries
    for idx, cam in enumerate(cameras):
        ip = cam['ip']
        path = cam['path']
        codec = cam['codec']
        width = cam['width']
        height = cam['height']
        fps = cam['fps']

        # Sanitize camera name
        cam_name = f"axis_{ip.replace('.', '_')}_{idx}"

        # Determine codec hint
        codec_hint = "h264" if codec == "h264" else "h265" if codec == "h265" else "auto"

        # Estimate bitrate based on resolution and FPS
        pixels = width * height
        bitrate = int(pixels * float(fps if fps != "unknown" else 15) * 0.07)  # ~0.07 bits per pixel

        # Calculate off_threshold based on FPS (1.5 seconds of quiet)
        if fps != "unknown":
            off_thresh = max(10, int(float(fps) * 1.5))
        else:
            off_thresh = 15

        print(f"""
[cameras.{cam_name}]
url               = "rtsp://root:{password_enc}@{ip}{path}"
transport         = "tcp"
codec_hint        = "{codec_hint}"
buffer_seconds    = 20
pre_roll_seconds  = 6
post_roll_seconds = 10
clip_max_seconds  = 120
motion_z_high     = 3.0
intra_ratio_high  = 2.5
on_threshold      = 2
off_threshold     = {off_thresh}
gradual_enabled   = false
max_bitrate_bps   = {bitrate}
""")

    print(f"\n# Total cameras: {len(cameras)}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: generate_config.py <cameras.json> <password>", file=sys.stderr)
        print("", file=sys.stderr)
        print("Example:", file=sys.stderr)
        print("  ./generate_config.py cameras.json '***REDACTED_PASSWORD***' > agent.toml", file=sys.stderr)
        sys.exit(1)

    generate_toml(sys.argv[1], sys.argv[2])
