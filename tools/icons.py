#!/usr/bin/env python3
"""
Rebuild every raster icon from assets/tinyiv.svg, the only master.

The PNGs in assets/ and the application icon in src/resources/Win32 are all
rendered from it here, so a change to the mark is a change to one file followed
by a run of this script.

Needs rsvg-convert and Pillow.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MASTER = os.path.join(ROOT, "assets", "tinyiv.svg")
WIN32 = os.path.join(ROOT, "src", "resources", "Win32")

# The sizes assets/ ships, and the ones an .ico carries.
PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


def render(size, out):
    """One size, straight off the vector."""
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), MASTER, "-o", out], check=True)


def ico(raster, out):
    """
    Handing over every size keeps Pillow from resampling one of them down into
    the rest, so each frame is still the vector rendered at that size.
    """
    frames = [Image.open(raster[s]).convert("RGBA") for s in ICO_SIZES]
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
