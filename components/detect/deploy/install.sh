#!/usr/bin/env bash
# Install or update cvkit-detect on an edge box (Raspberry Pi 5 + Hailo, Debian trixie).
#
#   components/detect/deploy/sync.sh pi@<edge>     # copies detect + the emd inspector source
#   ssh pi@<edge> 'sudo bash cvkit-detect/deploy/install.sh'
#
# NO_RESTART=1 installs without (re)starting the service.
#
# Idempotent. Never overwrites an existing /etc/cvkit/detect.toml (it holds
# camera credentials). Assumes hailo-all and nats-server are installed and
# nats-server is configured as a leaf node (see README.md).
set -euo pipefail
SRC="$(cd "$(dirname "$0")/.." && pwd)"

DEBIAN_FRONTEND=noninteractive apt-get install -y -q python3-venv python3-opencv gstreamer1.0-libav gcc libc6-dev >/dev/null

id cvkit >/dev/null 2>&1 || useradd --system --home-dir /var/lib/cvkit --shell /usr/sbin/nologin cvkit
install -d -o cvkit -g cvkit -m 0750 /var/lib/cvkit /var/lib/cvkit/samples
install -d -o root -g cvkit -m 0750 /etc/cvkit

# Sample clip for testing without a camera (Intel IoT DevKit sample videos, CC-BY 4.0).
SAMPLE=/var/lib/cvkit/samples/person-bicycle-car-detection.mp4
if [ ! -s "$SAMPLE" ]; then
  curl -fsSL -o "$SAMPLE.part" \
    https://github.com/intel-iot-devkit/sample-videos/raw/master/person-bicycle-car-detection.mp4
  mv "$SAMPLE.part" "$SAMPLE"; chown cvkit:cvkit "$SAMPLE"
fi

# Tier 0 motion inspector: cvkit's emd_inspector.c, built as a shared library.
install -d -m 0755 /opt/cvkit/lib
gcc -O2 -shared -fPIC -I "$SRC/native/include" "$SRC/native/emd_inspector.c" -lm \
  -o /opt/cvkit/lib/libemd_inspector.so.new
mv /opt/cvkit/lib/libemd_inspector.so.new /opt/cvkit/lib/libemd_inspector.so

[ -d /opt/cvkit/venv ] || python3 -m venv --system-site-packages /opt/cvkit/venv
# Build from a scratch copy so root-owned build/ and egg-info never land in $SRC.
BUILD=$(mktemp -d); trap 'rm -rf "$BUILD"' EXIT
cp -r "$SRC/pyproject.toml" "$SRC/cvkit_detect" "$BUILD/"
/opt/cvkit/venv/bin/pip install -q --upgrade "$BUILD"

if [ ! -f /etc/cvkit/detect.toml ]; then
  install -o root -g cvkit -m 0640 "$SRC/config.example.toml" /etc/cvkit/detect.toml
  echo "wrote /etc/cvkit/detect.toml from the example; edit site and cameras"
fi

install -m 0644 "$SRC/deploy/cvkit-detect.service" /etc/systemd/system/cvkit-detect.service
systemctl daemon-reload
systemctl enable cvkit-detect >/dev/null 2>&1
[ "${NO_RESTART:-}" = 1 ] && exit 0
systemctl restart cvkit-detect
sleep 2
systemctl --no-pager --lines=0 status cvkit-detect | head -3
