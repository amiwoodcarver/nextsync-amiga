#!/usr/bin/env python3
"""Convert an .ags screen dump written by AGUI_Snapshot() into a PNG.

The .ags format is deliberately trivial so the Amiga side stays small:

    char  magic[4]   "AGSH"
    u16   version    1
    u16   width
    u16   height
    u16   colors
    u8    palette[colors][3]
    u8    pixels[height][width]

Everything is big endian. Only the standard library is used.
"""

import struct
import sys
import zlib


def read_ags(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"AGSH":
        raise ValueError(f"{path}: not an AGSH file")

    version, width, height, colors = struct.unpack_from(">HHHH", data, 4)
    if version != 1:
        raise ValueError(f"{path}: unsupported version {version}")

    off = 12
    palette = data[off:off + colors * 3]
    off += colors * 3

    need = width * height
    pixels = data[off:off + need]
    if len(pixels) < need:
        raise ValueError(
            f"{path}: truncated, expected {need} pixel bytes, got {len(pixels)}"
        )
    return width, height, colors, palette, pixels


def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_png(path, width, height, palette, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)                       # filter type: none
        raw += pixels[y * width:(y + 1) * width]

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", palette)
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")

    with open(path, "wb") as f:
        f.write(out)


def main():
    if len(sys.argv) < 2:
        print("usage: ags2png.py input.ags [output.png]", file=sys.stderr)
        return 2

    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"

    width, height, colors, palette, pixels = read_ags(src)
    write_png(dst, width, height, palette, pixels)
    print(f"{dst}: {width}x{height}, {colors} colours")
    return 0


if __name__ == "__main__":
    sys.exit(main())
