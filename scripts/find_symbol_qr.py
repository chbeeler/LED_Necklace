#!/usr/bin/env python3
"""Search for QR codes whose modules already draw a minus and a plus sign.

For every URL number and every one of the 8 QR mask patterns the script counts
how many modules of a "-" (left) and "+" (right) symbol are already black and
how many modules directly around the symbols are already white. Every code
that needs fewer changes than the limit of its symbol size (--below) is kept.

These codes are written to <out>/svg (28 mm, no quiet zone) and
<out>/png (with quiet zone, for on-screen scanning) in three variants:

File names are qr_<changes>_<id>_mask<mask>_s<size>_<n>_<variant>, where <id> is the
URL path with "/" written as "-" (n-123 for /n/123, 123 for /123):

  *_1_raw     the unmodified QR code
  *_2_filled  symbol modules black, surround white
  *_3_gold    like filled, but the symbol modules are gold

Example:
  python find_symbol_qr.py                                  # numbers 000 to 999
  python find_symbol_qr.py --start 1000 --end 4999 --jobs 8
  python find_symbol_qr.py --start 0 --end 9999 --digits 4  # 0000 to 9999
"""
import argparse
from multiprocessing import Pool
from pathlib import Path

import numpy as np
import qrcode
from PIL import Image, ImageDraw
from qrcode.constants import ERROR_CORRECT_H
from qrcode.exceptions import DataOverflowError

VERSION = 4
N = 17 + 4 * VERSION          # 33 modules
CENTER = N // 2               # module index 16 is the centre module
GOLD = "#D4AF37"
FINDERS = [(0, 0), (0, N - 7), (N - 7, 0)]  # (row, col) of the finder patterns


# ----------------------------------------------------------------- QR matrix
def qr_matrices(url):
    """Return {mask: 33x33 bool array} for all 8 mask patterns."""
    qr = qrcode.QRCode(version=VERSION, error_correction=ERROR_CORRECT_H, border=0)
    qr.add_data(url)
    qr.make(fit=False)
    out = {}
    for mask in range(8):
        qr.makeImpl(False, mask)
        out[mask] = np.array(qr.modules, dtype=bool)
    return out


# ------------------------------------------------------------------- symbols
def symbol_masks(dx, dy, size):
    """Return (symbol, surround) bool arrays, or None if a symbol leaves the code.

    Minus at (-dx, dy) and plus at (+dx, dy): offsets of the symbol centres
    from the centre module, in modules (dy positive = below centre). Bars are
    one module wide and `size` modules long. The surround is every module that
    touches a symbol module (also diagonally); parts outside the code are
    ignored because the quiet zone is white anyway.
    """
    half = size // 2
    sym = np.zeros((N, N), dtype=bool)
    row = CENTER + dy
    for col, plus in ((CENTER - dx, False), (CENTER + dx, True)):
        if col - half < 0 or col + half >= N or row - half < 0 or row + half >= N:
            return None
        sym[row, col - half:col + half + 1] = True
        if plus:
            sym[row - half:row + half + 1, col] = True
    grown = np.zeros_like(sym)
    padded = np.pad(sym, 1)
    for i in range(3):
        for j in range(3):
            grown |= padded[i:i + N, j:j + N]
    return sym, grown & ~sym


def positions(size_mm, x_mm, y_mm, search):
    mod = size_mm / N
    dx0, dy0 = round(x_mm / mod), round(y_mm / mod)
    rng = range(-search, search + 1)
    return [(dx0 + i, dy0 + j) for i in rng for j in rng], (dx0, dy0)


# -------------------------------------------------------------------- search
_cfg = {}


def _init(targets, limits):
    _cfg.update(targets=targets, limits=limits)


def _score(url):
    """All hits for one URL: [(changes, -black, size, url, mask, dx, dy)].

    changes = missing symbol modules + surround modules that are black.
    For every size and mask the best position is taken, and it is a hit when
    its changes are below the limit of that size.
    """
    try:
        mats = qr_matrices(url)
    except DataOverflowError:
        return []
    hits = []
    for mask, a in mats.items():
        best = {}
        for size, dx, dy, sym, sur in _cfg["targets"]:
            black = int((a & sym).sum())
            changes = (int(sym.sum()) - black) + int((a & sur).sum())
            key = (changes, -black)
            if size not in best or key < best[size][:2]:
                best[size] = (changes, -black, size, url, mask, dx, dy)
        hits += [b for size, b in best.items() if b[0] < _cfg["limits"][size]]
    return hits


def search(urls, targets, limits, jobs):
    """Return {size: [(url, mask, dx, dy), ...]} of all hits, fewest changes first."""
    args = (targets, limits)
    if jobs > 1:
        pool = Pool(jobs, initializer=_init, initargs=args)
        results = pool.imap_unordered(_score, urls, chunksize=16)
    else:
        _init(*args)
        pool, results = None, map(_score, urls)
    found = {}
    for res in results:
        for hit in res:
            found.setdefault(hit[2], []).append(hit)
    if pool:
        pool.close()
    return {size: [h[3:] for h in sorted(hits)] for size, hits in found.items()}


# ----------------------------------------------------------------- rendering
def in_finder(r, c):
    return any(rr <= r < rr + 7 and cc <= c < cc + 7 for rr, cc in FINDERS)


def rect_path(x, y, w, h):
    return f"M {x:.4f},{y:.4f} H {x + w:.4f} V {y + h:.4f} H {x:.4f} Z"


def rounded_rect_path(x, y, w, h, radius, reverse=False):
    r = min(radius, w / 2, h / 2)
    if not reverse:
        return (f"M {x + r:.4f},{y:.4f} H {x + w - r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x + w:.4f},{y + r:.4f} "
                f"V {y + h - r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x + w - r:.4f},{y + h:.4f} "
                f"H {x + r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x:.4f},{y + h - r:.4f} "
                f"V {y + r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x + r:.4f},{y:.4f} Z")
    return (f"M {x + r:.4f},{y:.4f} A {r:.4f},{r:.4f} 0 0 0 {x:.4f},{y + r:.4f} "
            f"V {y + h - r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x + r:.4f},{y + h:.4f} "
            f"H {x + w - r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x + w:.4f},{y + h - r:.4f} "
            f"V {y + r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x + w - r:.4f},{y:.4f} H {x + r:.4f} Z")


def variant_layers(matrix, sym, sur, variant):
    """Return (black, gold) bool arrays for raw / filled / gold."""
    none = np.zeros_like(matrix)
    if variant == "raw":
        return matrix, none
    cleaned = matrix & ~sur
    if variant == "filled":
        return cleaned | sym, none
    return cleaned & ~sym, sym


def write_svg(path, black, gold, size_mm):
    mod = size_mm / N
    blk = [rect_path(c * mod, r * mod, mod, mod)
           for r in range(N) for c in range(N) if black[r, c] and not in_finder(r, c)]
    gld = [rect_path(c * mod, r * mod, mod, mod)
           for r in range(N) for c in range(N) if gold[r, c]]
    finders = []
    for rr, cc in FINDERS:
        x, y = cc * mod, rr * mod
        finders.append(rounded_rect_path(x, y, 7 * mod, 7 * mod, 1.2 * mod))
        finders.append(rounded_rect_path(x + mod, y + mod, 5 * mod, 5 * mod, 0.9 * mod, reverse=True))
        finders.append(rounded_rect_path(x + 2 * mod, y + 2 * mod, 3 * mod, 3 * mod, 0.65 * mod))
    gold_el = f'\n  <path d="{" ".join(gld)}" fill="{GOLD}"/>' if gld else ""
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size_mm}mm" height="{size_mm}mm" '
        f'viewBox="0 0 {size_mm} {size_mm}">\n'
        f'  <path d="{" ".join(blk)}" fill="black"/>\n'
        f'  <path d="{" ".join(finders)}" fill="black" fill-rule="nonzero"/>{gold_el}\n'
        f'</svg>\n', encoding="utf-8")


def make_png(black, gold, px=20, quiet=4):
    size = (N + 2 * quiet) * px
    img = Image.new("RGB", (size, size), "white")
    d = ImageDraw.Draw(img)
    o = quiet * px
    for r in range(N):
        for c in range(N):
            if gold[r, c] or (black[r, c] and not in_finder(r, c)):
                d.rectangle([o + c * px, o + r * px, o + (c + 1) * px - 1, o + (r + 1) * px - 1],
                            fill=GOLD if gold[r, c] else "black")
    for rr, cc in FINDERS:
        x, y = o + cc * px, o + rr * px
        d.rounded_rectangle([x, y, x + 7 * px - 1, y + 7 * px - 1], radius=1.2 * px, fill="black")
        d.rounded_rectangle([x + px, y + px, x + 6 * px - 1, y + 6 * px - 1], radius=0.9 * px, fill="white")
        d.rounded_rectangle([x + 2 * px, y + 2 * px, x + 5 * px - 1, y + 5 * px - 1], radius=0.65 * px, fill="black")
    return img


def decodes(img, url):
    """True/False if a decoder is installed, None otherwise."""
    try:
        import zxingcpp
    except ImportError:
        return None
    return any(r.text == url for r in zxingcpp.read_barcodes(img))


# ---------------------------------------------------------------------- main
def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", nargs="+",
                   default=["https://spirluma.pages.dev/n/{n}", "https://spirluma.pages.dev/{n}"],
                   help="one or more URL templates, {n} = number (default: both /n/{n} and /{n})")
    p.add_argument("--no-scheme", action="store_true",
                   help="also try every URL without the leading https:// (some scanners then show plain text)")
    p.add_argument("--start", type=int, default=0, help="first number to try (default 0)")
    p.add_argument("--end", type=int, default=999, help="last number to try, inclusive (default 999)")
    p.add_argument("--digits", type=int, default=3, help="pad numbers with leading zeros to this many digits (default 3)")
    p.add_argument("--below", nargs="+", default=["7:23", "5:12"], metavar="SIZE:CHANGES",
                   help="symbol sizes in modules (odd) and their limit: every code that needs fewer "
                        "changes than this is written (default: 7:23 5:12)")
    p.add_argument("--size-mm", type=float, default=28.0, help="QR size without quiet zone")
    p.add_argument("--x-mm", type=float, default=12.0, help="symbol centre offset left/right of centre")
    p.add_argument("--y-mm", type=float, default=3.0, help="symbol centre offset below centre")
    p.add_argument("--search", type=int, default=1, help="try +/- this many modules around the nominal position")
    p.add_argument("--jobs", type=int, default=1, help="worker processes")
    p.add_argument("-o", "--out", default="qr_out", help="output directory")
    args = p.parse_args()
    try:
        limits = {int(size): int(limit) for size, limit in (item.split(":") for item in args.below)}
    except ValueError:
        p.error("--below expects SIZE:CHANGES pairs, e.g. 7:23 5:12")

    mod = args.size_mm / N
    cand, (dx0, dy0) = positions(args.size_mm, args.x_mm, args.y_mm, args.search)
    print(f"Module size {mod:.3f} mm. Nominal position: {dx0} modules ({dx0 * mod:.2f} mm) sideways, "
          f"{dy0} modules ({dy0 * mod:.2f} mm) below centre.")
    targets = []
    for size in limits:
        usable = [(dx, dy) for dx, dy in cand if symbol_masks(dx, dy, size) is not None]
        print(f"Size {size}: positions searched (dx, dy) = {usable}")
        skipped = sorted({dx for dx, _ in cand} - {dx for dx, _ in usable})
        if skipped:
            print(f"         x offsets {skipped} skipped, the symbol would stick out of the code")
        targets += [(size, dx, dy, *symbol_masks(dx, dy, size)) for dx, dy in usable]
    if not targets:
        raise SystemExit("No usable position.")

    if args.end < args.start:
        raise SystemExit("--end must not be smaller than --start.")
    numbers = [f"{i:0{args.digits}d}" for i in range(args.start, args.end + 1)]
    templates = list(args.url)
    if args.no_scheme:
        templates += [t.partition("://")[2] for t in args.url if "://" in t]
    urls = list(dict.fromkeys(t.format(n=n) for t in templates for n in numbers))
    print(f"Searching {len(numbers)} numbers ({numbers[0]} to {numbers[-1]}) x {len(templates)} URL forms x 8 masks ...")
    best = search(urls, targets, limits, args.jobs)

    out = Path(args.out)
    for sub in ("svg", "png"):
        (out / sub).mkdir(parents=True, exist_ok=True)
    for size, limit in limits.items():
        print(f"\nSize {size}x{size}: {len(best.get(size, []))} codes with fewer than {limit} changes")
        print("  mask dx dy  black  white  changes  decodes raw/filled/gold  url")
        for url, mask, dx, dy in best.get(size, []):
            matrix = qr_matrices(url)[mask]
            sym, sur = symbol_masks(dx, dy, size)
            n_sym, n_sur = int(sym.sum()), int(sur.sum())
            black, white = int((matrix & sym).sum()), int((~matrix & sur).sum())
            changes = n_sym - black + n_sur - white
            # File name: qr_<changes>_<id>_mask<mask>_s<size>_<n>_<variant>. The id is
            # the URL path with "/" written as "-", e.g. "n-123" for /n/123.
            # URLs without https:// get "-noscheme" appended.
            scheme, sep, rest = url.rpartition("://")
            ident = rest.partition("/")[2].replace("/", "-") + ("" if sep else "-noscheme")
            ok = []
            # The variants are numbered so that raw sorts before filled and gold.
            for i, variant in enumerate(("raw", "filled", "gold"), 1):
                blk, gld = variant_layers(matrix, sym, sur, variant)
                stem = f"qr_{changes:02d}_{ident}_mask{mask}_s{size}_{i}_{variant}"
                write_svg(out / "svg" / f"{stem}.svg", blk, gld, args.size_mm)
                img = make_png(blk, gld)
                img.save(out / "png" / f"{stem}.png")
                r = decodes(img, url)
                ok.append("?" if r is None else "yes" if r else "NO")
            print(f"  {mask:>4} {dx:>2} {dy:>2}  {black:>2}/{n_sym:<2}  {white:>2}/{n_sur:<2}  "
                  f"{changes:>7}  {'/'.join(ok):<23}  {url}")
    print(f"\nFiles written to {out.resolve()}")


if __name__ == "__main__":
    main()
