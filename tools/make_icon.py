"""Draws the KoeLoom icons: two threads woven into a voice wave on the dark ground.

resources/icon/icon.png        1024 px, three crossings with over/under weaving (big sizes)
resources/icon/icon-small.png  32 px, the single-crossing mark with thicker strokes. JUCE uses the smallest
                               image at least as wide as each .ico size, so this one covers 16 and 32 px

Colors are the Theme tokens (bg #0E1217, text #EDF1F6, accent #4CC9F0).
Run: python tools/make_icon.py
"""
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

SIZE = 1024
SS = 4  # supersampling for smooth strokes
S = SIZE * SS

BG_TOP = (0x17, 0x1D, 0x26)
BG_BOTTOM = (0x0B, 0x0F, 0x14)
BG_FLAT = (0x0E, 0x12, 0x17)
WHITE = (0xED, 0xF1, 0xF6)
ACCENT = (0x4C, 0xC9, 0xF0)


def bg_at(y):
    t = y / (S - 1)
    return tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))


def ground(gradient):
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    fill = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(fill)
    for y in range(S):
        d.line((0, y, S, y), fill=bg_at(y) if gradient else BG_FLAT)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=int(S * 0.22), fill=255)
    img.paste(fill, (0, 0), mask)
    return img


def stamp(d, pts, r, col):
    # discs along a dense path avoid PIL's gaps between thick polyline segments
    for x, y in pts:
        c = col(y) if callable(col) else col
        d.ellipse((x - r, y - r, x + r, y + r), fill=c)


def woven():
    img = ground(True)
    n = 4000
    x0, x1, cy = S * 0.13, S * 0.87, S * 0.5
    amp = S * 0.14
    r = S * 0.04
    gap = S * 0.02

    def thread(sign):
        pts = []
        for i in range(n + 1):
            t = i / n
            env = 0.8 + 0.2 * math.sin(math.pi * t)  # a little louder in the middle, like a spoken word
            pts.append((x0 + (x1 - x0) * t, cy + sign * amp * env * math.cos(3 * math.pi * t)))
        return pts

    a, b = thread(-1), thread(+1)  # crossings at t = 1/6, 1/2, 5/6

    def mask(pts, rad, disc_ts=()):
        m = Image.new("L", (S, S), 0)
        stamp(ImageDraw.Draw(m), pts, rad, 255)
        if disc_ts:  # keep only the parts near these crossings
            keep = Image.new("L", (S, S), 0)
            for t in disc_ts:
                x, y = a[int(n * t)]
                ImageDraw.Draw(keep).ellipse((x - 3 * r, y - 3 * r, x + 3 * r, y + 3 * r), fill=255)
            m = ImageChops.multiply(m, keep)
        return m

    # weave: white goes over at the outer crossings, blue over at the middle one;
    # the thread underneath is cut by the other's outline only around its crossing
    blue = ImageChops.subtract(mask(b, r), mask(a, r + gap, (1 / 6, 5 / 6)))
    white = ImageChops.subtract(mask(a, r), mask(b, r + gap, (1 / 2,)))
    img.paste(ACCENT, (0, 0), blue)
    img.paste(WHITE, (0, 0), white)
    return img


def simple():
    img = ground(False)
    d = ImageDraw.Draw(img)

    def cubic(p0, p1, p2, p3, n=3000):
        out = []
        for i in range(n + 1):
            t = i / n
            u = 1 - t
            out.append(tuple(u**3 * p0[k] + 3 * u * u * t * p1[k] + 3 * u * t * t * p2[k] + t**3 * p3[k] for k in (0, 1)))
        return out

    # the in-app logo's 24x24 viewBox: M3 7 c6 0 9 10 18 10  /  M3 17 c6 0 9-10 18-10
    pad = 0.12
    k = S / 24.0 * (1 - 2 * pad)
    m = lambda x, y: (S * pad + x * k, S * pad + y * k)
    r = S * 0.062
    stamp(d, cubic(m(3, 7), m(9, 7), m(12, 17), m(21, 17)), r, WHITE)
    stamp(d, cubic(m(3, 17), m(9, 17), m(12, 7), m(21, 7)), r, ACCENT)
    return img


def main():
    out = Path(__file__).resolve().parent.parent / "resources" / "icon"
    for name, draw, px in (("icon.png", woven, SIZE), ("icon-small.png", simple, 32)):
        path = out / name
        draw().resize((px, px), Image.LANCZOS).save(path)
        print(path)


if __name__ == "__main__":
    main()
