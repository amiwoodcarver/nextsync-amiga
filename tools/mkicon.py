#!/usr/bin/env python3
"""
Generate Workbench icons (.info) for NextSync.

    tools/mkicon.py NextSync.info                 tool icon (an executable)
    tools/mkicon.py --drawer NextSync.info        drawer icon
    tools/mkicon.py --project NextSync.guide.info project icon
    tools/mkicon.py -                             print the art as text

    --stack N        stack for a tool icon, default 100000
    --tool NAME      default tool for a project icon,
                     default SYS:Utilities/MultiView

Writes an OS 2.0 style DiskObject: a 78 byte header, the DrawerData a
drawer needs, one Image header and one chunk of planar image data, then the
default tool string for a project.

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


def draw_drawer():
    """A drawer with a cloud on the front."""
    px = blank()

    # the tab, then the body
    box(px, 6, 3, 24, 7, WHITE)
    box(px, 4, 6, 51, 21, WHITE)
    px = outline(px, WHITE, BLACK)

    # a small cloud, the same shape as the tool icon wears
    disc(px, 22, 14, 4, BLUE)
    disc(px, 28, 12, 5, BLUE)
    disc(px, 34, 14, 4, BLUE)
    box(px, 20, 14, 36, 17, BLUE)
    return px


def draw_document():
    """A page with a folded corner and a few lines of text."""
    px = blank()

    box(px, 14, 1, 41, 22, WHITE)
    for i in range(7):                       # the fold, top right
        box(px, 41 - i, 1 + i, 41, 1 + i, BG)
    px = outline(px, WHITE, BLACK)

    for i in range(4):                       # lines of writing, below the fold
        box(px, 18, 10 + i * 3, 37, 10 + i * 3, BLUE)
    return px


def draw_cloud():
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


DRAWINGS = {'tool': draw_cloud, 'drawer': draw_drawer, 'project': draw_document}


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


WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4


def diskobject(kind, stack, has_tool):
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
                        kind,
                        0,      # padding
                        1 if has_tool else 0,   # do_DefaultTool follows
                        0,      # do_ToolTypes
                        NO_ICON_POSITION, NO_ICON_POSITION,
                        1 if kind == WBDRAWER else 0,   # do_DrawerData
                        0,      # do_ToolWindow
                        stack if kind == WBTOOL else 0))


def drawerdata():
    """
    The window Workbench opens when the drawer is double clicked: a
    NewWindow followed by the scroll offsets, 56 bytes in all.
    """
    newwindow = struct.pack(
        '>hhhhBBIIIIIIIhhHHH',
        50, 40, 300, 120,   # LeftEdge, TopEdge, Width, Height
        255, 255,           # DetailPen, BlockPen: use the defaults
        0,                  # IDCMPFlags
        0,                  # Flags
        0,                  # FirstGadget
        0,                  # CheckMark
        0,                  # Title
        0,                  # Screen
        0,                  # BitMap
        90, 40,             # MinWidth, MinHeight
        0xFFFF, 0xFFFF,     # MaxWidth, MaxHeight
        1)                  # Type: WBENCHSCREEN
    return newwindow + struct.pack('>ii', 0, 0)   # dd_CurrentX, dd_CurrentY


def bcpl_string(s):
    """Length prefixed and NUL terminated, the way icon.library stores it."""
    data = s.encode('latin-1') + b'\0'
    return struct.pack('>I', len(data)) + data


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
    kind, stack, tool, path = 'tool', 100000, 'SYS:Utilities/MultiView', None
    args = sys.argv[1:]

    while args:
        a = args.pop(0)
        if a == '--drawer':
            kind = 'drawer'
        elif a == '--project':
            kind = 'project'
        elif a == '--stack':
            stack = int(args.pop(0))
        elif a == '--tool':
            tool = args.pop(0)
        elif a.startswith('-') and a != '-':
            sys.exit("mkicon.py: unknown option %s" % a)
        else:
            path = a

    if not path:
        sys.exit(__doc__.strip())

    px = DRAWINGS[kind]()
    if path == '-':
        print(preview(px))
        return

    wbtype = {'tool': WBTOOL, 'drawer': WBDRAWER, 'project': WBPROJECT}[kind]
    has_tool = (kind == 'project')

    with open(path, 'wb') as f:
        f.write(diskobject(wbtype, stack, has_tool))
        if wbtype == WBDRAWER:
            f.write(drawerdata())
        f.write(image_header())
        f.write(planes(px))
        if has_tool:
            f.write(bcpl_string(tool))


if __name__ == '__main__':
    main()
