#!/usr/bin/env python3
"""Convert 8-bit RGBA PNGs to LVGL 8.3 LV_IMG_CF_TRUE_COLOR_ALPHA C arrays.

Target build is LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0, so each pixel is
RGB565 little-endian (2 bytes) followed by one alpha byte.

Pure stdlib: no PIL on this machine.
"""
import struct
import sys
import zlib


def read_png_rgba(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")

    pos = 8
    idat = b""
    width = height = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length          # length + type + body + crc

        if ctype == b"IHDR":
            width, height, depth, color, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", body)
            if depth != 8 or color != 6:
                raise SystemExit(f"{path}: need 8-bit RGBA (depth={depth} color={color})")
            if interlace:
                raise SystemExit(f"{path}: interlaced PNG unsupported")
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    raw = zlib.decompress(idat)
    bpp = 4
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)

    # Undo the per-scanline PNG filters.
    i = 0
    for _ in range(height):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i + stride]); i += stride
        if f == 1:      # Sub
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif f == 2:    # Up
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:    # Average
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif f == 4:    # Paeth
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        elif f != 0:
            raise SystemExit(f"{path}: bad filter {f}")
        out += line
        prev = line

    return width, height, bytes(out)


def to_lvgl(name, path):
    w, h, rgba = read_png_rgba(path)
    body = bytearray()
    for i in range(0, len(rgba), 4):
        r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        body += bytes((rgb565 & 0xFF, (rgb565 >> 8) & 0xFF, a))

    lines = []
    lines.append(f"// {name}: {w}x{h}, served by the speaker itself at {path.split('/')[-1]}")
    lines.append(f"static const uint8_t {name}_map[] = {{")
    for off in range(0, len(body), 12):
        chunk = ", ".join(f"0x{c:02X}" for c in body[off:off + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_img_dsc_t {name} = {{")
    lines.append("    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,")
    lines.append("    .header.always_zero = 0,")
    lines.append("    .header.reserved = 0,")
    lines.append(f"    .header.w = {w},")
    lines.append(f"    .header.h = {h},")
    lines.append(f"    .data_size = {len(body)},")
    lines.append(f"    .data = {name}_map,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines), w, h, len(body)


if __name__ == "__main__":
    out = []
    out.append("// GENERATED — do not edit by hand. See scratchpad/png2lvgl.py.")
    out.append("//")
    out.append("// Source artwork is fetched from the speakers themselves over the LAN")
    out.append("// (device_description.xml -> <iconList> -> /img/icon-*.png), so these are")
    out.append("// Sonos's own product icons rather than redrawn approximations.")
    out.append("")
    out.append('#include "sonos_icons.h"')
    out.append("")
    for name, path in (("icon_era300", sys.argv[1]), ("icon_era100", sys.argv[2])):
        text, w, h, size = to_lvgl(name, path)
        out.append(text)
        print(f"{name}: {w}x{h}, {size} bytes", file=sys.stderr)
    open(sys.argv[3], "w").write("\n".join(out))
