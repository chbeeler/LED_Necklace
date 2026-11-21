#!/usr/bin/env python3
"""Search for QR codes whose modules already draw a minus and a plus sign.

For every URL, QR version and mask pattern the script counts how many modules
of a "-" (left) and "+" (right) symbol are already black and how many modules
directly around the symbols are already white. Every code that needs fewer
changes than the limit of its symbol size (--below) is written to a CSV file,
sorted by the number of changes. make_symbol_qr.py turns rows of that CSV
into SVG and PNG files.

CSV columns:
  changes           modules to change (missing symbol + black surround modules)
  size              symbol size in modules
  version, modules  QR version and its modules per side
  ec                error correction level (L 7%, M 15%, Q 25%, H 30%)
  module_mm         size of one module
  url, id           the encoded URL and its number
  https, n_path     yes/no: URL starts with https://, URL contains /n/
  mask              QR mask pattern 0-7
  dx, dy            symbol centre offset from the centre module, in modules
  x_mm, y_mm        the same offset in mm
  black, symbol_modules     matching / total symbol modules
  white, surround_modules   matching / total surround modules

Example:
  python find_symbol_qr.py                                  # numbers 000 to 999
  python find_symbol_qr.py --start 1000 --end 4999 --jobs 8
  python find_symbol_qr.py --start 0 --end 9999 --digits 4 --no-scheme
  python find_symbol_qr.py --versions 4 10 11 --below 5:12 7:23 9:40
  python find_symbol_qr.py --versions 3 --ec Q M            # 29 modules
"""
import argparse
import csv
from multiprocessing import Pool

from qrcode.exceptions import DataOverflowError

from qr_symbol_common import count_changes, modules, qr_matrices, symbol_masks

FIELDS = ["changes", "size", "version", "ec", "modules", "module_mm", "url", "id", "https", "n_path", "mask",
          "dx", "dy", "x_mm", "y_mm", "black", "symbol_modules", "white", "surround_modules"]

_cfg = {}


def build_targets(versions, limits, size_mm, x_mm, y_mm, search, verbose=False):
    """Return {version: [(size, dx, dy, symbol, surround), ...]}."""
    targets = {}
    for version in versions:
        n = modules(version)
        mod = size_mm / n
        dx0, dy0 = round(x_mm / mod), round(y_mm / mod)
        rng = range(-search, search + 1)
        cand = [(dx0 + i, dy0 + j) for i in rng for j in rng]
        if verbose:
            print(f"Version {version}: {n} modules of {mod:.3f} mm. Nominal position {dx0} modules "
                  f"({dx0 * mod:.2f} mm) sideways, {dy0} modules ({dy0 * mod:.2f} mm) below centre.")
        targets[version] = []
        for size in limits:
            usable = [(dx, dy) for dx, dy in cand if symbol_masks(n, dx, dy, size) is not None]
            skipped = sorted({dx for dx, _ in cand} - {dx for dx, _ in usable})
            if verbose:
                note = f", x offsets {skipped} skipped (symbol would stick out)" if skipped else ""
                print(f"  size {size} ({size * mod:.1f} mm): x offsets {sorted({dx for dx, _ in usable})}, "
                      f"y offsets {sorted({dy for _, dy in usable})}{note}")
            targets[version] += [(size, dx, dy, *symbol_masks(n, dx, dy, size)) for dx, dy in usable]
    return targets


def _init(targets, limits, ecs):
    _cfg.update(targets=targets, limits=limits, ecs=ecs)


def _score(item):
    """All hits for one URL as CSV row tuples (without the mm columns).

    For every version, error correction level, size and mask the best position
    is taken, and it is a hit when its changes are below the limit of that size.
    Combinations the URL does not fit into are skipped.
    """
    number, url = item
    hits = []
    for version, targets in _cfg["targets"].items():
        for ec in _cfg["ecs"]:
            try:
                mats = qr_matrices(url, version, ec)
            except DataOverflowError:
                continue
            for mask, a in mats.items():
                best = {}
                for size, dx, dy, sym, sur in targets:
                    black, white, changes = count_changes(a, sym, sur)
                    key = (changes, -black)
                    if size not in best or key < best[size][0]:
                        best[size] = (key, (changes, size, version, ec, url, number, mask, dx, dy,
                                            black, int(sym.sum()), white, int(sur.sum())))
                hits += [row for size, (key, row) in best.items() if key[0] < _cfg["limits"][size]]
    return hits


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
    p.add_argument("--versions", type=int, nargs="+", default=[4],
                   help="QR versions to try (default 4 = 33 modules; 3 = 29 modules, 10 = 57 modules, "
                        "11 = 61 modules)")
    p.add_argument("--ec", nargs="+", choices=["L", "M", "Q", "H"], default=["H"],
                   help="error correction levels to try (default H). Small versions only hold the URL "
                        "at a lower level, e.g. version 3 needs Q or M")
    p.add_argument("--below", nargs="+", default=["7:23", "5:12"], metavar="SIZE:CHANGES",
                   help="symbol sizes in modules (odd) and their limit: every code that needs fewer "
                        "changes than this goes into the CSV (default: 7:23 5:12)")
    p.add_argument("--size-mm", type=float, default=28.0, help="QR size without quiet zone")
    p.add_argument("--x-mm", type=float, default=12.0, help="symbol centre offset left/right of centre")
    p.add_argument("--y-mm", type=float, default=3.0, help="symbol centre offset below centre")
    p.add_argument("--search", type=int, default=1, help="try +/- this many modules around the nominal position")
    p.add_argument("--jobs", type=int, default=1, help="worker processes")
    p.add_argument("-o", "--out", default="qr_candidates.csv", help="output CSV file")
    args = p.parse_args()
    try:
        limits = {int(size): int(limit) for size, limit in (item.split(":") for item in args.below)}
    except ValueError:
        p.error("--below expects SIZE:CHANGES pairs, e.g. 7:23 5:12")
    if args.end < args.start:
        p.error("--end must not be smaller than --start.")

    targets = build_targets(args.versions, limits, args.size_mm, args.x_mm, args.y_mm, args.search, verbose=True)
    if not any(targets.values()):
        raise SystemExit("No usable position.")

    numbers = [f"{i:0{args.digits}d}" for i in range(args.start, args.end + 1)]
    templates = list(args.url)
    if args.no_scheme:
        templates += [t.partition("://")[2] for t in args.url if "://" in t]
    items = list(dict.fromkeys((n, t.format(n=n)) for t in templates for n in numbers))
    print(f"Searching {len(numbers)} numbers ({numbers[0]} to {numbers[-1]}) x {len(templates)} URL forms "
          f"x {len(args.versions)} versions x {len(args.ec)} error correction levels x 8 masks ...")

    if args.jobs > 1:
        with Pool(args.jobs, initializer=_init, initargs=(targets, limits, args.ec)) as pool:
            hits = [h for res in pool.imap_unordered(_score, items, chunksize=16) for h in res]
    else:
        _init(targets, limits, args.ec)
        hits = [h for item in items for h in _score(item)]
    hits.sort()

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(FIELDS)
        for changes, size, version, ec, url, number, mask, dx, dy, black, n_sym, white, n_sur in hits:
            n = modules(version)
            mod = args.size_mm / n
            w.writerow([changes, size, version, ec, n, f"{mod:.3f}", url, number,
                        "yes" if url.startswith("https://") else "no", "yes" if "/n/" in url else "no",
                        mask, dx, dy, f"{dx * mod:.2f}", f"{dy * mod:.2f}", black, n_sym, white, n_sur])

    for version in args.versions:
        for ec in args.ec:
            for size, limit in limits.items():
                rows = [h for h in hits if h[1] == size and h[2] == version and h[3] == ec]
                best = f", best {rows[0][0]} ({rows[0][4]}, mask {rows[0][6]})" if rows else ""
                print(f"Version {version}-{ec}, size {size}: {len(rows)} codes with fewer than {limit} changes{best}")
    print(f"{len(hits)} rows written to {args.out}")


if __name__ == "__main__":
    main()
