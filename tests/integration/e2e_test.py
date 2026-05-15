#!/usr/bin/env python3
"""
e2e_test.py — End-to-end test orchestrator for emd.

Starts a fake RTSP server and the emd binary, then validates that clip files
are produced within a timeout.

Usage:
    python3 e2e_test.py [options]

Options:
    --scenario  static|motion|gradual  (default: motion)
    --container mpegts|fmp4            (default: mpegts)
    --binary    PATH                   path to emd binary
    --fixtures-dir PATH                directory containing .h264 fixtures
    --timeout   SECONDS                max seconds to wait for a clip (default: 90)
    --no-cleanup                       leave temp dirs after the test

Exit codes:
    0 — PASS
    1 — FAIL
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

THIS_DIR = Path(__file__).parent.resolve()
REPO_ROOT = THIS_DIR.parent.parent  # tests/integration/../../ = repo root

FIXTURE_MAP = {
    "static":  "static_1080p30.h264",
    "motion":  "motion_1080p30.h264",
    "gradual": "gradual_1080p30.h264",
}

EXTENSION_MAP = {
    "mpegts": ".ts",
    "fmp4":   ".mp4",
}

RTSP_PORT = 8554
MQTT_DUMMY_URL = "mqtt://127.0.0.1:11883"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def fail(msg: str, rtsp_proc=None, emd_proc=None) -> None:
    """Print FAIL message, clean up processes, and exit 1."""
    print(f"\nFAIL: {msg}", flush=True)
    _kill(rtsp_proc)
    _kill(emd_proc)
    sys.exit(1)


def _kill(proc: Optional[subprocess.Popen]) -> None:
    if proc is None:
        return
    try:
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            proc.kill()
        except ProcessLookupError:
            pass


def wait_for_port(host: str, port: int, timeout: float = 10.0) -> bool:
    """
    Try to connect to host:port until timeout seconds have elapsed.
    Returns True if connection succeeded, False on timeout.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=0.5)
            s.close()
            return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            time.sleep(0.25)
    return False


def find_clips(clip_root: Path, ext: str) -> list:
    """Recursively find all files with the given extension under clip_root."""
    return list(clip_root.rglob(f"*{ext}"))


def run_ffprobe(clip_path: Path) -> Optional[dict]:
    """
    Run ffprobe on the clip and return the parsed JSON output.
    Returns None if ffprobe is not available or fails.
    """
    if not shutil.which("ffprobe"):
        return None
    cmd = [
        "ffprobe",
        "-v", "quiet",
        "-print_format", "json",
        "-show_streams",
        str(clip_path),
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, timeout=30)
        if result.returncode != 0:
            return None
        return json.loads(result.stdout)
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError):
        return None


def write_emd_config(tmp_dir: Path, clip_root: Path, inflight_root: Path,
                     container: str) -> Path:
    """Write a temporary emd TOML config and return its path."""
    config_path = tmp_dir / "emd.toml"
    content = f"""\
[runtime]
log_level       = "debug"
metrics_listen  = "127.0.0.1:9464"
clip_root       = "{clip_root}"
inflight_root   = "{inflight_root}"

[recording]
container       = "{container}"
muxer           = "intree"
fsync_policy    = "never"

[mqtt]
url             = "{MQTT_DUMMY_URL}"
client_id_prefix = "emd-e2e"

[cameras.cam0]
url               = "rtsp://127.0.0.1:{RTSP_PORT}/stream"
transport         = "tcp"
codec_hint        = "h264"
buffer_seconds    = 12
pre_roll_seconds  = 4
post_roll_seconds = 6
clip_max_seconds  = 60
motion_z_high     = 2.5
intra_ratio_high  = 2.0
"""
    config_path.write_text(content)
    return config_path


def ensure_fixtures(fixtures_dir: Path, scenario: str) -> Path:
    """
    Return the path to the fixture file, generating it if necessary.
    """
    filename = FIXTURE_MAP[scenario]
    fixture_path = fixtures_dir / filename
    if fixture_path.exists() and fixture_path.stat().st_size > 0:
        return fixture_path

    print(f"  Fixture not found: {fixture_path}", flush=True)
    print("  Running generate_fixtures.sh ...", flush=True)
    gen_script = REPO_ROOT / "scripts" / "generate_fixtures.sh"
    if not gen_script.exists():
        raise FileNotFoundError(f"generate_fixtures.sh not found at {gen_script}")

    result = subprocess.run(["bash", str(gen_script)], check=False)
    if result.returncode != 0:
        raise RuntimeError("generate_fixtures.sh failed")

    if not fixture_path.exists() or fixture_path.stat().st_size == 0:
        raise FileNotFoundError(
            f"Fixture still missing after generation: {fixture_path}")

    return fixture_path


# ---------------------------------------------------------------------------
# Main test logic
# ---------------------------------------------------------------------------

def run_test(args: argparse.Namespace) -> None:
    scenario: str = args.scenario
    container: str = args.container
    clip_ext: str = EXTENSION_MAP[container]
    timeout: int = args.timeout

    # ------------------------------------------------------------------
    # 1. Locate emd binary
    # ------------------------------------------------------------------
    if args.binary:
        emd_bin = Path(args.binary).resolve()
    else:
        # Default: ../../build/emd relative to this file
        emd_bin = (THIS_DIR / ".." / ".." / "build" / "emd").resolve()

    if not emd_bin.exists():
        fail(f"emd binary not found at {emd_bin}. Build it first: "
             f"cmake -S . -B build && cmake --build build")

    print(f"Using emd binary: {emd_bin}", flush=True)

    # ------------------------------------------------------------------
    # 2. Locate fixture
    # ------------------------------------------------------------------
    if args.fixtures_dir:
        fixtures_dir = Path(args.fixtures_dir).resolve()
    else:
        fixtures_dir = (REPO_ROOT / "tests" / "fixtures" / "streams").resolve()

    print(f"Fixtures directory: {fixtures_dir}", flush=True)
    try:
        fixture_path = ensure_fixtures(fixtures_dir, scenario)
    except (FileNotFoundError, RuntimeError) as exc:
        fail(str(exc))

    print(f"Using fixture: {fixture_path}", flush=True)

    # ------------------------------------------------------------------
    # 3. Create temp directories
    # ------------------------------------------------------------------
    tmp_root = Path(tempfile.mkdtemp(prefix="emd_e2e_"))
    clip_root = tmp_root / "clips"
    inflight_root = tmp_root / "inflight"
    clip_root.mkdir()
    inflight_root.mkdir()

    def cleanup() -> None:
        if not args.no_cleanup:
            shutil.rmtree(tmp_root, ignore_errors=True)

    # ------------------------------------------------------------------
    # 4. Write emd config
    # ------------------------------------------------------------------
    config_path = write_emd_config(tmp_root, clip_root, inflight_root, container)
    print(f"Config written: {config_path}", flush=True)

    # ------------------------------------------------------------------
    # 5. Start fake RTSP server
    # ------------------------------------------------------------------
    fake_rtsp_script = THIS_DIR / "fake_rtsp_server.py"
    if not fake_rtsp_script.exists():
        cleanup()
        fail(f"fake_rtsp_server.py not found at {fake_rtsp_script}")

    rtsp_cmd = [
        sys.executable,
        str(fake_rtsp_script),
        "--stream", str(fixture_path),
        "--port", str(RTSP_PORT),
        "--fps", "30",
    ]
    print(f"\nStarting RTSP server: {' '.join(rtsp_cmd)}", flush=True)
    rtsp_proc = subprocess.Popen(
        rtsp_cmd,
        stderr=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
    )

    # Wait for the RTSP server to be ready
    print(f"  Waiting for RTSP server on port {RTSP_PORT}...", flush=True)
    if not wait_for_port("127.0.0.1", RTSP_PORT, timeout=10.0):
        _kill(rtsp_proc)
        cleanup()
        fail(f"RTSP server did not start within 10 s (port {RTSP_PORT})")

    if rtsp_proc.poll() is not None:
        stderr_out = rtsp_proc.stderr.read().decode("utf-8", errors="replace")
        cleanup()
        fail(f"RTSP server exited early (rc={rtsp_proc.returncode}):\n{stderr_out}")

    print("  RTSP server ready.", flush=True)

    # ------------------------------------------------------------------
    # 6. Start emd
    # ------------------------------------------------------------------
    emd_cmd = [str(emd_bin), "-c", str(config_path)]
    print(f"\nStarting emd: {' '.join(emd_cmd)}", flush=True)
    emd_proc = subprocess.Popen(
        emd_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    # Give emd a moment to start and potentially fail fast
    time.sleep(1.0)
    if emd_proc.poll() is not None:
        stdout_out = emd_proc.stdout.read().decode("utf-8", errors="replace")
        _kill(rtsp_proc)
        cleanup()
        fail(f"emd exited immediately (rc={emd_proc.returncode}):\n{stdout_out}")

    print("  emd started.", flush=True)

    # ------------------------------------------------------------------
    # 7. Poll for clip files up to timeout
    # ------------------------------------------------------------------
    print(f"\nWaiting up to {timeout} s for clip files in {clip_root} ...",
          flush=True)

    clips_found: list = []
    deadline = time.monotonic() + timeout
    poll_interval = 1.0

    while time.monotonic() < deadline:
        # Check emd hasn't crashed
        if emd_proc.poll() is not None:
            stdout_out = emd_proc.stdout.read().decode("utf-8", errors="replace")
            _kill(rtsp_proc)
            cleanup()
            fail(f"emd crashed (rc={emd_proc.returncode}) before producing clips:\n"
                 f"{stdout_out[-2000:]}")

        clips_found = find_clips(clip_root, clip_ext)
        if clips_found:
            print(f"  Found {len(clips_found)} clip(s).", flush=True)
            break

        time.sleep(poll_interval)
    else:
        # Timeout
        stdout_out = ""
        try:
            emd_proc.send_signal(signal.SIGTERM)
            emd_proc.wait(timeout=3)
            stdout_out = emd_proc.stdout.read().decode("utf-8", errors="replace")
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
        _kill(rtsp_proc)
        cleanup()
        fail(f"No clips produced within {timeout} s.\n"
             f"emd output (last 2000 chars):\n{stdout_out[-2000:]}")

    # ------------------------------------------------------------------
    # 8. Validate clips
    # ------------------------------------------------------------------
    print("\nValidating clips ...", flush=True)

    for clip_path in clips_found:
        size = clip_path.stat().st_size
        print(f"  {clip_path.name}: {size} bytes", flush=True)

        # Check minimum size
        min_size = 10 * 1024  # 10 KB
        if size < min_size:
            _kill(rtsp_proc)
            _kill(emd_proc)
            cleanup()
            fail(f"Clip is suspiciously small ({size} bytes < {min_size}): "
                 f"{clip_path}")

        # ffprobe validation (optional)
        probe = run_ffprobe(clip_path)
        if probe is not None:
            streams = probe.get("streams", [])
            if not streams:
                _kill(rtsp_proc)
                _kill(emd_proc)
                cleanup()
                fail(f"ffprobe found no streams in {clip_path.name}")

            video_stream = next(
                (s for s in streams if s.get("codec_type") == "video"), None)
            if video_stream is None:
                _kill(rtsp_proc)
                _kill(emd_proc)
                cleanup()
                fail(f"ffprobe found no video stream in {clip_path.name}")

            codec = video_stream.get("codec_name", "")
            if codec != "h264":
                _kill(rtsp_proc)
                _kill(emd_proc)
                cleanup()
                fail(f"Expected codec h264, got '{codec}' in {clip_path.name}")

            duration_str = video_stream.get("duration", "0")
            try:
                duration = float(duration_str)
            except ValueError:
                duration = 0.0

            if duration <= 0:
                _kill(rtsp_proc)
                _kill(emd_proc)
                cleanup()
                fail(f"ffprobe reported non-positive duration ({duration_str}) "
                     f"in {clip_path.name}")

            print(f"    ffprobe: codec={codec} duration={duration:.2f}s OK",
                  flush=True)
        else:
            print("    ffprobe: not available — skipping codec/duration check",
                  flush=True)

    # ------------------------------------------------------------------
    # 9. Clean up processes and temp dirs
    # ------------------------------------------------------------------
    _kill(emd_proc)
    _kill(rtsp_proc)
    cleanup()

    # ------------------------------------------------------------------
    # Done
    # ------------------------------------------------------------------
    print(f"\nPASS: scenario={scenario} container={container} "
          f"clips={len(clips_found)}", flush=True)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="emd end-to-end test orchestrator")
    parser.add_argument(
        "--scenario", choices=["static", "motion", "gradual"],
        default="motion",
        help="Test scenario (default: motion)")
    parser.add_argument(
        "--container", choices=["mpegts", "fmp4"],
        default="mpegts",
        help="Clip container format (default: mpegts)")
    parser.add_argument(
        "--binary", default=None,
        help="Path to emd binary (default: ../../build/emd)")
    parser.add_argument(
        "--fixtures-dir", default=None,
        help="Directory containing .h264 fixture files "
             "(default: ../../tests/fixtures/streams)")
    parser.add_argument(
        "--timeout", type=int, default=90,
        help="Max seconds to wait for clips (default: 90)")
    parser.add_argument(
        "--no-cleanup", action="store_true",
        help="Leave temp directories after the test")

    args = parser.parse_args()
    run_test(args)


if __name__ == "__main__":
    main()
