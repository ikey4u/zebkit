#!/usr/bin/env python3
"""Turn a snapshot tools/pageprobe wrote into a PNG file.

    raw-to-png.py SNAPSHOT PICTURE.png

The snapshot is a text line "width height stride" and then the pixels, four
bytes each, blue first. Written with the standard library alone, so that
looking at what a site came out as needs nothing installed.
"""

import struct
import sys
import zlib


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def main():
    with open(sys.argv[1], "rb") as file:
        width, height, stride = map(int, file.readline().split())
        pixels = file.read()
    rows = bytearray()
    for y in range(height):
        row = pixels[y * stride : y * stride + width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b"\0" + rgb  # each row starts with its filter: none
    with open(sys.argv[2], "wb") as file:
        file.write(b"\x89PNG\r\n\x1a\n")
        file.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        file.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        file.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
