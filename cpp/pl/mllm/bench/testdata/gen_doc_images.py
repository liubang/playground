#!/usr/bin/env python3
"""Generate deterministic OCR regression test images (doc_small / doc_large).

Renders realistic English document text with a system font so the
PaddleOCR-VL model produces meaningful, stable transcriptions. Used by the
mllm perf/correctness regression (see cpp/pl/mllm/bench/PERF_BASELINE.md).

Usage:
    python3 gen_doc_images.py [output_dir]   # default: /tmp
"""

import random
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/System/Library/Fonts/Supplemental/Arial.ttf"

PARAS_SMALL = [
    "The quick brown fox jumps over the lazy dog.",
    "Invoices: total $1,024.50, tax 8.25%.",
    "Page 1 of 3  |  Report 2026-09",
]

WORDS = (
    "the a of to and in is was he for it with as his on be at by this had not are but from "
    "or have an they which one you were her all she there would their we him been has when "
    "who will more no if out so said what up its about into than them can only other new "
    "some could time these two may then do first any my now such like our over man me even "
    "most made after also did many before must through back years where much your way well "
    "down should because each just those people how too little state good very make world "
    "still own see men work long get here between both life being under never day same "
    "another know while last might us great old year off come since against go came right "
    "used take three"
).split()


def render(paras, width, path, fsize=17):
    font = ImageFont.truetype(FONT_PATH, fsize)
    line_h = fsize + 8
    height = 30 + line_h * len(paras) + 20
    img = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(img)
    y = 20
    for para in paras:
        draw.text((24, y), para, font=font, fill=(10, 10, 10))
        y += line_h
    img.save(path)
    print(path, img.size)


def main():
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp")
    out_dir.mkdir(parents=True, exist_ok=True)

    random.seed(7)  # deterministic content
    paras_large = []
    for _ in range(28):
        line = " ".join(random.choice(WORDS) for _ in range(11)) + "."
        paras_large.append(line.capitalize())

    render(PARAS_SMALL, 760, out_dir / "doc_small.png")
    render(paras_large, 900, out_dir / "doc_large.png")


if __name__ == "__main__":
    main()
