#!/usr/bin/env python3
import argparse
import math
from pathlib import Path
import qrcode
from qrcode.constants import ERROR_CORRECT_H


def build_qr_matrix(url: str, version: int = 4):
    qr = qrcode.QRCode(version=version, error_correction=ERROR_CORRECT_H, box_size=10, border=4)
    qr.add_data(url)
    qr.make(fit=False)
    return qr.get_matrix()


def rect_path(x, y, w, h):
    return f"M {x:.4f},{y:.4f} H {x+w:.4f} V {y+h:.4f} H {x:.4f} Z"


def plus_minus_cutouts(mod, centers):
    """Return SVG path data for white cutout circles and signs.
    centers = [(row_center, col_center, kind), ...]
    kind in {'plus','minus'}
    """
    pieces = []
    circle_r = 1.25 * mod
    # thinner sign than circle diameter for decent visibility
    horiz_w = 1.55 * mod
    vert_h = 1.55 * mod
    bar_th = 0.42 * mod

    for row_c, col_c, kind in centers:
        cx = col_c * mod
        cy = row_c * mod
        # circle path using arc commands
        pieces.append(
            f"M {cx+circle_r:.4f},{cy:.4f} "
            f"A {circle_r:.4f},{circle_r:.4f} 0 1 0 {cx-circle_r:.4f},{cy:.4f} "
            f"A {circle_r:.4f},{circle_r:.4f} 0 1 0 {cx+circle_r:.4f},{cy:.4f} Z"
        )
        # minus bar
        pieces.append(rect_path(cx - horiz_w/2, cy - bar_th/2, horiz_w, bar_th))
        if kind == 'plus':
            pieces.append(rect_path(cx - bar_th/2, cy - vert_h/2, bar_th, vert_h))
    return " ".join(pieces)


def rounded_rect_path(x, y, w, h, radius, reverse=False):
    """Rounded rectangle subpath. Reverse winding for a true SVG hole."""
    r = min(radius, w / 2, h / 2)
    if not reverse:
        return (
            f"M {x+r:.4f},{y:.4f} "
            f"H {x+w-r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x+w:.4f},{y+r:.4f} "
            f"V {y+h-r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x+w-r:.4f},{y+h:.4f} "
            f"H {x+r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x:.4f},{y+h-r:.4f} "
            f"V {y+r:.4f} A {r:.4f},{r:.4f} 0 0 1 {x+r:.4f},{y:.4f} Z"
        )
    return (
        f"M {x+r:.4f},{y:.4f} "
        f"A {r:.4f},{r:.4f} 0 0 0 {x:.4f},{y+r:.4f} "
        f"V {y+h-r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x+r:.4f},{y+h:.4f} "
        f"H {x+w-r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x+w:.4f},{y+h-r:.4f} "
        f"V {y+r:.4f} A {r:.4f},{r:.4f} 0 0 0 {x+w-r:.4f},{y:.4f} "
        f"H {x+r:.4f} Z"
    )


def finder_svg(x, y, mod):
    # The middle contour is a real hole, NOT a white filled rectangle.
    outer = rounded_rect_path(x, y, 7*mod, 7*mod, 1.2*mod)
    hole = rounded_rect_path(x+mod, y+mod, 5*mod, 5*mod, 0.9*mod, reverse=True)
    center = rounded_rect_path(x+2*mod, y+2*mod, 3*mod, 3*mod, 0.65*mod)
    return f'<path d="{outer} {hole} {center}" fill="black" fill-rule="nonzero"/>'


def generate_svg(url: str, size_mm: float = 28.0, with_markers: bool = False):
    matrix = build_qr_matrix(url)
    n = len(matrix)
    mod = size_mm / n

    finder_areas = [(4, 4), (4, n - 11), (n - 11, 4)]

    def in_finder(r, c):
        return any(rr <= r < rr + 7 and cc <= c < cc + 7 for rr, cc in finder_areas)

    # marker centers moved 3 modules inward from previous test
    # centers listed as (row_center, col_center, kind)
    marker_centers = [(20.5, 10.5, 'minus'), (20.5, 30.5, 'plus')]
    marker_radius_modules = 1.55

    def in_marker(r, c):
        if not with_markers:
            return False
        yc = r + 0.5
        xc = c + 0.5
        for cy, cx, _kind in marker_centers:
            if (xc - cx) ** 2 + (yc - cy) ** 2 <= marker_radius_modules ** 2:
                return True
        return False

    module_paths = []
    for r, row in enumerate(matrix):
        for c, val in enumerate(row):
            if not val:
                continue
            if in_finder(r, c) or in_marker(r, c):
                continue
            x = c * mod
            y = r * mod
            module_paths.append(rect_path(x, y, mod, mod))

    finder_shapes = []
    for rr, cc in finder_areas:
        finder_shapes.append(finder_svg(cc * mod, rr * mod, mod))

    marker_cutout = ''
    if with_markers:
        marker_cutout = (
            f'<path d="{plus_minus_cutouts(mod, marker_centers)}" fill="white"/>'
        )

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{size_mm}mm" height="{size_mm}mm" viewBox="0 0 {size_mm} {size_mm}">
  <g fill="black">
    <path d="{' '.join(module_paths)}"/>
    {''.join(finder_shapes)}
  </g>
  {marker_cutout}
</svg>
'''
    return svg


def main():
    parser = argparse.ArgumentParser(description='Generate a 28x28 mm SVG QR code for Spirluma-style prints.')
    parser.add_argument('url', help='URL to encode')
    parser.add_argument('--size-mm', type=float, default=28.0, help='Final QR size including quiet zone (default: 28 mm)')
    parser.add_argument('--markers', action='store_true', help='Add minus/plus button markers')
    parser.add_argument('-o', '--output', default='qr.svg', help='Output SVG path')
    args = parser.parse_args()

    svg = generate_svg(args.url, size_mm=args.size_mm, with_markers=args.markers)
    Path(args.output).write_text(svg, encoding='utf-8')
    print(f'Wrote {args.output}')


if __name__ == '__main__':
    main()
