"""Potluck's one mark (the owner's design, 2026-10-15): a disc of three segments -- blue, amber, green -- parted by
white gaps, a white board in each: three boards, one whole. Drawn from one geometry for every place it appears: the
dashboard and the setup wizard (SVG), Home Assistant's integration icon (PNG), and the tray icon (ICO), where a small
badge in the corner says the cell's state. Below 48 pixels the boards are left out and the segments carry the mark.
No packages: the PNGs and the ICO are rasterised here, supersampled for smooth edges.

    python -m potluck.brandmark            # regenerate every file from this geometry
"""

from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

# The geometry, in a 64 x 64 box: a disc parted along three rays from its centre (up, lower left, lower right).
C, R, GAP = 32.0, 30.0, 2.6
SEGMENTS = (  # (colour, the angle its middle points to, degrees, 0 = right, counter-clockwise; y grows down)
    ((0x1E, 0x88, 0xE5), 150.0),   # blue, upper left
    ((0xF5, 0xB4, 0x2A), 30.0),    # amber, upper right
    ((0x7C, 0xC1, 0x3A), 270.0),   # green, below
)
RAYS = (90.0, 210.0, 330.0)
BOARD_AT = 15.0                    # how far from the centre each board sits
BOARD_W, BOARD_H = 11.5, 15.0
WHITE = (0xFF, 0xFF, 0xFF)
INK = (0x0B, 0x4A, 0x99)           # the wordmark's navy, for text beside the mark


def _rad(a: float) -> float:
    return math.radians(a)


def _segment_of(px: float, py: float) -> int | None:
    """Which segment a point of the disc belongs to, or None in a gap or outside."""
    dx, dy = px - C, C - py  # y up
    if dx * dx + dy * dy > R * R:
        return None
    for a in RAYS:  # inside a gap: near a ray, on its outward side
        ux, uy = math.cos(_rad(a)), math.sin(_rad(a))
        if dx * ux + dy * uy > 0 and abs(dx * uy - dy * ux) <= GAP / 2:
            return None
    ang = math.degrees(math.atan2(dy, dx)) % 360
    if 90 <= ang < 210:
        return 0
    if ang < 90 or ang >= 330:
        return 1
    return 2


def _board_centre(k: int) -> tuple[float, float]:
    a = _rad(SEGMENTS[k][1])
    return C + BOARD_AT * math.cos(a), C - BOARD_AT * math.sin(a)


def _board_lines(k: int) -> list[tuple]:
    """A board drawn as white strokes (rects and polylines), centred in segment k: the module's outline, its antenna's
    meander along the top, the chip, and three pins on each side. Coordinates in the 64-box."""
    cx, cy = _board_centre(k)
    x0, y0 = cx - BOARD_W / 2, cy - BOARD_H / 2
    shapes = [("rect", x0, y0, BOARD_W, BOARD_H, 1.0)]                       # outline (stroked)
    u = BOARD_W / 10.0  # the details were drawn for a 10-wide board
    m = [(x0 + 1.6 * u, y0 + 3.6 * u), (x0 + 1.6 * u, y0 + 1.7 * u), (x0 + 3.6 * u, y0 + 1.7 * u),
         (x0 + 3.6 * u, y0 + 3.2 * u), (x0 + 5.6 * u, y0 + 3.2 * u), (x0 + 5.6 * u, y0 + 1.7 * u),
         (x0 + 7.6 * u, y0 + 1.7 * u), (x0 + 7.6 * u, y0 + 3.2 * u), (x0 + 8.6 * u, y0 + 3.2 * u)]
    shapes.append(("poly", m))                                               # antenna meander
    shapes.append(("fill", cx - 2.0 * u, cy + 0.5 * u, 4.0 * u, 4.0 * u))    # chip
    for i in range(3):
        py = y0 + 6.2 * u + i * 2.4 * u
        shapes.append(("poly", [(x0 - 1.6, py), (x0, py)]))                  # pins, left
        shapes.append(("poly", [(x0 + BOARD_W, py), (x0 + BOARD_W + 1.6, py)]))  # pins, right
    return shapes


STROKE = 1.25


def _near_segment(px, py, ax, ay, bx, by) -> bool:
    vx, vy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy or 1)))
    qx, qy = ax + t * vx, ay + t * vy
    return (px - qx) ** 2 + (py - qy) ** 2 <= (STROKE / 2) ** 2


def _on_board(px: float, py: float, k: int) -> bool:
    for s in _board_lines(k):
        if s[0] == "rect":
            _, x, y, w, h, _r = s
            inside_outer = x - STROKE / 2 <= px <= x + w + STROKE / 2 and y - STROKE / 2 <= py <= y + h + STROKE / 2
            inside_inner = x + STROKE / 2 < px < x + w - STROKE / 2 and y + STROKE / 2 < py < y + h - STROKE / 2
            if inside_outer and not inside_inner:
                return True
        elif s[0] == "fill":
            _, x, y, w, h = s
            if x <= px <= x + w and y <= py <= y + h:
                return True
        else:
            pts = s[1]
            if any(_near_segment(px, py, *pts[i], *pts[i + 1]) for i in range(len(pts) - 1)):
                return True
    return False


def colour_at(px: float, py: float, boards: bool, grey: bool = False) -> tuple[int, int, int] | None:
    k = _segment_of(px, py)
    if k is None:
        return None
    if boards and _on_board(px, py, k):
        return WHITE
    col = SEGMENTS[k][0]
    if grey:
        g = round(0.3 * col[0] + 0.59 * col[1] + 0.11 * col[2])
        col = (g, g, g)
    return col


# ---- SVG --------------------------------------------------------------------------------------------------------

def svg(boards: bool = True, title: str = "Potluck") -> str:
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img" aria-label="{title}">',
             f"<title>{title}</title>"]
    bounds = {0: (90.0, 210.0), 1: (-30.0, 90.0), 2: (210.0, 330.0)}
    for k, (col, _mid) in enumerate(SEGMENTS):
        a0, a1 = bounds[k]
        p0 = (C + R * math.cos(_rad(a0)), C - R * math.sin(_rad(a0)))
        p1 = (C + R * math.cos(_rad(a1)), C - R * math.sin(_rad(a1)))
        parts.append(f'<path d="M{C} {C} L{p0[0]:.3f} {p0[1]:.3f} A{R} {R} 0 0 0 {p1[0]:.3f} {p1[1]:.3f} Z" '
                     f'fill="#%02x%02x%02x"/>' % col)
    for a in RAYS:  # the gaps, drawn as white strokes over the joins
        ex, ey = C + (R + 1) * math.cos(_rad(a)), C - (R + 1) * math.sin(_rad(a))
        parts.append(f'<line x1="{C}" y1="{C}" x2="{ex:.3f}" y2="{ey:.3f}" stroke="#fff" stroke-width="{GAP}"/>')
    if boards:
        for k in range(3):
            for s in _board_lines(k):
                if s[0] == "rect":
                    _, x, y, w, h, r = s
                    parts.append(f'<rect x="{x:.3f}" y="{y:.3f}" width="{w}" height="{h}" rx="{r}" fill="none" '
                                 f'stroke="#fff" stroke-width="{STROKE}"/>')
                elif s[0] == "fill":
                    _, x, y, w, h = s
                    parts.append(f'<rect x="{x:.3f}" y="{y:.3f}" width="{w}" height="{h}" fill="#fff"/>')
                else:
                    pts = " ".join(f"{x:.3f},{y:.3f}" for x, y in s[1])
                    parts.append(f'<polyline points="{pts}" fill="none" stroke="#fff" stroke-width="{STROKE}" '
                                 f'stroke-linecap="round" stroke-linejoin="round"/>')
    return "".join(parts) + "</svg>\n"


# ---- rasterising ------------------------------------------------------------------------------------------------

BADGES = {"ok": None, "warn": (0xE8, 0xA3, 0x17), "bad": (0xD3, 0x3B, 0x2F), "none": None}


def rgba(size: int, ss: int = 3, state: str | None = None) -> list[list[tuple[int, int, int, int]]]:
    """Rows top to bottom of (r, g, b, a). `state` (the tray): "none" greys the mark, "warn"/"bad" add a corner badge."""
    k = 64.0 / size
    boards = size >= 48
    badge = BADGES.get(state) if state else None
    rows = []
    for j in range(size):
        row = []
        for i in range(size):
            acc, hits = [0.0, 0.0, 0.0], 0
            for sj in range(ss):
                for si in range(ss):
                    px, py = (i + (si + 0.5) / ss) * k, (j + (sj + 0.5) / ss) * k
                    col = None
                    if badge is not None:
                        d2 = (px - 50.0) ** 2 + (py - 50.0) ** 2
                        if d2 <= 13.0 ** 2:
                            col = badge if d2 <= 10.5 ** 2 else WHITE
                    if col is None:
                        col = colour_at(px, py, boards, grey=(state == "none"))
                    if col is not None:
                        hits += 1
                        acc[0] += col[0]
                        acc[1] += col[1]
                        acc[2] += col[2]
            row.append((round(acc[0] / hits), round(acc[1] / hits), round(acc[2] / hits), round(255 * hits / ss / ss))
                       if hits else (0, 0, 0, 0))
        rows.append(row)
    return rows


def png(size: int) -> bytes:
    raw = b"".join(b"\x00" + b"".join(bytes(px) for px in row) for row in rgba(size))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ico(size: int = 32, state: str | None = None) -> bytes:
    """A one-image ICO (32-bit, bottom-up, alpha) -- the tray icon, its state as a badge."""
    rows = rgba(size, ss=4, state=state)
    pixels = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    mask = b"\x00" * (((size + 31) // 32) * 4 * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
    image = header + pixels + mask
    return struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", size, size, 0, 0, 1, 32, len(image), 22) + image


# ---- the files ----------------------------------------------------------------------------------------------------

def write_all(repo: Path) -> list[Path]:
    """Every place the mark appears: the dashboard's SVG (and favicon), Home Assistant's brand images. The mark has no
    dark-on-light parts, so one image serves light and dark themes."""
    out = []
    dash = repo / "host" / "potluck" / "potluck" / "dashboard"
    (dash / "potluck.svg").write_text(svg(), encoding="utf-8")
    (dash / "potluck-small.svg").write_text(svg(boards=False), encoding="utf-8")
    out += [dash / "potluck.svg", dash / "potluck-small.svg"]
    brand = repo / "custom_components" / "potluck" / "brand"
    brand.mkdir(parents=True, exist_ok=True)
    for name, size in (("icon.png", 256), ("icon@2x.png", 512)):
        (brand / name).write_bytes(png(size))
        out.append(brand / name)
    for stale in ("dark_icon.png", "dark_icon@2x.png"):  # the earlier pot needed a dark variant; this mark does not
        (brand / stale).unlink(missing_ok=True)
    return out


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[3]
    for p in write_all(root):
        print(p.relative_to(root))
    sys.exit(0)
