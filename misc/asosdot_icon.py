"""Generates the Asosdot Engine mark in every form the engine needs.

`asosdot_mark.png` beside this script is the source of truth: a white silhouette
on transparency. Everything else is derived from it, so the vector and raster
forms cannot drift apart.

The editor rasterises its icons from SVG, so the silhouette is traced to paths
rather than embedded, which keeps those icons crisp at any editor scale.

Run from the repository root:

    python misc/asosdot_icon.py .
"""

import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
SOURCE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "asosdot_mark.png")

# Dark and cold, to suit what the engine is being used for. BG is also what
# main_builders.py declares as the boot splash background, so the splash can sit
# on it without showing a seam.
BG = (0x14, 0x16, 0x1C)
MARK = (0xFF, 0xFF, 0xFF)
CONSOLE_MARK = (0xFF, 0xD9, 0xA8)
TEXT = (0xE4, 0xEC, 0xF7)
SUBTEXT = (0x8A, 0x93, 0xA6)

# Fraction of the icon the mark occupies, leaving a margin inside the badge.
MARK_INSET = 0.76


def load_mark(color=MARK):
    src = Image.open(SOURCE).convert("RGBA")
    a = np.asarray(src)
    out = np.zeros_like(a)
    out[..., 0], out[..., 1], out[..., 2] = color
    out[..., 3] = a[..., 3]
    return Image.fromarray(out, "RGBA")


def fit(mark, box):
    """Scales the mark to fit a square of `box` pixels, preserving its aspect."""
    w, h = mark.size
    scale = box / max(w, h)
    return mark.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)


def render(size, badge, color=MARK):
    ss = 4
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    if badge:
        ImageDraw.Draw(img).rounded_rectangle([0, 0, n - 1, n - 1], radius=int(n * 0.22), fill=BG + (255,))

    mark = fit(load_mark(color), int(n * MARK_INSET))
    img.alpha_composite(mark, ((n - mark.size[0]) // 2, (n - mark.size[1]) // 2))
    return img.resize((size, size), Image.LANCZOS)


def trace(scale, trace_res=512, epsilon_frac=0.0015):
    """Traces the silhouette to SVG path data in a `scale` unit square.

    Outer contours and the holes inside them come back in one path; the caller
    pairs it with fill-rule="evenodd" so the counter of the A and the bite out of
    the crescent stay open.
    """
    src = Image.open(SOURCE).convert("RGBA").resize((trace_res, trace_res), Image.LANCZOS)
    alpha = np.asarray(src)[..., 3]
    _, binary = cv2.threshold(alpha, 127, 255, cv2.THRESH_BINARY)

    contours, _ = cv2.findContours(binary, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)

    parts = []
    for contour in contours:
        simplified = cv2.approxPolyDP(contour, epsilon_frac * cv2.arcLength(contour, True), True)
        if len(simplified) < 3:
            continue
        points = simplified.reshape(-1, 2) * (scale / trace_res)
        parts.append("M" + "L".join(f"{x:.2f} {y:.2f}" for x, y in points) + "Z")

    return "".join(parts)


def write_svg(path, body, size):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as file:
        file.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}">{body}</svg>\n')
    print(f"  {path}")


def write_png(path, img):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path)
    print(f"  {path}  {img.size}")


def main():
    ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

    print("rasters:")
    write_png(os.path.join(OUT, "main/app_icon.png"), render(128, badge=True))

    ico = os.path.join(OUT, "platform/windows/asosdot.ico")
    render(256, badge=True).save(ico, sizes=ico_sizes)
    print(f"  {ico}  multi-size")

    # The console binary is tinted so the two are tellable apart in a taskbar.
    ico_console = os.path.join(OUT, "platform/windows/asosdot_console.ico")
    render(256, badge=True, color=CONSOLE_MARK).save(ico_console, sizes=ico_sizes)
    print(f"  {ico_console}  multi-size")

    # Boot splash, left transparent on purpose. The engine clears to
    # application/boot_splash/bg_color and draws this over it, so a transparent
    # splash matches whatever that colour is instead of baking one in and showing
    # a rectangle wherever the two disagree.
    splash = Image.new("RGBA", (800, 600), (0, 0, 0, 0))
    mark = fit(load_mark(), 240)
    splash.alpha_composite(mark, ((800 - mark.size[0]) // 2, 120))
    draw = ImageDraw.Draw(splash)

    font_path = next(
        (
            candidate
            for candidate in (
                "C:/Windows/Fonts/segoeuisb.ttf",
                "C:/Windows/Fonts/segoeui.ttf",
                "C:/Windows/Fonts/arial.ttf",
            )
            if os.path.exists(candidate)
        ),
        None,
    )
    if font_path:
        title_font = ImageFont.truetype(font_path, 46)
        sub_font = ImageFont.truetype(font_path, 20)
        for text, font, y, fill in (
            ("ASOSDOT ENGINE", title_font, 400, TEXT),
            ("Base model of Godot", sub_font, 462, SUBTEXT),
        ):
            width = draw.textbbox((0, 0), text, font=font)[2]
            draw.text(((800 - width) / 2, y), text, font=font, fill=fill + (255,))
    write_png(os.path.join(OUT, "main/splash.png"), splash)

    # The same art on the boot colour, for the README. GitHub composites images
    # onto whichever theme the reader has, so the transparent splash would go
    # invisible on a light one.
    banner = Image.new("RGBA", splash.size, BG + (255,))
    banner.alpha_composite(splash)
    write_png(os.path.join(OUT, "misc/asosdot_banner.png"), banner)

    print("svgs:")
    # Flat filled paths only: the editor rasterises these with ThorVG and
    # recolours some of them.
    small = f'<path fill="#e4ecf7" fill-rule="evenodd" d="{trace(16)}"/>'
    for name in ("Godot", "GodotMonochrome"):
        write_svg(os.path.join(OUT, f"editor/icons/{name}.svg"), small, 16)

    page = '<path fill="#e4ecf7" d="M3 1h6l4 4v10H3z" opacity=".25"/>'
    mark_small = (
        f'<g transform="translate(3.2 3.2) scale(0.6)"><path fill="#e4ecf7" fill-rule="evenodd" d="{trace(16)}"/></g>'
    )
    write_svg(os.path.join(OUT, "editor/icons/GodotFile.svg"), page + mark_small, 16)

    write_svg(
        os.path.join(OUT, "editor/icons/Logo.svg"),
        f'<path fill="#e4ecf7" fill-rule="evenodd" d="{trace(120, epsilon_frac=0.0008)}"/>',
        120,
    )


main()
