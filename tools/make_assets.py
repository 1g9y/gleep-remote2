"""Generate livearea PNGs for the Gleep Remote VPK. Pure stdlib."""
import struct, zlib, os

def write_png(path, w, h, pixel):
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    raw = b''
    for y in range(h):
        raw += b'\x00' + bytes(pixel(x, y) for x in range(w))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    print('wrote', path)

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
    import random
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

if __name__ == '__main__':
    gleep_assets()
