#!/usr/bin/env python3
"""Generate a DMG background TIFF with a subtle drag-and-drop arrow.

Produces a clean, professional arrow that works well on both light and dark
macOS themes. The background is transparent (letting the system chrome show
through) with a mid-grey arrow.

Output: 660x400 TIFF at 144 DPI (Retina).
"""

import struct
import sys
import zlib


def make_tiff(width, height, pixels, dpi=144):
    """Create an uncompressed RGBA TIFF from pixel data."""
    # TIFF with a single IFD
    img_data = bytes(pixels)
    img_size = len(img_data)

    # IFD entries (tag, type, count, value/offset)
    entries = [
        (256, 3, 1, width),           # ImageWidth
        (257, 3, 1, height),          # ImageLength
        (258, 3, 4, 0),               # BitsPerSample (offset, filled later)
        (259, 3, 1, 1),               # Compression = None
        (262, 3, 1, 2),               # PhotometricInterpretation = RGB
        (273, 4, 1, 0),               # StripOffsets (filled later)
        (277, 3, 1, 4),               # SamplesPerPixel
        (278, 3, 1, height),          # RowsPerStrip
        (279, 4, 1, img_size),        # StripByteCounts
        (282, 5, 1, 0),               # XResolution (offset, filled later)
        (283, 5, 1, 0),               # YResolution (offset, filled later)
        (284, 3, 1, 1),               # PlanarConfiguration = Chunky
        (296, 3, 1, 2),               # ResolutionUnit = inch
        (338, 3, 1, 2),               # ExtraSamples = Unassociated alpha
    ]

    num_entries = len(entries)
    # Layout: header(8) + IFD(2 + 12*n + 4) + extra_data + image_data
    ifd_offset = 8
    ifd_size = 2 + 12 * num_entries + 4
    extra_offset = ifd_offset + ifd_size

    # Extra data: BitsPerSample (8 bytes) + XRes (8 bytes) + YRes (8 bytes)
    bits_per_sample_off = extra_offset
    xres_off = extra_offset + 8
    yres_off = extra_offset + 16
    img_offset = extra_offset + 24

    # Fill offsets
    entries[2] = (258, 3, 4, bits_per_sample_off)
    entries[5] = (273, 4, 1, img_offset)
    entries[9] = (282, 5, 1, xres_off)
    entries[10] = (283, 5, 1, yres_off)

    out = bytearray()
    # Header: little-endian TIFF
    out += b"II"
    out += struct.pack("<H", 42)
    out += struct.pack("<I", ifd_offset)

    # IFD
    out += struct.pack("<H", num_entries)
    for tag, typ, count, value in entries:
        out += struct.pack("<HHI", tag, typ, count)
        if typ == 3 and count == 1:
            out += struct.pack("<HH", value, 0)
        elif typ == 4 and count == 1:
            out += struct.pack("<I", value)
        else:
            out += struct.pack("<I", value)
    out += struct.pack("<I", 0)  # next IFD = 0

    # Extra data
    out += struct.pack("<HHHH", 8, 8, 8, 8)  # BitsPerSample
    out += struct.pack("<II", dpi, 1)          # XResolution
    out += struct.pack("<II", dpi, 1)          # YResolution

    # Image data
    out += img_data

    return bytes(out)


def draw_arrow(pixels, width, height, x_start, x_end, y_center):
    """Draw a clean right-pointing arrow."""
    color = (160, 160, 160, 140)  # subtle mid-grey, semi-transparent

    shaft_half = 5
    head_len = 28
    head_half = 18

    shaft_x_end = x_end - head_len

    # Shaft
    for y in range(y_center - shaft_half, y_center + shaft_half + 1):
        if 0 <= y < height:
            for x in range(x_start, shaft_x_end):
                if 0 <= x < width:
                    idx = (y * width + x) * 4
                    pixels[idx:idx+4] = color

    # Arrowhead
    for x in range(shaft_x_end, x_end):
        progress = (x - shaft_x_end) / head_len
        half_h = int(head_half * (1.0 - progress))
        for y in range(y_center - half_h, y_center + half_h + 1):
            if 0 <= y < height and 0 <= x < width:
                idx = (y * width + x) * 4
                pixels[idx:idx+4] = color


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "dmg-background.tiff"

    W, H = 660, 400

    # Transparent background
    pixels = bytearray([0, 0, 0, 0] * W * H)

    # Arrow centered between the two icons (x=170 and x=490, midpoint=330)
    arrow_y = 160
    arrow_x_start = 275
    arrow_x_end = 385

    draw_arrow(pixels, W, H, arrow_x_start, arrow_x_end, arrow_y)

    tiff_data = make_tiff(W, H, pixels, dpi=72)
    with open(output, "wb") as f:
        f.write(tiff_data)

    print(f"Background created: {output} ({W}x{H}, {len(tiff_data)} bytes)")


if __name__ == "__main__":
    main()
