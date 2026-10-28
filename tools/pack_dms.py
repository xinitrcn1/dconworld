#!/usr/bin/env python3
"""Packs DressMySlugcat template atlases into assets/slugcat.png + slugcat.txt.

The DMS templates (https://github.com/MatheusVigaro/DressMySlugcat,
mod/dressmyslugcat/template) mirror the vanilla atlas elements: every frame
has the vanilla element size, so the vanilla sprite anchors apply as-is. They
also carry red guide markers (corner ticks, frame numbers, pivots) which are
stripped here. The output frame list is plain text so the game never parses
JSON:

    <name> <x> <y> <w> <h>        (pixels, origin at the atlas' top left)

A 4x4 opaque white block named "pixel" is added for untextured geometry
(the tail mesh) so the whole slugcat draws with one texture.

usage: pack_dms.py <dms template dir> <output dir>
"""
import json
import sys
from pathlib import Path

from PIL import Image

SHEETS = ["body", "hips", "head", "face", "legs", "arm"]
ATLAS_WIDTH = 1024
PAD = 2


def is_marker(r, g, b, a):
    return a > 0 and r > 180 and g < 90 and b < 90


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    frames = []  # (name, image)
    for sheet in SHEETS:
        meta = json.loads((src / f"{sheet}.txt").read_text())
        img = Image.open(src / f"{sheet}.png").convert("RGBA")
        for key, v in meta["frames"].items():
            f = v["frame"]
            assert not v.get("rotated"), key
            crop = img.crop((f["x"], f["y"], f["x"] + f["w"], f["y"] + f["h"]))
            px = crop.load()
            for y in range(crop.height):
                for x in range(crop.width):
                    if is_marker(*px[x, y]):
                        px[x, y] = (0, 0, 0, 0)
            frames.append((key.removesuffix(".png"), crop))
    frames.append(("pixel", Image.new("RGBA", (4, 4), (255, 255, 255, 255))))

    # shelf packing, tallest first
    order = sorted(frames, key=lambda fr: -fr[1].height)
    x = y = shelf_h = 0
    placed = {}
    for name, im in order:
        if x + im.width + PAD > ATLAS_WIDTH:
            x, y, shelf_h = 0, y + shelf_h + PAD, 0
        placed[name] = (x, y, im)
        x += im.width + PAD
        shelf_h = max(shelf_h, im.height)
    height = 1
    while height < y + shelf_h:
        height *= 2
    atlas = Image.new("RGBA", (ATLAS_WIDTH, height), (0, 0, 0, 0))
    lines = [
        "# slugcat sprite atlas frames, packed from the DressMySlugcat template",
        "# (github.com/MatheusVigaro/DressMySlugcat) by tools/pack_dms.py",
        "# name x y w h (pixels, origin top left of slugcat.png)",
    ]
    for name, _ in frames:
        fx, fy, im = placed[name]
        atlas.paste(im, (fx, fy))
        lines.append(f"{name} {fx} {fy} {im.width} {im.height}")
    out.mkdir(parents=True, exist_ok=True)
    atlas.save(out / "slugcat.png", optimize=True)
    (out / "slugcat.txt").write_text("\n".join(lines) + "\n")
    print(f"{len(frames)} frames -> {ATLAS_WIDTH}x{height}")


if __name__ == "__main__":
    main()
