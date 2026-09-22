"""Generates the Asosdot Engine mark in every form the engine needs.

The mark is a hexagonal camera iris with a bright core and an anamorphic
streak: it points at the engine's own lens flare work, and an aperture opening
in the dark suits what the engine is being used for. Geometry is defined once
here so the SVGs the editor rasterises and the PNG/ICO the OS shows cannot
drift apart.
"""

import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "."

# Palette. Dark, cold, with one warm accent so the core reads as light rather
# than as a hole.
BG = (0x14, 0x16, 0x1C)
RING = (0xE4, 0xEC, 0xF7)
CORE = (0xFF, 0xFF, 0xFF)
STREAK = (0x7F, 0xC4, 0xFF)
GHOST = (0xFF, 0xB4, 0x5E)

# In unit coordinates on a 0..1 square, centred on 0.5.
R_OUTER = 0.400
R_INNER = 0.300
R_CORE = 0.105
STREAK_HALF_W = 0.480
STREAK_HALF_H = 0.055


def hexagon(cx, cy, r, rot=0.0):
    return [(cx + r * math.cos(rot + i * math.pi / 3.0), cy + r * math.sin(rot + i * math.pi / 3.0)) for i in range(6)]


def svg_poly(pts, scale, digits=2):
    return "M" + "L".join(f"{x * scale:.{digits}f} {y * scale:.{digits}f}" for x, y in pts) + "Z"


def ring_path(scale):
    """Hexagonal annulus as one path, outer clockwise and inner reversed so the
    even-odd fill leaves the middle open."""
    outer = hexagon(0.5, 0.5, R_OUTER)
    inner = hexagon(0.5, 0.5, R_INNER)[::-1]
    return svg_poly(outer, scale) + svg_poly(inner, scale)


def render(size, rounded_bg, detail):
    """Supersampled raster of the mark. rounded_bg draws the dark badge behind
    it; detail adds the ghosts that only survive at larger sizes."""
    ss = 8
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if rounded_bg:
        d.rounded_rectangle([0, 0, n - 1, n - 1], radius=int(n * 0.22), fill=BG + (255,))

    # The iris itself.
    d.polygon([(x * n, y * n) for x, y in hexagon(0.5, 0.5, R_OUTER)], fill=RING + (255,))
    d.polygon([(x * n, y * n) for x, y in hexagon(0.5, 0.5, R_INNER)], fill=(0, 0, 0, 0))

    if detail:
        ghosts = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        gd = ImageDraw.Draw(ghosts)
        for t, rad, col, alpha in ((0.26, 0.062, GHOST, 210), (0.76, 0.042, STREAK, 190)):
            cx = 0.5 + (t - 0.5) * 1.34
            cy = 0.5 - (t - 0.5) * 0.52
            gd.polygon([(x * n, y * n) for x, y in hexagon(cx, cy, rad)], fill=col + (alpha,))
        img.alpha_composite(ghosts)

    # Streak and core go on top of the iris, the way stray light actually falls
    # across a lens rather than behind it. Both are soft falloffs built in numpy;
    # a hard ellipse read as a blue nub poking out either side of the hexagon.
    yy, xx = np.mgrid[0:n, 0:n]
    u = xx / n - 0.5
    v = yy / n - 0.5

    streak = np.clip(1.0 - np.abs(u) / STREAK_HALF_W, 0.0, 1.0) ** 1.5
    streak = streak * np.exp(-((v / STREAK_HALF_H) ** 2) * 2.2)
    layer = np.zeros((n, n, 4), dtype=np.uint8)
    layer[..., 0], layer[..., 1], layer[..., 2] = STREAK
    layer[..., 3] = (np.clip(streak, 0.0, 1.0) * 235).astype(np.uint8)
    img.alpha_composite(Image.fromarray(layer, "RGBA"))

    core = np.clip(1.0 - np.hypot(u, v) / R_CORE, 0.0, 1.0) ** 1.5
    layer = np.zeros((n, n, 4), dtype=np.uint8)
    layer[..., 0], layer[..., 1], layer[..., 2] = CORE
    layer[..., 3] = (core * 255).astype(np.uint8)
    img.alpha_composite(Image.fromarray(layer, "RGBA"))

    return img.resize((size, size), Image.LANCZOS)


def write(path, img):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)
    print(f"  {path}  {img.size}")


def main():
    print("rasters:")
    write(os.path.join(OUT, "main/app_icon.png"), render(128, True, True))

    ico = os.path.join(OUT, "platform/windows/asosdot.ico")
    os.makedirs(os.path.dirname(ico), exist_ok=True)
    render(256, True, True).save(ico, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(f"  {ico}  multi-size")

    # The console binary gets the same mark with a warmer core so the two are
    # tellable apart in a taskbar.
    global CORE, STREAK
    keep = (CORE, STREAK)
    CORE, STREAK = (0xFF, 0xE9, 0xC8), GHOST
    ico_c = os.path.join(OUT, "platform/windows/asosdot_console.ico")
    render(256, True, True).save(ico_c, sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print(f"  {ico_c}  multi-size")
    CORE, STREAK = keep

    # Boot splash: the mark on the dark badge colour, with a wordmark under it.
    splash = Image.new("RGBA", (800, 600), BG + (255,))
    mark = render(220, False, True)
    splash.alpha_composite(mark, (290, 150))
    draw = ImageDraw.Draw(splash)
    font = None
    for cand in ("C:/Windows/Fonts/segoeuisb.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"):
        if os.path.exists(cand):
            font = ImageFont.truetype(cand, 46)
            break
    text = "ASOSDOT ENGINE"
    if font:
        w = draw.textbbox((0, 0), text, font=font)[2]
        draw.text(((800 - w) / 2, 400), text, font=font, fill=(0xE4, 0xEC, 0xF7, 255))
        sub = ImageFont.truetype(font.path, 20)
        s = "Base model of Godot"
        sw = draw.textbbox((0, 0), s, font=sub)[2]
        draw.text(((800 - sw) / 2, 462), s, font=sub, fill=(0x8A, 0x93, 0xA6, 255))
    write(os.path.join(OUT, "main/splash.png"), splash)

    print("svgs:")
    # Editor icons. Flat, no gradients: the editor rasterises these with ThorVG
    # and recolours some of them, so they stay to simple filled paths.
    small = f'''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><path fill="#7fc4ff" d="M0.7 7.2h14.6v1.6H0.7z"/><path fill="#e4ecf7" fill-rule="evenodd" d="{ring_path(16)}"/><circle cx="8" cy="8" r="1.7" fill="#fff"/></svg>
'''
    for name in ("Godot", "GodotMonochrome"):
        p = os.path.join(OUT, f"editor/icons/{name}.svg")
        open(p, "w", newline="\n").write(small)
        print(f"  {p}")

    # The project.godot file icon: same mark, with a page behind it.
    gfile = f'''<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><path fill="#e4ecf7" d="M3 1h6l4 4v10H3z" opacity=".25"/><path fill="#7fc4ff" d="M2.6 7.4h10.8v1.3H2.6z"/><path fill="#e4ecf7" fill-rule="evenodd" d="{ring_path(16)}"/><circle cx="8" cy="8" r="1.6" fill="#fff"/></svg>
'''
    p = os.path.join(OUT, "editor/icons/GodotFile.svg")
    open(p, "w", newline="\n").write(gfile)
    print(f"  {p}")

    # About dialog mark, drawn large enough to carry the ghosts.
    s = 120
    ghosts = ""
    for t, rad, col in ((0.30, 0.055, "#ffb45e"), (0.72, 0.035, "#7fc4ff")):
        cx = 0.5 + (t - 0.5) * 1.30
        cy = 0.5 - (t - 0.5) * 0.55
        ghosts += f'<path fill="{col}" d="{svg_poly(hexagon(cx, cy, rad), s)}"/>'
    logo = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{s}" height="{s}"><path fill="#7fc4ff" d="M{0.045 * s:.1f} {0.470 * s:.1f}h{0.910 * s:.1f}v{0.060 * s:.1f}h-{0.910 * s:.1f}z"/>{ghosts}<path fill="#e4ecf7" fill-rule="evenodd" d="{ring_path(s)}"/><circle cx="{s / 2}" cy="{s / 2}" r="{R_CORE * s:.1f}" fill="#fff"/></svg>
'''
    p = os.path.join(OUT, "editor/icons/Logo.svg")
    open(p, "w", newline="\n").write(logo)
    print(f"  {p}")


main()
