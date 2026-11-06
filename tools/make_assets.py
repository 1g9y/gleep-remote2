"""Generate livearea PNGs + template.xml for the Gleep Remote VPK. Pure stdlib.

All PNGs are written as 8-bit palette (color type 3) images, which the Vita
requires for sce_sys graphics -- 24-bit RGB PNGs cause VitaShell install
error 0x8010113D.
"""
import struct, zlib, os, random

def write_png(path, w, h, pixel):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    # First pass: build the palette and per-pixel index data.
    # Color type 3: each pixel is a single byte indexing into PLTE.
    palette = []        # list of (r, g, b), insertion-ordered
    palette_index = {}  # (r, g, b) -> palette position
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # PNG filter type: none
        for x in range(w):
            r, g, b = pixel(x, y)
            color = (r & 0xff, g & 0xff, b & 0xff)
            idx = palette_index.get(color)
            if idx is None:
                if len(palette) >= 256:
                    raise ValueError('%s: over 256 colors, cannot write '
                                     'an 8-bit palette PNG' % path)
                idx = len(palette)
                palette.append(color)
                palette_index[color] = idx
            raw.append(idx)

    plte = bytearray()
    for r, g, b in palette:
        plte.append(r)
        plte.append(g)
        plte.append(b)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0))
           + chunk(b'PLTE', bytes(plte))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    print('wrote', path, '-', len(palette), 'colors')

def gleep_assets():
    out = os.path.join('sce_sys', 'livearea', 'contents')
    os.makedirs(out, exist_ok=True)

    # icon0.png 128x128: black tile, red frame, CRT screen with scanlines
    def icon(x, y):
        if x < 8 or x > 119 or y < 8 or y > 119: return (0, 0, 0)
        if x < 14 or x > 113 or y < 14 or y > 113: return (255, 0, 0)
        if 28 < x < 100 and 40 < y < 88:
            return (0, 255, 0) if y % 6 < 2 else (0, 40, 0)
        return (10, 10, 10)
    write_png(os.path.join('sce_sys', 'icon0.png'), 128, 128, icon)

    # bg0.png 840x500: dark with red scanlines and static specks
    rng = random.Random(1337)
    dots = {(rng.randrange(840), rng.randrange(500)) for _ in range(400)}
    def bg(x, y):
        if (x, y) in dots: return (200, 200, 200)
        if y % 4 == 0: return (40, 0, 0)
        return (8, 8, 8)
    write_png(os.path.join(out, 'bg0.png'), 840, 500, bg)

    # startup.png 480x272: black with a green 'CH 01' vibe line
    def startup(x, y):
        if 60 < x < 420 and 100 < y < 172 and y % 5 < 2:
            return (0, 255, 0)
        return (0, 0, 0)
    write_png(os.path.join(out, 'startup.png'), 480, 272, startup)

    # template.xml: livearea layout referencing the generated images
    template = '''<?xml version="1.0" encoding="utf-8"?>
<livearea style="a1" format-ver="01.00" content-rev="1">
\t<livearea-background>
\t\t<image>bg0.png</image>
\t</livearea-background>
\t<frame id="frame3" multi="false" alpha="1.0">
\t\t<liveitem>
\t\t\t<image>startup.png</image>
\t\t</liveitem>
\t</frame>
</livearea>
'''
    with open(os.path.join(out, 'template.xml'), 'w') as f:
        f.write(template)
    print('wrote', os.path.join(out, 'template.xml'))

if __name__ == '__main__':
    gleep_assets()
