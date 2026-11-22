"""Shared code for find_symbol_qr.py (search) and make_symbol_qr.py (output).

A QR code of version v has 17 + 4*v modules per side. The symbols are a "-"
left of the centre and a "+" right of it, with bars one module wide.
"""
import numpy as np
import qrcode
from PIL import Image, ImageDraw
from qrcode.constants import ERROR_CORRECT_H, ERROR_CORRECT_L, ERROR_CORRECT_M, ERROR_CORRECT_Q

GOLD = "#D4AF37"
# Error correction levels: share of the code that may be damaged or changed.
EC_LEVELS = {"L": ERROR_CORRECT_L, "M": ERROR_CORRECT_M, "Q": ERROR_CORRECT_Q, "H": ERROR_CORRECT_H}


def modules(version):
    return 17 + 4 * version


def qr_matrices(url, version, ec="H"):
    """Return {mask: bool array} for all 8 mask patterns.

    Raises qrcode.exceptions.DataOverflowError if the URL does not fit.
    """
    qr = qrcode.QRCode(version=version, error_correction=EC_LEVELS[ec], border=0)
    qr.add_data(url)
    qr.make(fit=False)
    out = {}
    for mask in range(8):
        qr.makeImpl(False, mask)
        out[mask] = np.array(qr.modules, dtype=bool)
    return out


def symbol_masks(n, dx, dy, size):
    """Return (symbol, surround) bool arrays, or None if a symbol leaves the code.

    n = modules per side. Minus at (-dx, dy) and plus at (+dx, dy): offsets of
    the symbol centres from the centre module, in modules (dy positive = below
    centre). Bars are one module wide and `size` modules long. The surround is
    every module that touches a symbol module (also diagonally); parts outside
    the code are ignored because the quiet zone is white anyway.
    """
    half = size // 2
    centre = n // 2
    sym = np.zeros((n, n), dtype=bool)
    row = centre + dy
    for col, plus in ((centre - dx, False), (centre + dx, True)):
        if col - half < 0 or col + half >= n or row - half < 0 or row + half >= n:
            return None
        sym[row, col - half:col + half + 1] = True
        if plus:
            sym[row - half:row + half + 1, col] = True
    grown = np.zeros_like(sym)
    padded = np.pad(sym, 1)
    for i in range(3):
        for j in range(3):
            grown |= padded[i:i + n, j:j + n]
    return sym, grown & ~sym


def count_changes(matrix, sym, sur):
    """Return (black, white, changes): matching symbol modules, matching
    surround modules and the number of modules that have to be changed."""
    black = int((matrix & sym).sum())
    white = int((~matrix & sur).sum())
    return black, white, int(sym.sum()) - black + int(sur.sum()) - white


# ----------------------------------------------------------------- rendering
def finders(n):
    return [(0, 0), (0, n - 7), (n - 7, 0)]  # (row, col)


def in_finder(n, r, c):
    return any(rr <= r < rr + 7 and cc <= c < cc + 7 for rr, cc in finders(n))


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
    n = len(black)
    mod = size_mm / n
    blk = [rect_path(c * mod, r * mod, mod, mod)
           for r in range(n) for c in range(n) if black[r, c] and not in_finder(n, r, c)]
    gld = [rect_path(c * mod, r * mod, mod, mod)
           for r in range(n) for c in range(n) if gold[r, c]]
    shapes = []
    for rr, cc in finders(n):
        x, y = cc * mod, rr * mod
        shapes.append(rounded_rect_path(x, y, 7 * mod, 7 * mod, 1.2 * mod))
        shapes.append(rounded_rect_path(x + mod, y + mod, 5 * mod, 5 * mod, 0.9 * mod, reverse=True))
        shapes.append(rounded_rect_path(x + 2 * mod, y + 2 * mod, 3 * mod, 3 * mod, 0.65 * mod))
    gold_el = f'\n  <path d="{" ".join(gld)}" fill="{GOLD}"/>' if gld else ""
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size_mm}mm" height="{size_mm}mm" '
        f'viewBox="0 0 {size_mm} {size_mm}">\n'
        f'  <path d="{" ".join(blk)}" fill="black"/>\n'
        f'  <path d="{" ".join(shapes)}" fill="black" fill-rule="nonzero"/>{gold_el}\n'
        f'</svg>\n', encoding="utf-8")


def write_svg_modules(path, layer, size_mm, color):
    """Write only the given modules in one colour (no finder patterns).

    Same page size and coordinates as write_svg, so the file lines up with
    the black one when both keep their position.
    """
    n = len(layer)
    mod = size_mm / n
    d = [rect_path(c * mod, r * mod, mod, mod) for r in range(n) for c in range(n) if layer[r, c]]
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{size_mm}mm" height="{size_mm}mm" '
        f'viewBox="0 0 {size_mm} {size_mm}">\n'
        f'  <path d="{" ".join(d)}" fill="{color}"/>\n'
        f'</svg>\n', encoding="utf-8")


def make_png(black, gold, px=20, quiet=4):
    n = len(black)
    size = (n + 2 * quiet) * px
    img = Image.new("RGB", (size, size), "white")
    d = ImageDraw.Draw(img)
    o = quiet * px
    for r in range(n):
        for c in range(n):
            if gold[r, c] or (black[r, c] and not in_finder(n, r, c)):
                d.rectangle([o + c * px, o + r * px, o + (c + 1) * px - 1, o + (r + 1) * px - 1],
                            fill=GOLD if gold[r, c] else "black")
    for rr, cc in finders(n):
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
