#!/usr/bin/env python3
"""Vertical-scroll coverage check for the port's tile renderer.

    make cover            (part of `make check`)

The renderer walks a window of tiles around the camera:

    trows = (DD2_H + (camy & 15) + 15) / 16      # 13 rows while the sub-tile
    tcols = (DD2_W + (camx & 15) + 15) / 16      # offset is small, 14 otherwise

With the old fixed bounds `DD2_H/16 + 1` (= 13 rows) the framebuffer was left
unpainted for the bottom `(camy & 15) - 8` pixel rows, which is the black
stripe that flickered on every vertical scroll step (the red band under it is
the level's own ground graphics, see re/audit/FIXES.md).

The frames come from a scripted `dd2 --record` run; this script fails when any
level frame has a full-width black band at the bottom.  Frames of the level-map
/ loading screens are skipped: they legitimately have black rows of their own
and are recognised by their mostly black top row (measured 0.00..0.21 non-black
against 0.97..0.99 for the level's sky row).

usage: cover.py <record-dir>
"""
import glob
import struct
import sys


def load_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<i', d, 10)[0]
    w = struct.unpack_from('<i', d, 18)[0]
    h = abs(struct.unpack_from('<i', d, 22)[0])
    row = (w * 3 + 3) // 4 * 4
    return w, h, [d[off + y * row: off + y * row + w * 3] for y in range(h)]


def black_row(pxrow):
    return all(pxrow[x * 3:x * 3 + 3] == b'\0\0\0' for x in range(len(pxrow) // 3))


def lit_frac(pxrow):
    w = len(pxrow) // 3
    return sum(1 for x in range(w) if pxrow[x * 3:x * 3 + 3] != b'\0\0\0') / w


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    files = sorted(glob.glob(sys.argv[1] + '/port_*.bmp'))
    if not files:
        print('cover: no frames in %s' % sys.argv[1])
        return 2

    checked = 0
    bad = []
    for i, p in enumerate(files):
        w, h, rows = load_bmp(p)
        if lit_frac(rows[0]) < 0.5:
            continue                      # loading / level-map screen
        checked += 1
        band = 0
        for y in range(h - 1, -1, -1):
            if black_row(rows[y]):
                band += 1
            else:
                break
        if band:
            bad.append((i, band))

    print('cover: %d frames, %d level frames checked' % (len(files), checked))
    if bad:
        print('cover: FAIL - black stripe at the bottom in %d frame(s)' % len(bad))
        print('       first: ' + ', '.join('frame %d: %d row(s)' % b for b in bad[:8]))
        return 1
    print('cover: ok - every level frame paints the full window')
    return 0


sys.exit(main())
