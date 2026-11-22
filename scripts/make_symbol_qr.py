#!/usr/bin/env python3
"""Create QR code files from the CSV written by find_symbol_qr.py.

The CSV is sorted by number of changes, so the first matching rows are the
best ones. Files go to <out>/svg (28 mm, no quiet zone) and <out>/png (with
quiet zone, for on-screen scanning), three per code:

  *_1_raw     the unmodified QR code
  *_2_filled  symbol modules black, surround white
  *_3_gold    like filled, but the symbol modules are gold

The gold variant is one PNG, but two single-colour SVG files, because slicers
such as Bambu Studio give an SVG only one colour:

  *_3_gold_a_black.svg   everything except the symbols
  *_3_gold_b_gold.svg    only the symbols

Both have the same 28 mm page. A slicer that centres each SVG on its own
shapes loses that, so the script prints where the centre of the gold part
belongs relative to the centre of the black part.

File names are qr_<changes>_<id>_v<version><ec>_mask<mask>_s<size>_<n>_<variant>,
where <ec> is the error correction level, <id> is the URL path with "/" written as "-" (n-123 for /n/123, 123 for
/123) and "-noscheme" appended when the URL has no https://.

Example:
  python make_symbol_qr.py qr_candidates.csv --size 5 --count 10
  python make_symbol_qr.py qr_candidates.csv --size 7 --https no --n-path no --count 5
"""
import argparse
import csv
from pathlib import Path

from qr_symbol_common import (GOLD, count_changes, decodes, make_png, qr_matrices, symbol_masks,
                              variant_layers, write_svg, write_svg_modules)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", help="CSV file from find_symbol_qr.py")
    p.add_argument("--size", type=int, required=True, help="symbol size in modules, e.g. 5 or 7")
    p.add_argument("--https", choices=["yes", "no", "any"], default="yes",
                   help="URL with https:// (yes), without (no) or both (any). Default yes")
    p.add_argument("--n-path", choices=["yes", "no", "any"], default="yes",
                   help="URL with /n/ (yes), without (no) or both (any). Default yes")
    p.add_argument("--count", type=int, default=5, help="number of QR codes to create (default 5)")
    p.add_argument("--version", type=int, help="only this QR version (default: any version in the CSV)")
    p.add_argument("--ec", choices=["L", "M", "Q", "H"],
                   help="only this error correction level (default: any level in the CSV)")
    p.add_argument("--size-mm", type=float, default=28.0, help="QR size without quiet zone")
    p.add_argument("-o", "--out", default="qr_out", help="output directory")
    args = p.parse_args()

    with open(args.csv, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    rows.sort(key=lambda r: int(r["changes"]))  # in case the CSV was re-sorted by hand

    def wanted(r):
        return (int(r["size"]) == args.size
                and args.https in ("any", r["https"])
                and args.n_path in ("any", r["n_path"])
                and args.version in (None, int(r["version"]))
                and args.ec in (None, r.get("ec", "H")))

    picked = [r for r in rows if wanted(r)]
    print(f"{len(picked)} of {len(rows)} CSV rows match, creating the best {min(args.count, len(picked))}.")
    picked = picked[:args.count]
    if not picked:
        return

    out = Path(args.out)
    for sub in ("svg", "png"):
        (out / sub).mkdir(parents=True, exist_ok=True)
    print("  changes  version  ec mask dx dy  black  white  decodes raw/filled/gold  url")
    for r in picked:
        url, version, mask = r["url"], int(r["version"]), int(r["mask"])
        dx, dy = int(r["dx"]), int(r["dy"])
        ec = r.get("ec", "H")
        matrix = qr_matrices(url, version, ec)[mask]
        sym, sur = symbol_masks(len(matrix), dx, dy, args.size)
        black, white, changes = count_changes(matrix, sym, sur)
        if changes != int(r["changes"]):
            print(f"  warning: {url} needs {changes} changes, the CSV says {r['changes']}")
        scheme, sep, rest = url.rpartition("://")
        ident = rest.partition("/")[2].replace("/", "-") + ("" if sep else "-noscheme")
        ok = []
        # The variants are numbered so that raw sorts before filled and gold.
        for i, variant in enumerate(("raw", "filled", "gold"), 1):
            blk, gld = variant_layers(matrix, sym, sur, variant)
            stem = f"qr_{changes:02d}_{ident}_v{version}{ec}_mask{mask}_s{args.size}_{i}_{variant}"
            if variant == "gold":
                write_svg(out / "svg" / f"{stem}_a_black.svg", blk, gld & False, args.size_mm)
                write_svg_modules(out / "svg" / f"{stem}_b_gold.svg", gld, args.size_mm, GOLD)
            else:
                write_svg(out / "svg" / f"{stem}.svg", blk, gld, args.size_mm)
            img = make_png(blk, gld)
            img.save(out / "png" / f"{stem}.png")
            res = decodes(img, url)
            ok.append("?" if res is None else "yes" if res else "NO")
        print(f"  {changes:>7}  {version:>7}  {ec:>2} {mask:>4} {dx:>2} {dy:>2}  {black:>2}/{int(sym.sum()):<2}  "
              f"{white:>2}/{int(sur.sum()):<2}  {'/'.join(ok):<23}  {url}")
        mod = args.size_mm / len(matrix)
        print(f"           gold part: {(2 * dx + args.size) * mod:.2f} x {args.size * mod:.2f} mm, "
              f"centre {dy * mod:.2f} mm below the centre of the black part, not shifted sideways")
    print(f"Files written to {out.resolve()}")


if __name__ == "__main__":
    main()
