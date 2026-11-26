"""Letterbox maths for the 640x640 network input. No Hailo/GStreamer imports, so it is unit-testable."""

from __future__ import annotations

NET_W = NET_H = 640


def letterbox_geometry(source_size: tuple[int, int] | None) -> tuple[float, float, float, float]:
    """(pad_x, pad_y, content_w, content_h) in network pixels for a source letterboxed into 640x640."""
    if not source_size:
        return 0.0, 0.0, float(NET_W), float(NET_H)
    sw, sh = source_size
    scale = min(NET_W / sw, NET_H / sh)
    cw, ch = sw * scale, sh * scale
    return (NET_W - cw) / 2, (NET_H - ch) / 2, cw, ch


def unletterbox(box: tuple[float, float, float, float],
                source_size: tuple[int, int] | None) -> tuple[float, float, float, float]:
    """Map a box normalised to the padded 640x640 input onto the original frame (normalised)."""
    px, py, cw, ch = letterbox_geometry(source_size)

    def clamp(v: float) -> float:
        return min(1.0, max(0.0, v))

    x0, y0, x1, y1 = box
    return (clamp((x0 * NET_W - px) / cw), clamp((y0 * NET_H - py) / ch),
            clamp((x1 * NET_W - px) / cw), clamp((y1 * NET_H - py) / ch))
