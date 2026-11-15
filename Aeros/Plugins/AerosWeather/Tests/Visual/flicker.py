"""Flicker metric: mean absolute difference between <shot>.png and <shot>_b.png (0.2 s apart) in the sky
region away from the HUD panels. Pure python PNG decoder (8-bit RGB/RGBA, non-interlaced)."""
import struct, zlib, sys, os

DIR = r"C:\Users\TheMiniBoss\Documents\GitHub\Aeros\Aeros\Saved\Screenshots\WindowsEditor\AerosTour"


def load(path):
    b = open(path, "rb").read()
    pos, w, h, ct, idat = 8, 0, 0, 0, b""
    while pos < len(b):
        n, t = struct.unpack(">I4s", b[pos:pos + 8])
        d = b[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", d[:10])
        elif t == b"IDAT":
            idat += d
        pos += 12 + n
    bpp = 4 if ct == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for y in range(h):
        f = raw[i]; i += 1
        cur = bytearray(raw[i:i + stride]); i += stride
        for x in range(stride):
            a = cur[x - bpp] if x >= bpp else 0
            up = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: cur[x] = (cur[x] + a) & 255
            elif f == 2: cur[x] = (cur[x] + up) & 255
            elif f == 3: cur[x] = (cur[x] + ((a + up) >> 1)) & 255
            elif f == 4:
                p = a + up - c; pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                cur[x] = (cur[x] + (a if pa <= pb and pa <= pc else (up if pb <= pc else c))) & 255
        rows.append(cur); prev = cur
    return w, h, bpp, rows


for shot in sys.argv[1:]:
    A, B = os.path.join(DIR, shot + ".png"), os.path.join(DIR, shot + "_b.png")
    if not (os.path.exists(A) and os.path.exists(B)):
        print(shot, "missing"); continue
    w, h, bpp, ra = load(A)
    _, _, _, rb = load(B)
    tot, n, big = 0, 0, 0
    for y in range(170, 500, 2):           # sky band below the caption box, above the horizon
        for x in range(420, 1240, 2):      # between the left panel and the radar
            d = sum(abs(ra[y][x * bpp + k] - rb[y][x * bpp + k]) for k in range(3)) / 3
            tot += d; n += 1; big += d > 12
    print("%s  mean |diff| %.2f   pixels > 12: %.1f %%" % (shot, tot / n, 100.0 * big / n))
