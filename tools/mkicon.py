#!/usr/bin/env python3
"""Build the application icon files from the raw sprite dump produced by
tools/mkicon.c (icon/dd2.rgba: u16 width, u16 height, then RGBA8888 rows).

Outputs (port/icon/):
    dd2_icon_16.png .. dd2_icon_256.png   nearest-neighbour scales of the art
    dd2.ico                               multi-size Windows icon
    dd2_icon_256.png                      master PNG
Nothing is drawn by hand: every pixel comes from the game's own sprite data.
"""
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
SRC = os.path.join(PORT, "icon", "dd2.rgba")
OUT = os.path.join(PORT, "icon")

SIZES = (16, 24, 32, 48, 64, 128, 256)


def main():
    raw = open(SRC, "rb").read()
    w, h = struct.unpack_from("<HH", raw, 0)
    img = Image.frombytes("RGBA", (w, h), raw[4:4 + w * h * 4])
    os.makedirs(OUT, exist_ok=True)

    makers = {}
    for s in SIZES:
        big = img.resize((s, s), Image.NEAREST)
        big.save(os.path.join(OUT, "dd2_icon_%d.png" % s))
        makers[s] = big
    makers[256].save(os.path.join(OUT, "dd2.ico"),
                     sizes=[(s, s) for s in SIZES])
    print("wrote %s/dd2.ico and %d PNG sizes from a %dx%d sprite"
          % (OUT, len(SIZES), w, h))


if __name__ == "__main__":
    sys.exit(main())
