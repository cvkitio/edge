from cvkit_detect.geometry import letterbox_geometry, unletterbox


def test_16x9_source_pads_top_and_bottom():
    px, py, cw, ch = letterbox_geometry((1920, 1080))
    assert (px, cw) == (0, 640) and round(ch) == 360 and round(py) == 140


def test_unletterbox_maps_content_edges_to_0_and_1():
    # Content occupies y in [140, 500] of the 640 square for a 16:9 source.
    x0, y0, x1, y1 = unletterbox((0.0, 140 / 640, 1.0, 500 / 640), (768, 432))
    assert (round(x0, 6), round(y0, 6), round(x1, 6), round(y1, 6)) == (0, 0, 1, 1)


def test_unletterbox_clamps_boxes_in_the_padding():
    assert unletterbox((0.1, 0.0, 0.2, 0.1), (1920, 1080))[1] == 0.0


def test_square_and_unknown_source_are_identity():
    box = (0.1, 0.2, 0.3, 0.4)
    assert unletterbox(box, (640, 640)) == box and unletterbox(box, None) == box
