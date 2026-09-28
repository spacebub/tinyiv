#!/usr/bin/env python3
"""
assets/tinyiv.svg is the only master: the PNGs in assets/ and the .ico in src/resources/Win32
are rendered from it here, never edited. Needs rsvg-convert and Pillow.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = os.path.join(ROOT, "assets", "tinyiv.svg")
WIN32 = os.path.join(ROOT, "src", "resources", "Win32")

PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


def render(size, out):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), MASTER, "-o", out], check=True)


def ico(raster, out):
    frames = [Image.open(raster[s]).convert("RGBA") for s in ICO_SIZES]
    # Every size is handed over, or Pillow scales the largest down into the rest.
    # https://pillow.readthedocs.io/en/stable/handbook/image-file-formats.html#ico
    frames[-1].save(out, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=frames[:-1])


def main():
    with tempfile.TemporaryDirectory() as tmp:
        raster = {}

        for size in sorted({*PNG_SIZES, *ICO_SIZES}):
            path = os.path.join(tmp, f"{size}.png")
            render(size, path)
            raster[size] = path

        for size in PNG_SIZES:
            out = os.path.join(ROOT, "assets", f"tinyiv-{size}.png")

            with open(raster[size], "rb") as src, open(out, "wb") as dst:
                dst.write(src.read())

            print(f"assets/tinyiv-{size}.png")

        ico(raster, os.path.join(WIN32, "tinyiv.ico"))
        print("src/resources/Win32/tinyiv.ico")


if __name__ == "__main__":
    sys.exit(main())
