#!/usr/bin/env python3
"""Measure whether a captured frame is a real rendered scene or a flat fill -- stdlib only.

Capture assertions that compare PNG hashes tell you a frame CHANGED; they cannot tell you the frame was
ever an image. A backend that silently returns a cleared surface still produces stable, differing hashes
if anything at all varies. `scripts/integration_hello_xr.mjs` has carried a distinctColors check for
this reason on the node side; this is the Python-side equivalent so scenario/E2E harnesses can measure
it too, instead of using file size as a proxy (file size cannot separate "small flat image" from
"large simple scene" -- that mistake sent one VRAppDummyGame run chasing a non-existent bug).

Usage: python tools/playspectra_png_stats.py <file.png> [more.png ...]
"""
import json
import os
import struct
import sys
import zlib


def png_stats(path, max_rows=None, cols_sampled=200, sample_rows=300):
    """Decode a PNG far enough to characterise its pixels.

    Every row is unfiltered -- PNG rows must be decoded in order, since a row's filter may reference the
    previous one -- but colours are harvested from `sample_rows` rows spread across the WHOLE frame.
    Sampling only the top band is NOT equivalent, and gets this exact check wrong: a correctly rendered
    1080x1200 capture whose upper quarter is a plain dark ceiling reported distinctColors=1 from its
    first 300 rows, while the full frame had 2926. A rendered scene looked like a flat fill.

    max_rows optionally caps the unfilter loop for pathologically tall frames; leave it None to read the
    whole image. Returns a dict; on an unsupported encoding the geometry fields are still filled in.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return {"error": "not a PNG", "bytes": len(data)}
    pos, idat, w, h, bd, ct = 8, b"", 0, 0, 0, 0
    while pos + 8 <= len(data):
        ln = int.from_bytes(data[pos:pos + 4], "big")
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
    info = {"w": w, "h": h, "bitdepth": bd, "colortype": ct, "bytes": len(data)}
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ct)
    if bd != 8 or ch is None or not idat:
        info["note"] = "unsupported encoding for pixel stats"
        return info
    raw = zlib.decompress(idat)
    stride = w * ch
    prev = bytearray(stride)
    colors = {}
    step = max(1, w // max(1, cols_sampled))
    rows = min(h, max_rows) if max_rows else h
    row_step = max(1, rows // max(1, sample_rows))
    info["rowsDecoded"] = rows
    info["rowStep"] = row_step
    p = 0
    for _y in range(rows):
        if p >= len(raw):
            break
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        # Harvest from every row_step'th row so the sample spans the full height, not just the top.
        # Read min(3, ch) bytes, not a fixed 3: for grayscale/palette (ch < 3) a fixed width would run
        # past the pixel and count a byte string spanning neighbouring pixels as if it were one colour.
        if _y % row_step == 0:
            px = min(3, ch)
            for x in range(0, w, step):
                key = bytes(line[x * ch:x * ch + px])
                colors[key] = colors.get(key, 0) + 1
        prev = line
    total = sum(colors.values()) or 1
    info["distinctColors"] = len(colors)
    info["dominantFraction"] = round(max(colors.values()) / total, 4)
    info["top5"] = [(k.hex(), round(v / total, 3))
                    for k, v in sorted(colors.items(), key=lambda kv: -kv[1])[:5]]
    return info


def is_non_degenerate(stats, min_colors=3, max_dominant=0.999):
    """The same bar integration_hello_xr.mjs applies: more than a couple of colours, and no single
    colour covering essentially the whole frame."""
    return (stats.get("distinctColors", 0) >= min_colors
            and stats.get("dominantFraction", 1.0) < max_dominant)


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    for path in sys.argv[1:]:
        if not os.path.exists(path):
            print("%s: missing" % path)
            continue
        st = png_stats(path)
        print("%s\n  %s\n  non-degenerate: %s" % (path, json.dumps(st), is_non_degenerate(st)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
