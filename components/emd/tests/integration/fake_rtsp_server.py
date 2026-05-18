#!/usr/bin/env python3
"""
fake_rtsp_server.py — Synthetic RTSP/RTP server for emd integration tests.

Serves a raw H.264 Annex-B file over TCP-interleaved RTP (RFC 2326 §10.12).
Implements the RTSP state machine required by the emd RTSP client:
    OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER (keepalive)

Usage:
    python3 fake_rtsp_server.py --stream path/to/file.h264 \
                                 --port 8554 \
                                 --fps 30 \
                                 [--no-loop]

After each IDR frame is sent, prints to stderr:
    FRAME: idr pts=NNN

This output is consumed by the E2E test harness to track stream progress.
"""

import argparse
import base64
import socket
import struct
import sys
import threading
import time
from typing import List, Tuple, Optional


# ---------------------------------------------------------------------------
# Annex-B parser
# ---------------------------------------------------------------------------

def _find_start_codes(data: bytes) -> List[int]:
    """Return byte offsets of every Annex-B start code (4-byte or 3-byte)."""
    offsets = []
    i = 0
    n = len(data)
    while i < n - 2:
        if data[i] == 0 and data[i + 1] == 0:
            if i + 3 < n and data[i + 2] == 0 and data[i + 3] == 1:
                offsets.append(i)
                i += 4
                continue
            if data[i + 2] == 1:
                offsets.append(i)
                i += 3
                continue
        i += 1
    return offsets


def _nal_start(data: bytes, offset: int) -> int:
    """Given offset of a start code, return the offset of the first NAL byte."""
    if data[offset + 2] == 1:
        return offset + 3   # 3-byte start code: 00 00 01
    return offset + 4       # 4-byte start code: 00 00 00 01


def parse_annexb(data: bytes) -> List[Tuple[int, bytes]]:
    """
    Parse raw Annex-B H.264 bitstream into NAL units.

    Returns a list of (nal_type, nal_bytes) where nal_bytes does NOT
    include the start code prefix.
    """
    offsets = _find_start_codes(data)
    if not offsets:
        return []

    nals: List[Tuple[int, bytes]] = []
    for idx, sc_offset in enumerate(offsets):
        nal_start = _nal_start(data, sc_offset)
        if idx + 1 < len(offsets):
            nal_end = offsets[idx + 1]
        else:
            nal_end = len(data)

        nal_bytes = data[nal_start:nal_end]
        # Strip trailing zero bytes that belong to the next start code
        while nal_bytes and nal_bytes[-1] == 0:
            nal_bytes = nal_bytes[:-1]

        if not nal_bytes:
            continue

        nal_type = nal_bytes[0] & 0x1F
        nals.append((nal_type, nal_bytes))

    return nals


# ---------------------------------------------------------------------------
# SDP builder
# ---------------------------------------------------------------------------

def build_sdp(sps_b64: str, pps_b64: str, fps: int, server_ip: str) -> str:
    """Build an SDP string for H.264 over RTP/AVP."""
    sprop = f"{sps_b64},{pps_b64}"
    sdp = (
        "v=0\r\n"
        f"o=- 0 0 IN IP4 {server_ip}\r\n"
        "s=emd-fake-rtsp\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=control:*\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        f"a=fmtp:96 packetization-mode=1;sprop-parameter-sets={sprop}\r\n"
        f"a=framerate:{fps}\r\n"
        "a=control:trackID=0\r\n"
    )
    return sdp


# ---------------------------------------------------------------------------
# RTP packet builder
# ---------------------------------------------------------------------------

def build_rtp(payload: bytes, seq: int, ts: int,
              ssrc: int = 0x12345678, pt: int = 96, marker: int = 0) -> bytes:
    """Build a minimal RTP packet (RFC 3550)."""
    # V=2, P=0, X=0, CC=0
    first_byte = 0x80
    second_byte = ((marker & 0x1) << 7) | (pt & 0x7F)
    header = struct.pack("!BBHII", first_byte, second_byte,
                         seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
    return header + payload


# ---------------------------------------------------------------------------
# TCP interleaved framing (RFC 2326 §10.12)
# ---------------------------------------------------------------------------

def build_interleaved(channel: int, rtp_packet: bytes) -> bytes:
    """Wrap an RTP packet in the RTSP interleaved binary frame."""
    return struct.pack("!BBH", 0x24, channel & 0xFF,
                       len(rtp_packet)) + rtp_packet


# ---------------------------------------------------------------------------
# RTP packetizer for H.264
# ---------------------------------------------------------------------------

MAX_RTP_PAYLOAD = 1400   # bytes, safe MTU headroom


def packetize_nal(nal: bytes, seq: int, ts: int,
                  marker: int) -> List[Tuple[int, bytes]]:
    """
    Packetize one NAL unit into RTP payloads.

    Returns list of (new_seq, rtp_packet) tuples.
    Uses single-NAL packets for NALs <= MAX_RTP_PAYLOAD,
    FU-A fragmentation for larger ones.
    """
    packets: List[Tuple[int, bytes]] = []

    if len(nal) <= MAX_RTP_PAYLOAD:
        # Single NAL unit packet
        pkt = build_rtp(nal, seq, ts, marker=marker)
        packets.append((seq + 1, pkt))
        return packets

    # FU-A fragmentation (RFC 6184 §5.8)
    nal_hdr = nal[0]
    nal_type = nal_hdr & 0x1F
    nal_ref_idc = nal_hdr & 0x60
    fu_indicator = nal_ref_idc | 28   # FU-A type = 28

    payload_data = nal[1:]  # skip original NAL header
    offset = 0
    is_first = True

    while offset < len(payload_data):
        chunk = payload_data[offset: offset + MAX_RTP_PAYLOAD - 2]
        offset += len(chunk)
        is_last = (offset >= len(payload_data))

        start_bit = 0x80 if is_first else 0x00
        end_bit = 0x40 if is_last else 0x00
        fu_header = start_bit | end_bit | (nal_type & 0x1F)

        fu_payload = bytes([fu_indicator, fu_header]) + chunk
        pkt_marker = marker if is_last else 0
        pkt = build_rtp(fu_payload, seq, ts, marker=pkt_marker)
        packets.append((seq + 1, pkt))
        seq += 1
        is_first = False

    return packets


# ---------------------------------------------------------------------------
# RTSP session handler (one per client connection)
# ---------------------------------------------------------------------------

CRLF = b"\r\n"


class RTSPSession:
    """
    Handles one RTSP client connection (TCP-interleaved RTP/AVP).

    State machine:
        INIT -> READY (after SETUP) -> PLAYING (after PLAY) -> DONE
    """

    def __init__(self, conn: socket.socket, addr,
                 nals: List[Tuple[int, bytes]],
                 fps: int, server_ip: str,
                 loop: bool):
        self._conn = conn
        self._addr = addr
        self._nals = nals
        self._fps = fps
        self._server_ip = server_ip
        self._loop = loop

        # Find SPS (type 7) and PPS (type 8) for SDP
        self._sps_b64 = ""
        self._pps_b64 = ""
        for nal_type, nal_bytes in nals:
            if nal_type == 7 and not self._sps_b64:
                self._sps_b64 = base64.b64encode(nal_bytes).decode("ascii")
            elif nal_type == 8 and not self._pps_b64:
                self._pps_b64 = base64.b64encode(nal_bytes).decode("ascii")

        self._sdp = build_sdp(self._sps_b64, self._pps_b64, fps, server_ip)

        self._session_id = "emd00000001"
        self._state = "INIT"   # INIT | READY | PLAYING | DONE
        self._stream_thread: Optional[threading.Thread] = None
        self._stop_streaming = threading.Event()

        self._seq = 0
        self._ssrc = 0x12345678
        self._send_lock = threading.Lock()

    # ------------------------------------------------------------------
    # Public entry point
    # ------------------------------------------------------------------

    def run(self) -> None:
        """Main loop: read RTSP requests and dispatch."""
        self._conn.settimeout(60.0)
        buf = b""
        try:
            while self._state != "DONE":
                try:
                    chunk = self._conn.recv(4096)
                except (socket.timeout, OSError):
                    break
                if not chunk:
                    break
                buf += chunk
                # Process complete RTSP messages (terminated by \r\n\r\n)
                while CRLF + CRLF in buf:
                    msg_end = buf.index(CRLF + CRLF) + 4
                    msg = buf[:msg_end]
                    buf = buf[msg_end:]
                    self._dispatch(msg.decode("utf-8", errors="replace"))
        finally:
            self._stop_streaming.set()
            if self._stream_thread and self._stream_thread.is_alive():
                self._stream_thread.join(timeout=3.0)
            try:
                self._conn.close()
            except OSError:
                pass

    # ------------------------------------------------------------------
    # Request dispatch
    # ------------------------------------------------------------------

    def _dispatch(self, raw: str) -> None:
        lines = raw.split("\r\n")
        if not lines:
            return
        parts = lines[0].split(" ")
        if len(parts) < 3:
            return
        method = parts[0].upper()
        cseq = self._extract_header(lines, "CSeq")

        if method == "OPTIONS":
            self._handle_options(cseq)
        elif method == "DESCRIBE":
            self._handle_describe(cseq)
        elif method == "SETUP":
            self._handle_setup(cseq)
        elif method == "PLAY":
            self._handle_play(cseq)
        elif method == "TEARDOWN":
            self._handle_teardown(cseq)
        elif method == "GET_PARAMETER":
            self._handle_get_parameter(cseq)
        else:
            self._send_response(405, "Method Not Allowed", cseq)

    @staticmethod
    def _extract_header(lines: List[str], name: str) -> str:
        name_lower = name.lower()
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                if k.strip().lower() == name_lower:
                    return v.strip()
        return ""

    # ------------------------------------------------------------------
    # RTSP method handlers
    # ------------------------------------------------------------------

    def _handle_options(self, cseq: str) -> None:
        extra = {"Public": "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}
        self._send_response(200, "OK", cseq, extra)

    def _handle_describe(self, cseq: str) -> None:
        sdp_bytes = self._sdp.encode("utf-8")
        extra = {
            "Content-Type": "application/sdp",
            "Content-Length": str(len(sdp_bytes)),
        }
        self._send_response(200, "OK", cseq, extra, body=sdp_bytes)

    def _handle_setup(self, cseq: str) -> None:
        # We only support TCP interleaved; accept any client Transport request
        extra = {
            "Transport": "RTP/AVP/TCP;unicast;interleaved=0-1",
            "Session": f"{self._session_id};timeout=60",
        }
        self._state = "READY"
        self._send_response(200, "OK", cseq, extra)

    def _handle_play(self, cseq: str) -> None:
        extra = {
            "Session": self._session_id,
            "RTP-Info": "url=rtsp://127.0.0.1/stream/trackID=0;seq=0;rtptime=0",
        }
        self._send_response(200, "OK", cseq, extra)
        self._state = "PLAYING"
        self._stop_streaming.clear()
        self._stream_thread = threading.Thread(
            target=self._stream_loop, daemon=True)
        self._stream_thread.start()

    def _handle_teardown(self, cseq: str) -> None:
        extra = {"Session": self._session_id}
        self._send_response(200, "OK", cseq, extra)
        self._state = "DONE"
        self._stop_streaming.set()

    def _handle_get_parameter(self, cseq: str) -> None:
        # Used as a keepalive by emd; just echo 200 OK
        extra = {"Session": self._session_id}
        self._send_response(200, "OK", cseq, extra)

    # ------------------------------------------------------------------
    # Response sender
    # ------------------------------------------------------------------

    def _send_response(self, code: int, reason: str, cseq: str,
                       headers: Optional[dict] = None,
                       body: Optional[bytes] = None) -> None:
        lines = [f"RTSP/1.0 {code} {reason}"]
        if cseq:
            lines.append(f"CSeq: {cseq}")
        lines.append("Server: emd-fake-rtsp/1.0")
        if headers:
            for k, v in headers.items():
                lines.append(f"{k}: {v}")
        if body:
            lines.append(f"Content-Length: {len(body)}")
        lines.append("")  # blank line
        lines.append("")
        response = "\r\n".join(lines).encode("utf-8")
        if body:
            response += body
        with self._send_lock:
            try:
                self._conn.sendall(response)
            except OSError:
                self._state = "DONE"

    # ------------------------------------------------------------------
    # RTP streaming loop
    # ------------------------------------------------------------------

    def _stream_loop(self) -> None:
        """
        Deliver NAL units as RTP packets paced at the configured frame rate.

        Groups NALs into access units by detecting IDR/non-IDR boundaries,
        then sleeps between access units to maintain the correct frame rate.
        """
        frame_interval = 1.0 / self._fps
        # 90 kHz RTP clock ticks per frame
        rtp_increment = int(90000 / self._fps)

        rtp_ts = 0
        pts_counter = 0

        nals = self._nals
        nal_idx = 0
        total = len(nals)

        while not self._stop_streaming.is_set():
            if nal_idx >= total:
                if self._loop:
                    nal_idx = 0
                    rtp_ts = 0
                    pts_counter = 0
                else:
                    break

            nal_type, nal_bytes = nals[nal_idx]
            nal_idx += 1

            # Determine if this is a frame boundary (IDR=5, non-IDR=1)
            # Param sets (SPS=7, PPS=8) are bundled with the next IDR
            is_frame = nal_type in (1, 5)  # non-IDR slice, IDR slice
            is_idr = (nal_type == 5)
            is_marker = is_frame  # RTP marker bit: set on last packet of AU

            # Packetize
            new_packets = packetize_nal(
                nal_bytes, self._seq, rtp_ts,
                marker=1 if is_marker else 0)

            for new_seq, pkt in new_packets:
                self._seq = new_seq
                frame = build_interleaved(0, pkt)
                with self._send_lock:
                    try:
                        self._conn.sendall(frame)
                    except OSError:
                        self._stop_streaming.set()
                        return

            if is_frame:
                if is_idr:
                    print(f"FRAME: idr pts={pts_counter}", file=sys.stderr,
                          flush=True)
                rtp_ts = (rtp_ts + rtp_increment) & 0xFFFFFFFF
                pts_counter += 1
                # Pace delivery
                self._stop_streaming.wait(timeout=frame_interval)


# ---------------------------------------------------------------------------
# TCP server
# ---------------------------------------------------------------------------

class FakeRTSPServer:
    def __init__(self, stream_path: str, port: int, fps: int, loop: bool):
        self._port = port
        self._fps = fps
        self._loop = loop

        print(f"Loading stream: {stream_path}", file=sys.stderr)
        with open(stream_path, "rb") as fh:
            raw = fh.read()
        self._nals = parse_annexb(raw)
        if not self._nals:
            print("ERROR: no NAL units found in stream file", file=sys.stderr)
            sys.exit(1)

        sps_count = sum(1 for t, _ in self._nals if t == 7)
        pps_count = sum(1 for t, _ in self._nals if t == 8)
        idr_count = sum(1 for t, _ in self._nals if t == 5)
        total = len(self._nals)
        print(f"Parsed {total} NAL units: {sps_count} SPS, {pps_count} PPS, "
              f"{idr_count} IDR", file=sys.stderr)

        if sps_count == 0 or pps_count == 0:
            print("WARNING: stream has no SPS or PPS — SDP will be empty",
                  file=sys.stderr)

    def serve_forever(self) -> None:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", self._port))
        srv.listen(8)
        srv.settimeout(1.0)

        # Resolve the local IP for SDP
        try:
            tmp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            tmp.connect(("8.8.8.8", 80))
            server_ip = tmp.getsockname()[0]
            tmp.close()
        except OSError:
            server_ip = "127.0.0.1"

        print(f"Listening on 0.0.0.0:{self._port} (server IP for SDP: {server_ip})",
              file=sys.stderr, flush=True)

        while True:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break

            print(f"Client connected: {addr}", file=sys.stderr)
            session = RTSPSession(
                conn, addr, self._nals, self._fps, server_ip, self._loop)
            t = threading.Thread(target=session.run, daemon=True)
            t.start()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Fake RTSP server for emd integration tests")
    parser.add_argument("--stream", required=True,
                        help="Path to raw H.264 Annex-B file")
    parser.add_argument("--port", type=int, default=8554,
                        help="TCP port to listen on (default: 8554)")
    parser.add_argument("--fps", type=int, default=30,
                        help="Frame rate for pacing (default: 30)")
    parser.add_argument("--no-loop", action="store_true",
                        help="Exit after stream ends instead of looping")
    args = parser.parse_args()

    server = FakeRTSPServer(
        stream_path=args.stream,
        port=args.port,
        fps=args.fps,
        loop=not args.no_loop,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Shutting down.", file=sys.stderr)


if __name__ == "__main__":
    main()
