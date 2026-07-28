#!/usr/bin/env python3
"""
Generate a Workbench tool icon (.info) for NextSync.

    tools/mkicon.py NextSync.info [stacksize]

Writes an OS 2.0 style DiskObject: a 78 byte header, one Image header and
one chunk of planar image data. No default tool and no tool types, which is
all a plain executable needs.

Drawn from geometry rather than stored as a blob so it stays readable and
editable in a source tree -- and so it needs nothing but the standard
library, like everything else on the host side here.

Colours are the four Workbench 3.1 defaults: 0 grey (background), 1 black,
2 white, 3 blue.
"""

import struct
import sys

W, H = 56, 24
BG, BLACK, WHITE, BLUE = 0, 1, 2, 3


def blank():
    return [[BG] * W for _ in range(H)]


def disc(px, cx, cy, r, colour):
    for y in range(H):
        for x in range(W):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                px[y][x] = colour


def box(px, x0, y0, x1, y1, colour):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            px[y][x] = colour


def outline(px, inside, colour):
    """Put `colour` on every background pixel touching an `inside` pixel."""
    out = [row[:] for row in px]
    for y in range(H):
        for x in range(W):
            if px[y][x] != BG:
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = y + dy, x + dx
                    if 0 <= ny < H and 0 <= nx < W and px[ny][nx] == inside:
                        out[y][x] = colour
    return out


def arrow(px, cx, top, colour, down):
    """A stubby arrow: a three pixel shaft and a nine pixel wide head."""
    if down:
        box(px, cx - 1, top, cx + 1, top + 6, colour)
        for i in range(5):                       # head, 9 wide down to 1
            box(px, cx - 4 + i, top + 7 + i, cx + 4 - i, top + 7 + i, colour)
    else:
        for i in range(5):                       # head, 1 wide down to 9
            box(px, cx - i, top + i, cx + i, top + i, colour)
        box(px, cx - 1, top + 5, cx + 1, top + 11, colour)


def draw():
    px = blank()

    # a cloud: three overlapping discs sitting on a flat base
    disc(px, 17, 12, 7, WHITE)
    disc(px, 28, 10, 9, WHITE)
    disc(px, 39, 12, 7, WHITE)
    box(px, 10, 12, 46, 19, WHITE)

    px = outline(px, WHITE, BLACK)

    # sync: one arrow each way
    arrow(px, 22, 6, BLUE, down=True)
    arrow(px, 34, 6, BLUE, down=False)
    return px


def planes(px, depth=2):
    """Amiga planar layout: whole plane 0 first, then whole plane 1."""
    words = (W + 15) // 16
    rowbytes = words * 2
    out = bytearray()
    for p in range(depth):
        for y in range(H):
            row = bytearray(rowbytes)
            for x in range(W):
                if (px[y][x] >> p) & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            out += row
    return bytes(out)


def diskobject(stack):
    NO_ICON_POSITION = -0x80000000   # 0x80000000, "place me automatically"

    gadget = struct.pack(
        '>IhhhhHHHIIIiIHI',
        0,          # NextGadget
        0, 0,       # LeftEdge, TopEdge
        W, H,       # Width, Height
        4,          # GFLG_GADGIMAGE, highlight by complementing
        1,          # GACT_RELVERIFY
        1,          # BOOLGADGET
        1,          # GadgetRender: non zero means "an image follows"
        0,          # SelectRender: none, see the highlight method above
        0,          # GadgetText
        0,          # MutualExclude
        0,          # SpecialInfo
        0,          # GadgetID
        1)          # UserData: icon revision

    return (struct.pack('>HH', 0xE310, 1) + gadget +
            struct.pack('>BBIIiiIII',
                        3,      # WBTOOL
                        0,      # padding
                        0,      # do_DefaultTool
                        0,      # do_ToolTypes
                        NO_ICON_POSITION, NO_ICON_POSITION,
                        0,      # do_DrawerData
                        0,      # do_ToolWindow
                        stack))


def image_header():
    return struct.pack('>hhhhhIBBI',
                       0, 0, W, H,
                       2,          # depth
                       1,          # ImageData: non zero, data follows
                       3,          # PlanePick: both planes
                       0,          # PlaneOnOff
                       0)          # NextImage


def preview(px):
    glyph = {BG: '.', BLACK: '#', WHITE: 'o', BLUE: '+'}
    return '\n'.join(''.join(glyph[c] for c in row) for row in px)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: mkicon.py <file.info> [stacksize]")
    path = sys.argv[1]
    stack = int(sys.argv[2]) if len(sys.argv) > 2 else 100000

    px = draw()
    if path == '-':
        print(preview(px))
        return

    with open(path, 'wb') as f:
        f.write(diskobject(stack))
        f.write(image_header())
        f.write(planes(px))


if __name__ == '__main__':
    main()
