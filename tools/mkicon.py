#!/usr/bin/env python3
"""Draw the OnionOS app icon: five Euclidean rings, one per voice.

usage: mkicon.py OUT.png
"""
import math
import sys
from PIL import Image, ImageDraw

COLORS = [(255, 70, 90), (255, 160, 40), (250, 230, 60), (60, 220, 160), (80, 170, 255)]
PATTERNS = [(3, 8), (4, 16), (2, 8), (5, 13), (7, 16)]


def main():
    s = 4  # supersample
    size = 80
    im = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, size * s - 1, size * s - 1], radius=16 * s, fill=(18, 18, 26, 255))
    cx = cy = size * s / 2
    for i, ((k, n), col) in enumerate(zip(PATTERNS, COLORS)):
        r = (34 - i * 6) * s
        for j in range(n):
            a = -math.pi / 2 + 2 * math.pi * j / n
            x, y = cx + r * math.cos(a), cy + r * math.sin(a)
            hit = (j * k) % n < k
            rr = (2.4 if hit else 1.1) * s
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=col if hit else (70, 70, 85))
    im.resize((size, size), Image.LANCZOS).save(sys.argv[1])


if __name__ == "__main__":
    main()
