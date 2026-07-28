#!/usr/bin/env python3
"""Render the app icon at every hicolor size from the Android app's launcher art.

Usage: make-icons.py <path-to-fujinet-go-coco>/app/src/main/res

fujinet-go-coco (the Android app) is a sibling checkout, not part of this
repository, so its path is an argument rather than assumed.

Composites the xxxhdpi adaptive icon layers -- ic_launcher_adaptive_back.png
and ic_launcher_adaptive_fore.png, 432x432, already safe-zone padded by
Android's own icon tooling -- rather than upscaling the small (192x192)
flattened ic_launcher.png, so the largest installed size (512) is a mild
upscale from 432 instead of a soft one from 192. Masked to the same circular
badge ic_launcher.png itself uses, so the desktop icon matches the launcher
icon Android users already recognize.

Output: data/icons/hicolor/<size>x<size>/apps/fujinet-go-coco.png, for every
size CMakeLists.txt installs. Committed to the repo like any other asset --
this is a one-shot dev tool, not a build step, so the source Android checkout
does not need to be present for anyone else to build this repo.
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 24, 32, 48, 64, 128, 192, 256, 512]
CANVAS = 432
# Matches ic_launcher.png's own proportions: the flattened legacy icon is a
# circle with a small margin, not edge-to-edge.
CIRCLE_FRACTION = 0.92

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <path-to-fujinet-go-coco>/app/src/main/res")
    res_dir = Path(sys.argv[1])
    back = res_dir / "mipmap-xxxhdpi" / "ic_launcher_adaptive_back.png"
    fore = res_dir / "mipmap-xxxhdpi" / "ic_launcher_adaptive_fore.png"
    for p in (back, fore):
        if not p.exists():
            sys.exit(f"make-icons.py: {p} not found")

    bg = Image.open(back).convert("RGBA")
    fg = Image.open(fore).convert("RGBA")
    if bg.size != (CANVAS, CANVAS) or fg.size != (CANVAS, CANVAS):
        sys.exit(f"make-icons.py: expected {CANVAS}x{CANVAS} adaptive icon "
                 f"layers, got {bg.size} / {fg.size}")

    flat = Image.alpha_composite(bg, fg)

    # The background layer is opaque everywhere, so compositing alone gives
    # a square with no transparency -- the circular badge comes entirely
    # from replacing the alpha channel with this mask.
    mask = Image.new("L", (CANVAS, CANVAS), 0)
    d = CANVAS * CIRCLE_FRACTION
    off = (CANVAS - d) / 2
    ImageDraw.Draw(mask).ellipse([off, off, off + d, off + d], fill=255)
    flat.putalpha(mask)

    for size in SIZES:
        out_dir = REPO_ROOT / "data" / "icons" / "hicolor" / f"{size}x{size}" / "apps"
        out_dir.mkdir(parents=True, exist_ok=True)
        resized = flat.resize((size, size), Image.Resampling.LANCZOS)
        out_path = out_dir / "fujinet-go-coco.png"
        resized.save(out_path)
        print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
