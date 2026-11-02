#!/usr/bin/env python3
"""JupiterBot's and the JMF Trainer's art: "Star chart at night", the B
direction of the 2026-11-01 mockups (Nigel: "B for SURE"), drawn as image
layers that the menu puts together at run time (src/ui/pages/jupiter_backdrop.cpp).

Original art in the language of Jupiter My Favourite (Akunakunn): a hand-inked
crescent-moon dial looking in from the top-right corner, scribbled one-stroke
pentagrams, filled stars and twinkles, blue square particles, and a near-black
skyline with lit windows over a thin yellow ground line, lemon yellow on an
ultramarine night. Nothing is traced or copied from the level, its videos or
RobTop's assets; the shapes are the mockup's (scratchpad draw_mock.py, art_b).

Every layer is drawn at 2 pixels a menu unit (the menu's first size is 720 x
500 units), supersampled, with straight alpha as ImGui blends it:

  jupiter_sky.png       the sky: its gradient, the nebula, a soft beam and a
                        faint astrolabe (opaque; cover-fitted to the window)
  jupiter_dial.png      the dial's still part: the crescent and its rings
  jupiter_dial_cw.png   the rings that turn clockwise: the dots and the bezel
  jupiter_dial_ccw.png  the rings that turn the other way: the dashed rings
  jupiter_city.png      the skyline: the sidebar's foot, a strip that repeats
                        along the bottom, the tower on the right edge
  jupiter_sprites.png   the stars, twinkles, a halo and a lit window

and src/ui/pages/jupiter_art.hpp, the tables that say where each goes.

Run from anywhere:  python -I tools/jupiter_art.py      (numpy and Pillow)
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources", "jupiter")
HEADER = os.path.join(ROOT, "src", "ui", "pages", "jupiter_art.hpp")

OUT = 2          # file pixels a menu unit
W, H = 720, 500  # the menu's first size (ui/shell.cpp): the layout's design space
PAGE_LEFT = 172  # the page's left edge there: the window's padding, the sidebar and a gap

YEL = "#F9F558"        # the level's line art
LIT = "#FCF378"        # a lit window
NAVY = "#06041F"       # the buildings
SLOT = "#1A12A6"       # an unlit window
HALO = "#4A41B8"       # the light round a scribbled star
PARTICLES = ("#1CA3C6", "#0EA5F9")  # teal, blue (even, odd)


def hexc(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def rng(seed):
    return np.random.default_rng(seed)


# --------------------------------------------------------------------------
# Drawing: a layer of art in menu units, and coverage masks drawn on it
# --------------------------------------------------------------------------

class Layer:
    """RGBA art over the rect (x0, y0, w, h) in menu units, drawn ss times
    finer than the file and averaged down. Kept premultiplied while drawing."""

    def __init__(self, x0, y0, w, h, ss=3):
        self.x0, self.y0, self.w, self.h = float(x0), float(y0), float(w), float(h)
        self.pw, self.ph = int(round(w * OUT)), int(round(h * OUT))
        assert abs(self.pw - w * OUT) < 1e-6 and abs(self.ph - h * OUT) < 1e-6, (w, h)
        self.ss = ss
        self.s = OUT * ss
        self.W, self.H = self.pw * ss, self.ph * ss
        self.rgb = np.zeros((self.H, self.W, 3), np.float32)
        self.a = np.zeros((self.H, self.W), np.float32)
        xs = self.x0 + (np.arange(self.W, dtype=np.float32) + 0.5) / self.s
        ys = self.y0 + (np.arange(self.H, dtype=np.float32) + 0.5) / self.s
        self.X, self.Y = np.meshgrid(xs, ys)

    def over(self, col, a, box=None):
        """Alpha-over `col` with coverage `a` (the whole layer, or a patch
        whose top-left pixel is box = (i, j))."""
        a = np.clip(a, 0.0, 1.0)
        if box is None:
            rgb, al = self.rgb, self.a
        else:
            i, j = box
            rgb = self.rgb[j:j + a.shape[0], i:i + a.shape[1]]
            al = self.a[j:j + a.shape[0], i:i + a.shape[1]]
        rgb *= (1.0 - a)[..., None]
        rgb += hexc(col) * a[..., None]
        al *= 1.0 - a
        al += a

    def image(self, opaque=False):
        ss = self.ss
        rgb = self.rgb.reshape(self.ph, ss, self.pw, ss, 3).mean(axis=(1, 3))
        a = self.a.reshape(self.ph, ss, self.pw, ss).mean(axis=(1, 3))
        if opaque:
            return Image.fromarray((np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
        straight = rgb / np.maximum(a, 1e-6)[..., None]
        straight[a <= 1e-6] = 0.0
        straight, a = bleed(np.clip(straight, 0, 1), np.clip(a, 0, 1))
        px = np.dstack([straight, a])
        return Image.fromarray((px * 255 + 0.5).astype(np.uint8), "RGBA")


def bleed(rgb, a, steps=3):
    """Gives the clear pixels next to the art the art's colour, so a texture
    filtered at its edges fades the art out rather than darkening it (a
    straight-alpha texture would mix in the clear pixels' black)."""
    have = a > 0.5 / 255.0
    rgb = rgb.copy()
    for _ in range(steps):
        sum_c = np.zeros_like(rgb)
        sum_w = np.zeros(a.shape, np.float32)
        w = have.astype(np.float32)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                sum_c += np.roll(np.roll(rgb * w[..., None], dy, 0), dx, 1)
                sum_w += np.roll(np.roll(w, dy, 0), dx, 1)
        fill = (~have) & (sum_w > 0)
        rgb[fill] = sum_c[fill] / sum_w[fill][:, None]
        have = have | fill
    return rgb, a


class Mask:
    """A coverage mask over a layer, drawn in menu units."""

    def __init__(self, layer):
        self.L = layer
        self.img = Image.new("L", (layer.W, layer.H), 0)
        self.d = ImageDraw.Draw(self.img)

    def P(self, p):
        return ((p[0] - self.L.x0) * self.L.s, (p[1] - self.L.y0) * self.L.s)

    def circle(self, c, r, v=255):
        x, y = self.P(c)
        s = self.L.s
        self.d.ellipse([x - r * s, y - r * s, x + r * s, y + r * s], fill=v)

    def poly(self, pts, v=255):
        self.d.polygon([self.P(p) for p in pts], fill=v)

    def line(self, pts, w, v=255, closed=False, caps=True):
        P = [self.P(p) for p in pts]
        if closed:
            P = P + [P[0], P[1]]
        self.d.line(P, fill=v, width=max(1, int(round(w * self.L.s))), joint="curve")
        if caps and not closed:
            for p in (pts[0], pts[-1]):
                self.circle(p, w / 2.0, v)

    def arc(self, c, r, a0, a1, w, v=255):
        """Degrees, 0 = +x, increasing clockwise on screen (y down)."""
        x, y = self.P(c)
        s = self.L.s
        ro = r + w / 2.0
        self.d.arc([x - ro * s, y - ro * s, x + ro * s, y + ro * s], a0, a1, fill=v,
                   width=max(1, int(round(w * s))))

    def rect(self, x0, y0, x1, y1, v=255):
        a = self.P((x0, y0))
        b = self.P((x1, y1))
        self.d.rectangle([a[0], a[1], b[0] - 1, b[1] - 1], fill=v)

    def arr(self, blur=0.0):
        im = self.img
        if blur > 0:
            im = im.filter(ImageFilter.GaussianBlur(blur * self.L.s))
        return np.asarray(im, np.float32) / 255.0


def paint(layer, col, m, alpha=1.0, blur=0.0):
    layer.over(col, m.arr(blur) * alpha)


def glow(layer, c, r0, r1, col, peak, power=1.6):
    """A radial falloff from r0 (full) to r1 (none)."""
    s = layer.s
    i0 = max(0, int((c[0] - r1 - layer.x0) * s))
    i1 = min(layer.W, int((c[0] + r1 - layer.x0) * s) + 1)
    j0 = max(0, int((c[1] - r1 - layer.y0) * s))
    j1 = min(layer.H, int((c[1] + r1 - layer.y0) * s) + 1)
    if i1 <= i0 or j1 <= j0:
        return
    d = np.hypot(layer.X[j0:j1, i0:i1] - c[0], layer.Y[j0:j1, i0:i1] - c[1])
    f = np.clip(1.0 - (d - r0) / max(1e-6, r1 - r0), 0, 1) ** power
    layer.over(col, f * peak, box=(i0, j0))


def beam(layer, p0, p1, hw, col, peak):
    """A soft diagonal light beam, fading out across it and at both ends."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L
    rx, ry = layer.X - p0[0], layer.Y - p0[1]
    along = (rx * ux + ry * uy) / L
    across = np.abs(rx * (-uy) + ry * ux)
    a = peak * np.clip(1.0 - across / hw, 0, 1)
    ramp = np.clip(np.minimum(along / 0.25, (1.0 - along) / 0.25), 0, 1)
    layer.over(col, a * ramp)


# --------------------------------------------------------------------------
# Shapes
# --------------------------------------------------------------------------

def star_points(c, r, inner, rot, jitter=0.0, seed=0):
    g = rng(seed)
    pts = []
    for k in range(10):
        rr = r if k % 2 == 0 else r * inner
        if jitter:
            rr *= 1.0 + g.uniform(-jitter, jitter)
        a = math.radians(rot - 90 + 36 * k + (g.uniform(-4, 4) if jitter else 0))
        pts.append((c[0] + rr * math.cos(a), c[1] + rr * math.sin(a)))
    return pts


def twinkle_points(c, r, rot=0.0, pinch=0.16):
    """A concave four-point twinkle: four tips joined by inward-curving sides."""
    tips = []
    for k in range(4):
        a = math.radians(rot - 90 + 90 * k)
        tips.append((c[0] + r * math.cos(a), c[1] + r * math.sin(a)))
    pts = []
    for k in range(4):
        p0, p2 = tips[k], tips[(k + 1) % 4]
        mx, my = (p0[0] + p2[0]) / 2, (p0[1] + p2[1]) / 2
        ctrl = (c[0] + (mx - c[0]) * pinch, c[1] + (my - c[1]) * pinch)
        for i in range(10):
            t = i / 10.0
            pts.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * ctrl[0] + t * t * p2[0],
                        (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * ctrl[1] + t * t * p2[1]))
    return pts


def scribble_star(c, r, rot, seed):
    """A pentagram drawn in one stroke by hand: uneven points, slightly bowed
    sides, the stroke starting a little early and running past its start."""
    g = rng(seed)
    v = []
    for k in range(5):
        a = math.radians(rot - 90 + 72 * k + g.uniform(-5, 5))
        rr = r * (1 + g.uniform(-0.09, 0.07))
        v.append((c[0] + rr * math.cos(a), c[1] + rr * math.sin(a)))
    order = [0, 2, 4, 1, 3, 0]
    pts = [(v[0][0] - (v[2][0] - v[0][0]) * 0.06, v[0][1] - (v[2][1] - v[0][1]) * 0.06)]
    for i in range(5):
        a_, b_ = v[order[i]], v[order[i + 1]]
        mx, my = (a_[0] + b_[0]) / 2, (a_[1] + b_[1]) / 2
        dx, dy = b_[0] - a_[0], b_[1] - a_[1]
        L = math.hypot(dx, dy)
        bow = g.uniform(-0.035, 0.035) * L
        ctrl = (mx - dy / L * bow, my + dx / L * bow)
        for j in range(1, 9):
            t = j / 8.0
            pts.append(((1 - t) ** 2 * a_[0] + 2 * (1 - t) * t * ctrl[0] + t * t * b_[0],
                        (1 - t) ** 2 * a_[1] + 2 * (1 - t) * t * ctrl[1] + t * t * b_[1]))
    a_, b_ = v[3], v[0]
    pts.append((b_[0] + (b_[0] - a_[0]) * 0.16, b_[1] + (b_[1] - a_[1]) * 0.16))
    return pts


def wobble_circle(c, r, seed, amp=0.0045):
    """A circle inked by hand: its radius wanders a little, never the same twice."""
    g = rng(seed)
    k1, k2 = g.integers(2, 5), g.integers(5, 9)
    f1, f2 = g.uniform(0, 6.28), g.uniform(0, 6.28)
    n = max(24, int(r * 1.2))
    pts = []
    for i in range(n + 1):
        a = math.radians(360.0 * i / n)
        rr = r * (1 + amp * math.sin(k1 * a + f1) + amp * 0.6 * math.sin(k2 * a + f2))
        pts.append((c[0] + rr * math.cos(a), c[1] + rr * math.sin(a)))
    return pts


# --------------------------------------------------------------------------
# The layers
# --------------------------------------------------------------------------

def make_sky():
    """Night navy at the top, the city's ultramarine glow at the horizon,
    with the level's nebula, one soft beam and a faint astrolabe drawn in the
    sky in dim blue-violet (never faded yellow, which goes grey on indigo)."""
    L = Layer(0, 0, W, H, ss=2)
    stops = [(0.0, "#050236"), (0.42, "#0A055E"), (0.74, "#160A8C"), (0.86, "#1E0EA2"), (1.0, "#14097E")]
    t = np.clip(L.Y / H, 0, 1)
    for (lo, c0), (hi, c1) in zip(stops, stops[1:]):
        sel = (t >= lo) & (t <= hi)
        f = ((t - lo) / (hi - lo))[sel][:, None]
        L.rgb[sel] = hexc(c0) * (1 - f) + hexc(c1) * f
    L.a[:] = 1.0
    g = rng(21)
    md, ml = Mask(L), Mask(L)
    for i in range(16):
        x, y = g.uniform(-40, W + 40), g.uniform(-40, H * 0.8)
        r = g.uniform(30, 80)
        (md if i % 2 else ml).circle((x, y), r)
    paint(L, "#03011C", md, 0.30, blur=26)
    paint(L, "#3A2FC0", ml, 0.14, blur=30)
    beam(L, (300, -30), (520, 530), 60, "#8C82F0", 0.07)
    m = Mask(L)
    ac = (250, 640)
    for rr in (300, 410, 520, 630):
        m.arc(ac, rr, 180, 360, 1.15)
    for a in np.arange(186, 360, 14.0):
        aa = math.radians(a)
        m.line([(ac[0] + 300 * math.cos(aa), ac[1] + 300 * math.sin(aa)),
                (ac[0] + 630 * math.cos(aa), ac[1] + 630 * math.sin(aa))], 1.0, caps=False)
    paint(L, "#3A30A8", m, 0.45)
    return L.image(opaque=True)


# The dial's centre, from its corner: 80 left of the window's right edge and
# 12 down from its top (in the first-size menu, (640, 12)).
DIAL_RIGHT, DIAL_TOP = 80.0, 12.0
DIAL_STILL = (-142.0, -12.0, 222.0, 154.0)  # x, y, w, h round the centre: all of it the window can show
DIAL_CW_R = 121.0
DIAL_CCW_R = 153.0


def make_dial_still():
    """The crescent (a disc with a darker one bitten out of it, the bite
    outlined) and the rings that hold still."""
    L = Layer(*DIAL_STILL)
    c = (0.0, 0.0)
    bite = (-16.0, -14.0)
    m = Mask(L)
    m.circle(c, 50)
    m.circle(bite, 42, v=0)
    paint(L, YEL, m)
    m = Mask(L)
    m.line(wobble_circle(bite, 42, 3, amp=0.006), 1.1, closed=True)
    for rr, w, sd in [(62, 1.5, 4), (65.6, 1.2, 5), (98, 1.6, 6), (119, 1.6, 7),
                      (129, 1.2, 8), (134, 1.7, 9), (134.9, 0.8, 19), (139, 1.2, 10)]:
        m.line(wobble_circle(c, rr, sd), w, closed=True)
    paint(L, YEL, m)
    return L.image()


def make_dial_cw():
    """Turning clockwise: two arcs of dots, and the bezel's dividers with dots
    or a dash in its segments."""
    L = Layer(-DIAL_CW_R, -DIAL_CW_R, 2 * DIAL_CW_R, 2 * DIAL_CW_R)
    c = (0.0, 0.0)
    m = Mask(L)
    for a in list(np.arange(96, 176, 8.0)) + list(np.arange(196, 236, 8.0)):
        aa = math.radians(a)
        m.circle((77 * math.cos(aa), 77 * math.sin(aa)), 2.4)
    for k, a in enumerate(np.arange(0, 360, 20.0)):
        aa = math.radians(a)
        m.line([(98 * math.cos(aa), 98 * math.sin(aa)), (119 * math.cos(aa), 119 * math.sin(aa))], 1.4, caps=False)
        if k % 3 == 0:
            for j in range(3):
                ab = math.radians(a + 5 + j * 5)
                m.circle((108.5 * math.cos(ab), 108.5 * math.sin(ab)), 1.7)
        elif k % 3 == 1:
            m.arc(c, 108.5, a + 5, a + 15, 1.3)
    paint(L, YEL, m)
    return L.image()


def make_dial_ccw():
    """Turning the other way: a dashed ring inside, and the outermost dashed
    ring with spokes back to the still rings."""
    L = Layer(-DIAL_CCW_R, -DIAL_CCW_R, 2 * DIAL_CCW_R, 2 * DIAL_CCW_R)
    c = (0.0, 0.0)
    m = Mask(L)
    for a in np.arange(0, 360, 9.0):
        m.arc(c, 89, a, a + 5.0, 1.4)
    for a in np.arange(0, 360, 15.0):
        m.arc(c, 151, a, a + 10.5, 1.4)
    for a in np.arange(7.5, 360, 30.0):
        aa = math.radians(a)
        m.line([(139 * math.cos(aa), 139 * math.sin(aa)), (151 * math.cos(aa), 151 * math.sin(aa))], 1.3, caps=False)
    paint(L, YEL, m)
    return L.image()


# The skyline, in the first-size menu: (x0, x1, top, spire apex or None).
LEFT_BUILDINGS = [(-2, 60, 434, None), (52, 98, 404, (75, 380)), (90, 128, 420, None),
                  (120, 172, 350, (146, 326))]
MIDDLE_BUILDINGS = [(166, 260, 452, None), (250, 330, 424, None), (322, 420, 446, (371, 426)),
                    (412, 520, 432, None), (512, 600, 456, None), (592, 690, 428, None)]
RIGHT_BUILDINGS = [(684, 722, 382, (703, 362))]
# Window columns: (x, the top of the building they're in). A slot every 12
# units down from 9 below it; the seeded draw lights about one in eight.
WINDOW_COLUMNS = [(128, 350), (136, 350), (144, 350), (152, 350), (160, 350),
                  (100, 422), (108, 422), (116, 422), (70, 408), (78, 408), (86, 408),
                  (692, 384), (700, 384), (708, 384), (716, 384)]
SLOT_W, SLOT_H = 3.2, 6.0

LEFT_RECT = (0.0, 324.0, 176.0, 176.0)    # from the window's left edge, down to its bottom
MIDDLE_X, MIDDLE_PERIOD = 172.0, 512.0     # repeats from x 172 to the right piece
MIDDLE_RECT = (MIDDLE_X, 422.0, MIDDLE_PERIOD, 78.0)
RIGHT_RECT = (680.0, 358.0, 40.0, 142.0)  # to the window's right edge


def windows():
    g = rng(5)
    slots, lit = [], []
    for (x, top) in WINDOW_COLUMNS:
        y = top + 9
        while y < H - 12:
            slots.append((x, y))
            if g.uniform() < 0.13:
                lit.append((x, y))
            y += 12
    return slots, lit


def building(m, b):
    x0, x1, top, apex = b
    m.rect(x0, top, x1, H + 2)
    if apex:
        m.poly([(x0 + (x1 - x0) * 0.22, top + 1), apex, (x1 - (x1 - x0) * 0.22, top + 1)])


def make_city_piece(rect, buildings, slots):
    L = Layer(*rect)
    m = Mask(L)
    for b in buildings:
        building(m, b)
    paint(L, NAVY, m)
    m = Mask(L)
    for (x, y) in slots:
        m.rect(x, y, x + SLOT_W, y + SLOT_H)
    paint(L, SLOT, m, 0.9)
    return L.image()


# Stars, in the first-size menu, each with where it holds when the window is
# another size: x from the Left edge, the Right edge, or Proportionally across
# the page (right of the sidebar); y from the Top or the Bottom.
SCRIBBLES = [  # centre, radius, turn, seed, anchor
    ((132, 150), 25, 8, 41, "LT"), ((500, 128), 18, -14, 42, "PT"), ((116, 270), 13, 20, 43, "LT"),
    ((707, 252), 13, 4, 44, "RT"), ((372, 26), 15, -6, 45, "PT"), ((332, 128), 13, 10, 46, "PT"),
]
FILLED = [
    ((154, 237), 7.5, 10, 51, "LT"), ((468, 22), 5.5, -8, 52, "PT"), ((560, 140), 4.5, 16, 53, "PT"),
    ((706, 470), 6.0, 20, 55, "RB"), ((690, 200), 3.6, -12, 56, "RT"),
]
TWINKLES = [
    ((152, 66), 5.2, "LT"), ((442, 90), 4.4, "PT"), ((706, 330), 5.8, "RT"), ((588, 118), 3.6, "PT"),
    ((112, 214), 3.2, "LT"), ((706, 140), 3.4, "RT"), ((440, 339.5), 5.0, "PT"),
]
HALO_R0, HALO_R1 = 0.2, 1.55  # a scribble's halo, in its radii
HALO_SPRITE_R = 32.0
STROKE = 1.55


def sprite_layer(cx, cy, half_w, half_h):
    """A layer centred on (cx, cy) whose sides fall on whole pixels."""
    hw = math.ceil(half_w * OUT) / OUT
    hh = math.ceil(half_h * OUT) / OUT
    return Layer(cx - hw, cy - hh, 2 * hw, 2 * hh)


def extent(pts, c):
    return (max(abs(p[0] - c[0]) for p in pts), max(abs(p[1] - c[1]) for p in pts))


def make_sprites():
    """Each sprite with its centre (where it's placed from): [(name, image,
    (left, top) of the image from the centre in units)]."""
    out = []
    for i, (c, r, rot, sd, anchor) in enumerate(SCRIBBLES):
        pts = scribble_star(c, r, rot, sd)
        ex, ey = extent(pts, c)
        L = sprite_layer(c[0], c[1], ex + STROKE + 1, ey + STROKE + 1)
        m = Mask(L)
        m.line(pts, STROKE)
        paint(L, YEL, m)
        out.append(("scribble%d" % i, L.image(), (L.x0 - c[0], L.y0 - c[1])))
    for i, (c, r, rot, sd, anchor) in enumerate(FILLED):
        pts = star_points(c, r, 0.46, rot, jitter=0.08, seed=sd)
        ex, ey = extent(pts, c)
        L = sprite_layer(c[0], c[1], ex + 1.5, ey + 1.5)
        m = Mask(L)
        m.poly(pts)
        paint(L, YEL, m)
        out.append(("filled%d" % i, L.image(), (L.x0 - c[0], L.y0 - c[1])))
    for i, (c, r, anchor) in enumerate(TWINKLES):
        pts = twinkle_points(c, r, rot=i * 7)
        ex, ey = extent(pts, c)
        L = sprite_layer(c[0], c[1], ex + 1.5, ey + 1.5)
        m = Mask(L)
        m.poly(pts)
        paint(L, YEL, m)
        out.append(("twinkle%d" % i, L.image(), (L.x0 - c[0], L.y0 - c[1])))
    # The halo: white, its colour and strength given when it's drawn.
    R = HALO_SPRITE_R
    L = sprite_layer(0.0, 0.0, R + 1, R + 1)
    glow(L, (0.0, 0.0), R * HALO_R0 / HALO_R1, R, "#FFFFFF", 1.0, power=1.4)
    out.append(("halo", L.image(), (L.x0, L.y0)))
    # A lit window: pale yellow, with its light on the wall round it.
    L = sprite_layer(0.0, 0.0, 10.0, 10.0)
    glow(L, (0.0, 0.0), 1.0, 9.0, LIT, 0.30, power=1.8)
    m = Mask(L)
    m.rect(-SLOT_W / 2, -SLOT_H / 2, SLOT_W / 2, SLOT_H / 2)
    paint(L, LIT, m)
    out.append(("window", L.image(), (L.x0, L.y0)))
    return out


def pack(images, width, pad=4):
    """Shelf-packs images into one atlas: [(x, y)] in pixels, and its height."""
    order = sorted(range(len(images)), key=lambda i: -images[i].height)
    at = [None] * len(images)
    x = y = shelf = 0
    for i in order:
        im = images[i]
        if x + im.width > width:
            x, y, shelf = 0, y + shelf + pad, 0
        at[i] = (x, y)
        x += im.width + pad
        shelf = max(shelf, im.height)
    return at, y + shelf


def particles():
    """Square specks in the level's blue and teal: (x, y, size) in the first-
    size menu, rising with Animations on."""
    g = rng(9)
    out = []
    for i in range(30):
        x, y = g.uniform(90, W - 4), g.uniform(4, 380)
        sz = g.uniform(1.4, 3.0)
        out.append((x, y, sz))
    return out


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def save(im, name):
    path = os.path.join(RES, name)
    im.save(path, optimize=True)
    return im.size, os.path.getsize(path)


def f(v):
    """A C++ float literal."""
    s = ("%.3f" % v).rstrip("0").rstrip(".")
    if s in ("-0", ""):
        s = "0"
    return s + ("f" if "." in s else ".f")


def main():
    os.makedirs(RES, exist_ok=True)
    sizes = {}
    sizes["sky"] = save(make_sky(), "jupiter_sky.png")
    sizes["dial"] = save(make_dial_still(), "jupiter_dial.png")
    sizes["dial_cw"] = save(make_dial_cw(), "jupiter_dial_cw.png")
    sizes["dial_ccw"] = save(make_dial_ccw(), "jupiter_dial_ccw.png")

    slots, lit = windows()
    left = make_city_piece(LEFT_RECT, LEFT_BUILDINGS, [s for s in slots if s[0] < 340])
    right = make_city_piece(RIGHT_RECT, RIGHT_BUILDINGS, [s for s in slots if s[0] >= 340])
    middle = make_city_piece(MIDDLE_RECT, MIDDLE_BUILDINGS, [])
    city_at, city_h = pack([left, right, middle], 1024)
    city = Image.new("RGBA", (1024, city_h), (0, 0, 0, 0))
    for im, (x, y) in zip([left, right, middle], city_at):
        city.paste(im, (x, y))
    sizes["city"] = save(city, "jupiter_city.png")

    sprites = make_sprites()
    sprite_at, sprite_h = pack([s[1] for s in sprites], 512)
    atlas = Image.new("RGBA", (512, sprite_h), (0, 0, 0, 0))
    for (name, im, off), (x, y) in zip(sprites, sprite_at):
        atlas.paste(im, (x, y))
    sizes["sprites"] = save(atlas, "jupiter_sprites.png")

    write_header(sizes, (left, right, middle), city_at, lit, sprites, sprite_at)
    total = 0
    for k, ((w_, h_), b) in sizes.items():
        print("%-9s %4d x %4d  %7d bytes" % (k, w_, h_, b))
        total += b
    print("total %d bytes (%.1f KB)" % (total, total / 1024.0))


def write_header(sizes, city_pieces, city_at, lit, sprites, sprite_at):
    L = []
    w = L.append
    w("// Generated by tools/jupiter_art.py: don't edit it by hand, run that again.")
    w("//")
    w("// Where the JupiterBot / JMF Trainer art's layers (resources/jupiter/*.png)")
    w("// go, in menu units at the menu's first size (720 x 500). The files are")
    w("// kPx pixels a unit; rects in them are pixels.")
    w("")
    w("#pragma once")
    w("")
    w("namespace gucci::ui::pages::jupiter_art {")
    w("")
    w("    inline constexpr float kPx = %s;" % f(OUT))
    w("    inline constexpr float kDesignW = %s, kDesignH = %s;" % (f(W), f(H)))
    w("    inline constexpr float kPageLeft = %s;  // the page's left edge at that size" % f(PAGE_LEFT))
    w("")
    w("    struct File {")
    w("        const char* name;")
    w("        int w, h;")
    w("    };")
    names = [("kSky", "jupiter_sky.png", "sky"), ("kDial", "jupiter_dial.png", "dial"),
             ("kDialCw", "jupiter_dial_cw.png", "dial_cw"), ("kDialCcw", "jupiter_dial_ccw.png", "dial_ccw"),
             ("kCity", "jupiter_city.png", "city"), ("kSprites", "jupiter_sprites.png", "sprites")]
    for var, name, key in names:
        (pw, ph), _ = sizes[key]
        w("    inline constexpr File %s{\"%s\", %d, %d};" % (var, name, pw, ph))
    w("")
    w("    // The dial: its centre kDialRight left of the window's right edge and")
    w("    // kDialTop below its top. Its still part covers kDialStill (x, y, w, h)")
    w("    // round the centre; the turning rings are squares reaching kDialCwR and")
    w("    // kDialCcwR from it.")
    w("    inline constexpr float kDialRight = %s, kDialTop = %s;" % (f(DIAL_RIGHT), f(DIAL_TOP)))
    w("    inline constexpr float kDialStill[4] = {%s};" % ", ".join(f(v) for v in DIAL_STILL))
    w("    inline constexpr float kDialCwR = %s, kDialCcwR = %s;" % (f(DIAL_CW_R), f(DIAL_CCW_R)))
    w("")
    w("    // A part of an atlas: its rect in the file (pixels), and the rect it")
    w("    // covers in the menu (units).")
    w("    struct Part {")
    w("        float u0, v0, u1, v1;")
    w("        float x, y, w, h;")
    w("    };")
    labels = ["kCityLeft", "kCityRight", "kCityMiddle"]
    rects = [LEFT_RECT, RIGHT_RECT, MIDDLE_RECT]
    comments = ["from the window's left edge, down to its bottom (y from the bottom)",
                "to the window's right edge (x from the right edge, y from the bottom)",
                "repeating every kCityPeriod from kCityMiddle.x to the right piece (y from the bottom)"]
    for lab, im, (x, y), rect, com in zip(labels, city_pieces, city_at, rects, comments):
        rx, ry, rw, rh = rect
        if lab == "kCityRight":
            rx = rx - W
        ry = ry - H
        w("    // The skyline, %s." % com)
        w("    inline constexpr Part %s{%d, %d, %d, %d, %s, %s, %s, %s};" % (
            lab, x, y, x + im.width, y + im.height, f(rx), f(ry), f(rw), f(rh)))
    w("    inline constexpr float kCityPeriod = %s;" % f(MIDDLE_PERIOD))
    w("    // The strip stops this far from the right edge, where the tower starts.")
    w("    inline constexpr float kCityMiddleEnd = %s;" % f(RIGHT_BUILDINGS[0][0] - W))
    w("")
    w("    // The lit windows' centres: x from the left edge (x >= 0) or, negative,")
    w("    // from the right edge; y from the bottom. In the order the flicker counts.")
    w("    struct Spot {")
    w("        float x, y;")
    w("    };")
    w("    inline constexpr Spot kLitWindows[] = {")
    for (x, y) in lit:
        cx, cy = x + SLOT_W / 2, y + SLOT_H / 2
        if x >= 340:
            cx -= W
        w("        {%s, %s}," % (f(cx), f(cy - H)))
    w("    };")
    w("")
    w("    // A sprite: its rect in kSprites (pixels), and where its top-left sits")
    w("    // from its centre (units).")
    w("    struct Sprite {")
    w("        float u0, v0, u1, v1;")
    w("        float left, top;")
    w("    };")
    index = {}
    w("    inline constexpr Sprite kSpriteRects[] = {")
    for i, ((name, im, (ox, oy)), (x, y)) in enumerate(zip(sprites, sprite_at)):
        index[name] = i
        w("        {%d, %d, %d, %d, %s, %s},  // %d %s" % (x, y, x + im.width, y + im.height, f(ox), f(oy), i, name))
    w("    };")
    w("    inline constexpr int kHaloSprite = %d, kWindowSprite = %d;" % (index["halo"], index["window"]))
    w("    inline constexpr float kHaloR = %s;     // the halo sprite's outer radius (units)" % f(HALO_SPRITE_R))
    w("    inline constexpr float kHaloReach = %s;  // a halo's outer radius, in its star's radii" % f(HALO_R1))
    w("")
    w("    // A star placed in the menu: the sprite, its centre at the first size,")
    w("    // and how it holds when the window is another size: x from the Left or")
    w("    // Right edge or Proportionally across the page right of the sidebar,")
    w("    // y from the Top or Bottom. `r` is its radius (a halo's size).")
    w("    enum class Kind { Scribble, Filled, Twinkle };")
    w("    struct Star {")
    w("        Kind kind;")
    w("        int sprite;")
    w("        float x, y, r;")
    w("        char ax, ay;")
    w("    };")
    w("    inline constexpr Star kStars[] = {")
    for i, (c, r, rot, sd, anchor) in enumerate(SCRIBBLES):
        w("        {Kind::Scribble, %d, %s, %s, %s, '%s', '%s'}," % (
            index["scribble%d" % i], f(c[0]), f(c[1]), f(r), anchor[0], anchor[1]))
    for i, (c, r, rot, sd, anchor) in enumerate(FILLED):
        w("        {Kind::Filled, %d, %s, %s, %s, '%s', '%s'}," % (
            index["filled%d" % i], f(c[0]), f(c[1]), f(r), anchor[0], anchor[1]))
    for i, (c, r, anchor) in enumerate(TWINKLES):
        w("        {Kind::Twinkle, %d, %s, %s, %s, '%s', '%s'}," % (
            index["twinkle%d" % i], f(c[0]), f(c[1]), f(r), anchor[0], anchor[1]))
    w("    };")
    w("")
    w("    // Square specks (x, y, size) at the first size, odd ones blue, even ones")
    w("    // teal; they rise and come round again below y kSpeckSpan.")
    w("    struct Speck {")
    w("        float x, y, size;")
    w("    };")
    w("    inline constexpr Speck kSpecks[] = {")
    for (x, y, sz) in particles():
        w("        {%s, %s, %s}," % (f(x), f(y), f(sz)))
    w("    };")
    w("    inline constexpr float kSpeckSpan = %s;" % f(380))
    w("")
    w("} // namespace gucci::ui::pages::jupiter_art")
    with open(HEADER, "w", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
