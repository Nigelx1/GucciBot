#!/usr/bin/env python3
"""SolarBot's art: "the Orrery on the dark sky", the v2 mockup Nigel picked
(2026-11-02), drawn as image layers that the menu puts together at run time
(src/ui/pages/solar_backdrop.cpp).

Original art in the language of Solar Flare (Linear and Rynoxious): Janet, the
level's smiling sun, looking in from the top-right corner at the middle of an
orrery (thin orbit rings, dotted rings turning, small planets riding them), gold
constellations with four-point sparkles, a cyan-cored sunburst medallion, a
jeweled kite, warm bokeh and lens-flare twinkles, and the end card's scalloped
corona bands along the top and bottom, all on the level's warm near-black sky
(#1A0800). Nothing is traced or copied from the level, its videos, the
verification thumbnail or RobTop's assets; the shapes are the mockup's
(scratchpad draw_mock_v2.py), with Janet's eyes bowed toward the menu as Nigel
asked.

Every layer is drawn in menu units (the menu's first size is 720 x 500),
supersampled, with straight alpha as ImGui blends it:

  solar_sky.png      the sky (warmer toward Janet), its stars and two far spiky
                     suns: opaque, 2 pixels a unit, cover-fitted to the window
  solar_glow.png     Janet's light round her rim, half a pixel a unit (it is all
                     gradient); it breathes
  solar_disc.png     her gold band, the dark-gold line inside it, her body and
                     its mottling
  solar_flames.png   her flame streaks: a tile 2R tall that wraps top to bottom,
                     rising inside her disc
  solar_top.png      her lemon limb and her face, over the flames
  solar_sprites.png  the planets, both constellations and their sparkle, the
                     medallion and its two turning rings, the kite and its glint,
                     a bokeh light, a twinkle, and one period of each corona band

Janet's four only hold the part of her the window can show: she sits in its
top-right corner, her centre 34 units in from the right edge and 6 down.

It also writes src/ui/pages/solar_art.hpp, the tables that place each part,
and the rings the menu draws itself round Janet (six orbits and three dotted
rings: textures of those would be up to 3156 pixels square) with the quiet
zones where they fade behind the header's and the sidebar's words.

Run from anywhere:  python -I tools/solar_art.py      (numpy and Pillow)
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources", "solar")
HEADER = os.path.join(ROOT, "src", "ui", "pages", "solar_art.hpp")

OUT = 2          # file pixels a unit (the glow: GLOW_OUT)
GLOW_OUT = 0.5
W, H = 720, 500  # the menu's first size (ui/shell.cpp): the layout's design space
PAGE_LEFT = 172  # the page's left edge there: the window's padding, the sidebar and a gap


def hexc(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], np.float32)


def mix(a, b, t):
    return a * (1.0 - t) + b * t


def rng(seed):
    return np.random.default_rng(seed)


# --------------------------------------------------------------------------
# Drawing: a canvas in menu units (premultiplied while drawing) and coverage
# masks drawn on it
# --------------------------------------------------------------------------

class Canvas:
    """RGBA art over w x h units from `origin`, `s` samples a unit."""

    def __init__(self, w, h, s, origin=(0.0, 0.0)):
        self.w, self.h, self.s = int(w), int(h), int(s)
        assert abs(self.w - w) < 1e-9 and abs(self.h - h) < 1e-9, (w, h)
        self.ox, self.oy = float(origin[0]), float(origin[1])
        self.px = np.zeros((self.h * s, self.w * s, 3), np.float32)
        self.A = np.zeros((self.h * s, self.w * s), np.float32)
        ys = (np.arange(self.h * s, dtype=np.float32) + 0.5) / s + self.oy
        xs = (np.arange(self.w * s, dtype=np.float32) + 0.5) / s + self.ox
        self.X, self.Y = np.meshgrid(xs, ys)

    def over(self, col, a):
        a = np.clip(np.asarray(a, np.float32), 0.0, 1.0)
        if a.ndim == 0:
            a = np.full(self.A.shape, float(a), np.float32)
        self.px *= (1.0 - a)[..., None]
        self.px += np.asarray(col, np.float32) * a[..., None]
        self.A *= 1.0 - a
        self.A += a

    def over_img(self, img, a):
        a = np.clip(a, 0.0, 1.0)
        self.px *= (1.0 - a)[..., None]
        self.px += img * a[..., None]
        self.A *= 1.0 - a
        self.A += a

    def over_local(self, col, a, x0, y0):
        h, w = a.shape
        a = np.clip(a, 0.0, 1.0)
        sub = self.px[y0:y0 + h, x0:x0 + w]
        sub *= (1.0 - a)[..., None]
        sub += np.asarray(col, np.float32) * a[..., None]
        sa = self.A[y0:y0 + h, x0:x0 + w]
        sa *= 1.0 - a
        sa += a

    def image(self, out=OUT, opaque=False):
        """The canvas at `out` pixels a unit (a box filter down from its
        samples): straight alpha, its clear edges given the art's colour."""
        b = int(round(self.s / out))
        assert abs(b * out - self.s) < 1e-9, (self.s, out)
        ph, pw = self.h * self.s // b, self.w * self.s // b
        assert ph * b == self.h * self.s and pw * b == self.w * self.s, (self.w, self.h, out)
        p = self.px.reshape(ph, b, pw, b, 3).mean(axis=(1, 3))
        A = self.A.reshape(ph, b, pw, b).mean(axis=(1, 3))
        if opaque:
            return Image.fromarray((np.clip(p, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
        c = np.where(A[..., None] > 1e-6, p / np.maximum(A[..., None], 1e-6), 0.0)
        c, A = bleed(np.clip(c, 0, 1), np.clip(A, 0, 1))
        o = np.concatenate([c, A[..., None]], -1)
        return Image.fromarray((o * 255 + 0.5).astype(np.uint8), "RGBA")


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
    """A coverage mask at the canvas's samples, drawn in menu units."""

    def __init__(self, cv):
        self.cv, self.s = cv, cv.s
        self.ox, self.oy = cv.ox, cv.oy
        self.img = Image.new("L", (cv.w * cv.s, cv.h * cv.s), 0)
        self.d = ImageDraw.Draw(self.img)

    def P(self, p):
        return ((p[0] - self.ox) * self.s, (p[1] - self.oy) * self.s)

    def _box(self, x0, y0, x1, y1):
        a, b = self.P((x0, y0)), self.P((x1, y1))
        return [a[0], a[1], b[0], b[1]]

    def circle(self, c, r, v=255):
        self.d.ellipse(self._box(c[0] - r, c[1] - r, c[0] + r, c[1] + r), fill=v)

    def ring(self, c, r, w, v=255):
        ro = r + w / 2.0
        self.d.ellipse(self._box(c[0] - ro, c[1] - ro, c[0] + ro, c[1] + ro),
                       outline=v, width=max(1, int(round(w * self.s))))

    def poly(self, pts, v=255):
        self.d.polygon([self.P(p) for p in pts], fill=v)

    def line(self, pts, w, v=255, closed=False, caps=True):
        P = [self.P(p) for p in pts]
        if closed:
            P = P + [P[0], P[1]]
        self.d.line(P, fill=v, width=max(1, int(round(w * self.s))), joint="curve")
        if caps and not closed:
            for p in (pts[0], pts[-1]):
                self.circle(p, w / 2.0, v)

    def stroke(self, pts, w, v=255):
        """A thick round-ended stroke with no seams: a quad per segment and a
        disc at every point (Pillow's wide polylines leave hairline gaps)."""
        r = w / 2.0
        for p0, p1 in zip(pts[:-1], pts[1:]):
            dx, dy = p1[0] - p0[0], p1[1] - p0[1]
            L = math.hypot(dx, dy)
            if L < 1e-9:
                continue
            nx, ny = -dy / L * r, dx / L * r
            self.poly([(p0[0] + nx, p0[1] + ny), (p1[0] + nx, p1[1] + ny), (p1[0] - nx, p1[1] - ny), (p0[0] - nx, p0[1] - ny)], v)
        for p in pts:
            self.circle(p, r, v)

    def arc(self, c, r, a0, a1, w, v=255):
        """Degrees, 0 = +x, increasing clockwise on screen (y down)."""
        ro = r + w / 2.0
        self.d.arc(self._box(c[0] - ro, c[1] - ro, c[0] + ro, c[1] + ro),
                   a0, a1, fill=v, width=max(1, int(round(w * self.s))))

    def arr(self, blur=0.0):
        im = self.img
        if blur > 0:
            im = im.filter(ImageFilter.GaussianBlur(blur * self.s))
        return np.asarray(im, np.float32) / 255.0


def paint(cv, col, m, alpha=1.0, blur=0.0):
    cv.over(col, m.arr(blur) * alpha)


def ramp_stops(t, stops):
    """Piecewise-linear colour ramp: t (array) through [(t, hex), ...]."""
    out = np.zeros(t.shape + (3,), np.float32)
    ts = [s[0] for s in stops]
    cols = [hexc(s[1]) for s in stops]
    out[:] = cols[0]
    for i in range(len(stops) - 1):
        lo, hi = ts[i], ts[i + 1]
        sel = (t >= lo) & (t <= hi)
        f = ((t - lo) / max(1e-6, hi - lo))[sel][:, None]
        out[sel] = cols[i] * (1 - f) + cols[i + 1] * f
    out[t > ts[-1]] = cols[-1]
    return out


def glow(cv, c, r0, r1, col, peak, power=1.6):
    """Radial falloff from r0 (full) to r1 (none)."""
    s = cv.s
    x0 = max(0, int((c[0] - r1 - cv.ox) * s)); x1 = min(cv.w * s, int((c[0] + r1 - cv.ox) * s) + 1)
    y0 = max(0, int((c[1] - r1 - cv.oy) * s)); y1 = min(cv.h * s, int((c[1] + r1 - cv.oy) * s) + 1)
    if x1 <= x0 or y1 <= y0:
        return
    X = cv.X[y0:y1, x0:x1]; Y = cv.Y[y0:y1, x0:x1]
    d = np.hypot(X - c[0], Y - c[1])
    f = np.clip(1.0 - (d - r0) / max(1e-6, r1 - r0), 0, 1) ** power
    cv.over_local(col, f * peak, x0, y0)


# --------------------------------------------------------------------------
# The level's colours
# --------------------------------------------------------------------------

GOLD = hexc("#F6CF10")        # the level's line art
CYAN = hexc("#30FFFC")        # its gems, at full brightness
NAVY = hexc("#0B1E3A")        # their settings
FACE_INK = hexc("#492100")    # Janet's face

# --------------------------------------------------------------------------
# Janet
# --------------------------------------------------------------------------

JANET = (686.0, -6.0)         # her centre at the first size: 34 in from the right edge, 6 above the top
JR = 134.0
JANET_RIGHT, JANET_DOWN = W - JANET[0], JANET[1]
FACE_AT = (-0.42, 0.24)       # her face's eye midpoint, from her centre, in R: her lower left
FACE_TILT = 20.0              # degrees counter-clockwise, the thumbnail's
GLOW_BREATHE = (0.06, 0.8)    # her glow's breath: scale 1 + 0.06 sin(0.8 t)
FLAME_RISE = 0.035            # her flames rise this much of their 2R tile a second (0.07 R)

# Her parts' images reach this far round her centre (left, top, right, bottom,
# units): what the window can show of her, with a margin. Her centre is
# JANET_RIGHT in from the right edge and JANET_DOWN below the top (it's
# above), so to the right and above she is cut there; the tile of flames is 2R
# tall, as it wraps.
DISC_BOX = (-148.0, -12.0, 40.0, 148.0)
TOP_BOX = (-136.0, -12.0, 40.0, 136.0)
FLAME_BOX = (-136.0, -134.0, 40.0, 134.0)
GLOW_BOX = (-296.0, -12.0, 40.0, 296.0)


def janet_glow(cv, c, R):
    """Her light: a narrow gold glow off her rim and a wide faint orange bloom
    on the sky (the menu scales it to breathe)."""
    glow(cv, c, R * 1.06, R * 2.05, hexc("#FF9A1E"), 0.30, power=2.4)
    glow(cv, c, R * 1.06, R * 1.36, hexc("#FFC21A"), 0.55, power=1.7)


def janet_disc(cv, c, R, seed=7):
    """The gold band (more orange at the bottom), the dark-gold line inside
    it, her body (orange in the middle, lighter toward the rim) and its
    mottling."""
    m = Mask(cv)
    m.circle(c, R * 1.085)
    band = mix(hexc("#F4C81C")[None, None, :], hexc("#EC951E")[None, None, :],
               np.clip((cv.Y - (c[1] - R * 0.2)) / (R * 1.3), 0, 1)[..., None])
    cv.over_img(band, m.arr())
    m = Mask(cv)
    m.circle(c, R * 1.04)
    paint(cv, hexc("#A86C00"), m)
    body = Mask(cv)
    body.circle(c, R)
    bm = body.arr()
    d = np.hypot(cv.X - c[0], cv.Y - c[1]) / R
    base = ramp_stops(d, [(0.0, "#FF8306"), (0.5, "#FF8602"), (0.74, "#FF9405"), (0.88, "#FFA802"), (1.0, "#FFB400")])
    cv.over_img(base, bm)
    g = rng(seed + 100)
    deep, light = Mask(cv), Mask(cv)
    for i in range(14):
        a = g.uniform(0, 6.283)
        rr = g.uniform(0.0, 0.62) * R
        pr = g.uniform(0.16, 0.30) * R
        (deep if i % 2 else light).circle((c[0] + rr * math.cos(a), c[1] + rr * math.sin(a)), pr)
    cv.over(hexc("#F26A00"), deep.arr(blur=R * 0.10) * 0.40 * bm)
    cv.over(hexc("#FFA012"), light.arr(blur=R * 0.10) * 0.34 * bm)


FLAME_TONES = [("#F06000", 0.40), ("#FF7C00", 0.34), ("#FFA818", 0.40), ("#FFC63E", 0.30)]


def janet_flames(cv, c, R, seed=7):
    """Flame streaks: soft upright capsules in four tones, as a tile 2R tall
    that wraps top to bottom (each streak drawn again 2R above and below)."""
    g = rng(seed)
    masks = [Mask(cv) for _ in FLAME_TONES]
    n = int(40 + R * 0.75)
    for i in range(n):
        while True:
            x, y = g.uniform(-1, 1), g.uniform(-1, 1)
            if x * x + y * y < 0.88:
                break
        L = g.uniform(0.16, 0.46) * R
        wdt = g.uniform(0.035, 0.085) * R
        lean = math.radians(g.uniform(-7, 7))
        wob = g.uniform(-0.18, 0.18) * L
        k = g.choice(len(FLAME_TONES), p=[0.30, 0.26, 0.30, 0.14])
        for dy in (-2.0, 0.0, 2.0):
            px, py = c[0] + x * R, c[1] + (y + dy) * R
            ux, uy = math.sin(lean), -math.cos(lean)
            pts = [(px - ux * L / 2, py - uy * L / 2), (px + wob * 0.6, py), (px + ux * L / 2, py + uy * L / 2)]
            masks[k].line(pts, wdt)
    for (col, al), mk in zip(FLAME_TONES, masks):
        cv.over(hexc(col), mk.arr(blur=max(0.7, R * 0.016)) * al)


def janet_limb(cv, c, R):
    """The lemon limb just inside her edge."""
    body = Mask(cv)
    body.circle(c, R)
    bm = body.arr()
    d = np.hypot(cv.X - c[0], cv.Y - c[1]) / R
    cv.over(hexc("#FFE303"), np.clip((d - 0.80) / 0.15, 0, 1) ** 1.3 * 0.95 * bm)
    cv.over(hexc("#FFD602"), np.clip((d - 0.965) / 0.035, 0, 1) * bm * 0.6)


# Her face, measured off the verification thumbnail, in units of her radius
# along the face's own axes (x her right, y her down), origin the midpoint of
# her eyes. Eyes: short open strokes, leaning a little (bottoms to her right),
# each bowed like a bracket whose point faces where she looks. In the
# thumbnail her face is on her upper right and the points face up and right;
# here it is on her lower left, looking down and left into the menu, so the
# points face her left: down and left on the screen, toward the rest of the
# menu (Nigel, 2026-11-02). The bow is the thumbnail's depth. Smile: one
# rounded V, nothing across its ends.
FACE_EYES = (((-0.091, -0.086), (-0.071, 0.086)), ((0.070, -0.086), (0.092, 0.086)))
FACE_EYE_BOW = -0.014         # + bows to her right, - to her left
FACE_EYE_W = 0.076
FACE_SMILE = ((-0.078, 0.207), (-0.013, 0.364), (0.064, 0.213))   # quadratic: end, pull, end
FACE_SMILE_W = 0.074


def janet_face(cv, c, u, tilt_deg):
    th = math.radians(tilt_deg)
    ex = (math.cos(th), -math.sin(th))
    ey = (math.sin(th), math.cos(th))

    def P(x, y):
        return (c[0] + (ex[0] * x + ey[0] * y) * u, c[1] + (ex[1] * x + ey[1] * y) * u)
    m = Mask(cv)
    for (top, bot) in FACE_EYES:
        pts = []
        for k in range(13):
            s = k / 12.0
            pts.append(P(top[0] + (bot[0] - top[0]) * s + FACE_EYE_BOW * math.sin(math.pi * s),
                         top[1] + (bot[1] - top[1]) * s))
        m.stroke(pts, FACE_EYE_W * u)
    p0, pc, p2 = FACE_SMILE
    smile = []
    for k in range(25):
        s = k / 24.0
        x = (1 - s) ** 2 * p0[0] + 2 * (1 - s) * s * pc[0] + s * s * p2[0]
        y = (1 - s) ** 2 * p0[1] + 2 * (1 - s) * s * pc[1] + s * s * p2[1]
        smile.append(P(x, y))
    m.stroke(smile, FACE_SMILE_W * u)
    paint(cv, FACE_INK, m)


def face_centre():
    return (JANET[0] + FACE_AT[0] * JR, JANET[1] + FACE_AT[1] * JR)


# --------------------------------------------------------------------------
# Motifs
# --------------------------------------------------------------------------

def sparkle(m, c, r, rot=0.0, pinch=0.18):
    """A concave four-point sparkle (constellations), into a mask."""
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
            s = i / 10.0
            pts.append(((1 - s) ** 2 * p0[0] + 2 * (1 - s) * s * ctrl[0] + s * s * p2[0],
                        (1 - s) ** 2 * p0[1] + 2 * (1 - s) * s * ctrl[1] + s * s * p2[1]))
    m.poly(pts)


def twinkle(cv, c, r):
    """A lens-flare twinkle: a thin four-point cross with a soft glow."""
    glow(cv, c, 0, r * 0.9, hexc("#FFE680"), 0.35, power=1.6)
    m = Mask(cv)
    for (dx, dy, L) in ((1, 0, r), (0, 1, r * 1.15)):
        m.poly([(c[0] - dx * L, c[1] - dy * L), (c[0] + dy * r * 0.10, c[1] + dx * r * 0.10),
                (c[0] + dx * L, c[1] + dy * L), (c[0] - dy * r * 0.10, c[1] - dx * r * 0.10)])
    m.circle(c, r * 0.16)
    paint(cv, hexc("#FFF6C0"), m)


def bokeh(cv, c, r):
    glow(cv, c, 0, r * 3.0, hexc("#FF9A2A"), 0.30, power=1.8)
    glow(cv, c, 0, r * 1.2, hexc("#FFF0C0"), 0.85, power=1.2)
    m = Mask(cv)
    m.circle(c, r * 0.45)
    paint(cv, hexc("#FFFFF4"), m)


def planet(cv, c, r, body, rim, ring=False):
    """A small planet lit from Janet's side (the top-right)."""
    lx, ly = JANET[0] - c[0], JANET[1] - c[1]
    n = math.hypot(lx, ly)
    lx, ly = lx / n, ly / n
    glow(cv, c, r * 0.8, r * 2.6, rim, 0.30, power=1.6)
    m = Mask(cv)
    m.circle(c, r)
    paint(cv, hexc(body), m)
    m = Mask(cv)
    m.circle((c[0] + lx * r * 0.3, c[1] + ly * r * 0.3), r * 0.55)
    paint(cv, rim, m, 0.55, blur=r * 0.25)
    m = Mask(cv)
    m.circle((c[0] + lx * r * 0.38, c[1] + ly * r * 0.38), r * 0.22)
    paint(cv, hexc("#FFFFFF"), m, 0.7)
    if ring:
        m = Mask(cv)
        m.arc(c, r * 1.7, 200, 340, 1.1)
        m.arc(c, r * 1.7, 20, 160, 1.1)
        paint(cv, hexc("#FFE680"), m, 0.8)


def spiky_sun_far(cv, c, R, rot):
    """A spiky sun far back: a body barely lighter than the sky, its rim
    faintly lit (a silhouette darker than near-black would vanish)."""
    def shape(m, grow=0.0):
        m.circle(c, R * 0.52 + grow)
        for k in range(16):
            a = math.radians(rot + k * 22.5)
            L = R * (1.0 if k % 2 == 0 else 0.70) + grow
            hw = R * (0.10 if k % 2 == 0 else 0.08) + grow * 0.5
            ux, uy = math.cos(a), math.sin(a)
            nx, ny = -uy, ux
            b = (c[0] + ux * R * 0.45, c[1] + uy * R * 0.45)
            m.poly([(b[0] + nx * hw, b[1] + ny * hw), (c[0] + ux * L, c[1] + uy * L), (b[0] - nx * hw, b[1] - ny * hw)])
    m = Mask(cv)
    shape(m, 1.2)
    paint(cv, hexc("#5A1E06"), m, 0.55)
    m = Mask(cv)
    shape(m)
    paint(cv, hexc("#250B02"), m, 1.0)
    m = Mask(cv)
    m.ring(c, R * 0.30, 1.4)
    paint(cv, hexc("#5A1E06"), m, 0.45)


def medallion(cv, c, R, part):
    """A zigzag sunburst disc with an orange outline, a thin yellow zigzag
    star and a cyan core in a navy ring (body); two dashed rings that turn."""
    def zig(r_out, r_in, n, rot):
        pts = []
        for k in range(n * 2):
            a = math.radians(rot + k * 180.0 / n)
            rr = r_out if k % 2 == 0 else r_in
            pts.append((c[0] + rr * math.cos(a), c[1] + rr * math.sin(a)))
        return pts
    if part == "body":
        glow(cv, c, R * 0.9, R * 1.6, hexc("#FF9A20"), 0.24, power=1.5)
        m = Mask(cv); m.poly(zig(R, R * 0.86, 14, 0)); paint(cv, hexc("#C8560A"), m)
        m = Mask(cv); m.poly(zig(R * 0.93, R * 0.80, 14, 0)); paint(cv, hexc("#3A1203"), m)
        m = Mask(cv)
        pts = zig(R * 0.62, R * 0.47, 16, 11.25)
        m.line(pts + [pts[0]], max(0.9, R * 0.04), caps=False)
        paint(cv, hexc("#FFD21A"), m)
        m = Mask(cv); m.circle(c, R * 0.30); paint(cv, NAVY, m)
        glow(cv, c, R * 0.1, R * 0.42, CYAN, 0.45, power=1.3)
        m = Mask(cv); m.circle(c, R * 0.21); paint(cv, CYAN, m)
        m = Mask(cv); m.circle((c[0] - R * 0.07, c[1] - R * 0.07), R * 0.065); paint(cv, hexc("#E8FFFF"), m, 0.9)
    elif part == "ring_outer":
        m = Mask(cv)
        for k in range(18):
            a0 = k * 20
            m.arc(c, R * 0.70, a0, a0 + 11, max(0.9, R * 0.035))
        paint(cv, hexc("#B24A08"), m)
    elif part == "ring_inner":
        m = Mask(cv)
        for k in range(12):
            a0 = k * 30
            m.arc(c, R * 0.40, a0, a0 + 18, max(0.8, R * 0.03))
        paint(cv, hexc("#C8560A"), m)


def jewel_kite(cv, c, h, part):
    """Nested gold diamond frames round a cyan square gem in a navy setting
    (body); the gem's light (glint, which the menu pulses)."""
    w = h * 0.58

    def dia(hh, ww):
        return [(c[0], c[1] - hh), (c[0] + ww, c[1]), (c[0], c[1] + hh), (c[0] - ww, c[1])]
    j = h * 0.30
    if part == "body":
        glow(cv, c, h * 0.2, h * 1.3, hexc("#FFC23A"), 0.22, power=1.5)
        m = Mask(cv); m.poly(dia(h, w)); paint(cv, hexc("#C55308"), m)
        m = Mask(cv); m.poly(dia(h - 1.6, w - 1.0)); paint(cv, GOLD, m)
        m = Mask(cv); m.poly(dia(h - 3.6, w - 2.3)); paint(cv, hexc("#4A1203"), m)
        m = Mask(cv)
        pts = dia(h - 6.4, w - 4.1)
        m.line(pts + [pts[0]], 1.1, caps=False)
        paint(cv, GOLD, m)
        m = Mask(cv); m.poly(dia(j + 2.2, j + 2.2)); paint(cv, GOLD, m)
        m = Mask(cv); m.poly(dia(j + 1.0, j + 1.0)); paint(cv, NAVY, m)
        m = Mask(cv); m.poly(dia(j * 0.82, j * 0.82)); paint(cv, CYAN, m)
        m = Mask(cv)
        m.poly([(c[0] - j * 0.25, c[1] - j * 0.45), (c[0], c[1] - j * 0.2), (c[0] - j * 0.25, c[1] + 0.05 * j), (c[0] - j * 0.5, c[1] - j * 0.2)])
        paint(cv, hexc("#E8FFFF"), m, 0.85)
    elif part == "glint":
        glow(cv, c, j * 0.9, j * 2.4, CYAN, 0.32, power=1.4)


def constellation_lines(cv, pts, big):
    m = Mask(cv)
    m.line(pts, 1.15, caps=False)
    paint(cv, hexc("#F6CF10"), m, 0.85)
    for i, p in enumerate(pts):
        if i in big:
            glow(cv, p, 0.5, 9.0, hexc("#FFE680"), 0.55, power=1.6)
    m = Mask(cv)
    for i, p in enumerate(pts):
        m.circle(p, 3.2 if i in big else 1.7)
    paint(cv, hexc("#FFF0A0"), m)


def scallop_band(cv, top, period, depth):
    """The end card's corona bands, one period of them: arches along the top
    edge, bowls along the bottom; black beyond, an orange outer band, a yellow
    inner band and a glow off them into the window."""
    y_edge = 0.0 if top else float(H)
    base = 3.0
    x_from, x_to = 0.0, period
    xs = np.arange(x_from, x_to + 0.125, 0.125)
    u = (xs % period) / period
    u[-1] = 1.0
    bulge = np.sqrt(np.clip(1 - (2 * u - 1) ** 2, 0, 1))
    prof = base + depth * (1 - bulge)

    def poly_at(off):
        if top:
            pts = [(x, y_edge + p + off) for x, p in zip(xs, prof)]
            return [(x_from, -5)] + pts + [(x_to, -5)]
        pts = [(x, y_edge - p - off) for x, p in zip(xs, prof)]
        return [(x_from, H + 5)] + pts + [(x_to, H + 5)]
    if top:
        gm = np.clip(1 - (cv.Y - (base + depth)) / 30.0, 0, 1) ** 2.2
    else:
        gm = np.clip(1 - ((H - base - depth) - cv.Y) / 30.0, 0, 1) ** 2.2
    cv.over(hexc("#FF9A1E"), gm * 0.30)
    m = Mask(cv); m.poly(poly_at(5.6)); paint(cv, hexc("#F49E0B"), m)
    m = Mask(cv); m.poly(poly_at(2.8)); paint(cv, hexc("#EE4D05"), m)
    m = Mask(cv); m.poly(poly_at(0.0)); paint(cv, hexc("#0C0300"), m)


# --------------------------------------------------------------------------
# The scene's parts, at the first size
# --------------------------------------------------------------------------

SKY_STOPS = [(0, "#5C2208"), (150, "#4A1A06"), (230, "#371204"), (330, "#280D02"),
             (470, "#1F0901"), (640, "#1A0800"), (900, "#170700")]
# Rings round Janet, which the menu draws itself: (radius, width, alpha).
ORBITS = [(214, 2.0, 0.52), (306, 1.8, 0.48), (410, 1.6, 0.44), (530, 1.4, 0.40), (650, 1.3, 0.38), (780, 1.2, 0.36)]
ORBIT_COL = "#E8862A"
DOTS = [(312, 33, 1.2), (536, 58, -0.9), (786, 85, 0.6)]     # radius, count, degrees a second
DOT_COL = "#FFC24A"
DOT_ALPHA, DOT_ARC, DOT_W = 0.46, 1.2, 1.6   # each dash: 1.2 degrees of its ring, 1.6 units thick, round ends
PLANETS = [  # orbit r, base angle, rock amplitude, radius, body, rim, ring
    (214, 160.9, 3.0, 6.5, "#F9C21E", "#FFF0A0", False),
    (306, 85.9, 1.2, 6.5, "#25D8EE", "#C8FFFF", True),
    (410, 86.8, 1.0, 5.0, "#F57A12", "#FFD080", False),
    (650, 147.1, 0.9, 8.0, "#F9C21E", "#FFF0A0", False),
]
PLANET_ROCK = (0.45, 1.7)     # angle = base + amp sin(0.45 t + 1.7 k)
CONSTELLATIONS = [  # its anchor: x from the Left edge or Proportionally across the page, y from the Top
    dict(name="header", anchor="PT", pts=[(300, 32), (334, 20), (368, 29), (400, 15), (436, 25), (466, 13), (506, 30)],
         big={2, 5}, seed=1, sparkles=[((352, 40), 4.2), ((420, 36), 3.0), ((486, 22), 3.6)]),
    dict(name="side", anchor="LT", pts=[(152, 168), (132, 192), (148, 228), (124, 256), (142, 292)],
         big={2, 4}, seed=2, sparkles=[((158, 208), 4.2), ((116, 226), 3.0), ((128, 312), 3.4)]),
]
SPARKLE_R = 4.2               # the sparkle sprite's size; each scales 0.85..1 of its own
MEDALLION = ((136.0, 404.0), 19.0, "LB")
MEDALLION_RATES = (14.0, -19.6)   # its outer and inner dashed rings, degrees a second clockwise
KITE = ((526.0, 80.0), 15.0, "PT")
BOKEH = [((560, 128), 2.6, "PT"), ((706, 200), 2.2, "RT"), ((158, 148), 1.8, "LT"), ((96, 470), 2.0, "LB"),
         ((708, 452), 2.4, "RB")]
TWINKLES = [((706, 160), 4.6, "RT"), ((126, 90), 3.8, "LT")]
FAR_SUNS = [((64, 262), 96, 8), ((300, 520), 120, 20)]
CORONA = dict(period=28.0, depth=6.5, speed=4.0, height=44.0)
# Where text sits on the sky: the page header's title, quote and status line,
# and the sidebar's labels (to the window's bottom). Rings go quiet there.
QUIET_ZONES = [(166, 10, 504, 118), (6, 6, 92, 498)]
QUIET_FLOOR, QUIET_BLUR = 0.18, 6.0


def star_list():
    """A sparse starfield, fixed: white dots and a few brighter ones, kept
    out of the text."""
    g = rng(31)
    out = []
    tries = 0
    while len(out) < 150 and tries < 5000:
        tries += 1
        x, y = g.uniform(2, W - 2), g.uniform(2, H - 2)
        if any(x0 - 4 <= x <= x1 + 4 and y0 - 4 <= y <= y1 + 4 for (x0, y0, x1, y1) in QUIET_ZONES):
            continue
        if math.hypot(x - JANET[0], y - JANET[1]) < JR * 1.25:
            continue
        big = g.uniform() < 0.08
        r = g.uniform(0.9, 1.3) if big else g.uniform(0.40, 0.75)
        a = g.uniform(0.55, 0.95) if big else g.uniform(0.30, 0.80)
        out.append((x, y, r, a, big))
    return out


def planet_pos(k, t=0.0):
    r, base, amp, _, _, _, _ = PLANETS[k]
    a = math.radians(base + amp * math.sin(t * PLANET_ROCK[0] + k * PLANET_ROCK[1]))
    return (JANET[0] + r * math.cos(a), JANET[1] + r * math.sin(a))


# --------------------------------------------------------------------------
# The layers
# --------------------------------------------------------------------------

def make_sky():
    """The sky lit by Janet, the starfield and the far suns: opaque."""
    cv = Canvas(W, H, 4)
    d = np.hypot(cv.X - JANET[0], cv.Y - JANET[1])
    cv.px[:] = ramp_stops(d, SKY_STOPS)
    cv.A[:] = 1.0
    stars = star_list()
    soft = Mask(cv)
    for (x, y, r, a, big) in stars:
        if big:
            soft.circle((x, y), r * 2.6, int(70 * a))
    cv.over(hexc("#FFD9A0"), soft.arr(blur=1.2))
    for level in (0.35, 0.55, 0.75, 0.95):
        m = Mask(cv)
        for (x, y, r, a, big) in stars:
            if abs(a - level) <= 0.10 + 1e-6:
                m.circle((x, y), r)
        paint(cv, hexc("#FFF8EC"), m, level)
    for (c, R, rot) in FAR_SUNS:
        spiky_sun_far(cv, c, R, rot)
    return cv.image(opaque=True)


def janet_canvas(box, s):
    x0, y0, x1, y1 = box
    return Canvas(x1 - x0, y1 - y0, s, origin=(JANET[0] + x0, JANET[1] + y0))


def crop(img, drawn, box):
    """The part of `img` (drawn over the box `drawn` round her) over `box`."""
    return img.crop(tuple(int(round((box[i] - drawn[i % 2]) * OUT)) for i in range(4)))


def make_janet():
    """Her four layers, each over its box round her centre. The disc and the
    flames are drawn whole and then cut to their boxes, so their blurs see all
    of her up to a box's edge."""
    cv = janet_canvas(GLOW_BOX, 2)
    janet_glow(cv, JANET, JR)
    glow_img = cv.image(GLOW_OUT)
    whole = (-148.0, -148.0, 148.0, 148.0)
    cv = janet_canvas(whole, 6)
    janet_disc(cv, JANET, JR)
    disc = crop(cv.image(), whole, DISC_BOX)
    whole = (-136.0, -134.0, 136.0, 134.0)
    cv = janet_canvas(whole, 6)
    janet_flames(cv, JANET, JR)
    flames = crop(cv.image(), whole, FLAME_BOX)
    cv = janet_canvas(TOP_BOX, 6)
    janet_limb(cv, JANET, JR)
    janet_face(cv, face_centre(), JR, FACE_TILT)
    top = cv.image()
    return glow_img, disc, flames, top


def sprite_canvas(cx, cy, half_w, half_h=None):
    """A canvas round (cx, cy) whose sides fall on whole pixels."""
    half_h = half_w if half_h is None else half_h
    hw = math.ceil(half_w * OUT) / OUT
    hh = math.ceil(half_h * OUT) / OUT
    return Canvas(2 * hw, 2 * hh, 6, origin=(cx - hw, cy - hh))


def make_sprites():
    """Each sprite with where its top-left sits from its centre (units):
    [(name, image, (left, top))]. A constellation's centre is its lines'
    middle; every other sprite's is the point it's placed by."""
    out = []

    def add(name, cv, c):
        out.append((name, cv.image(), (cv.ox - c[0], cv.oy - c[1])))
    for k, (r, base, amp, pr, body, rim, ring) in enumerate(PLANETS):
        c = planet_pos(k)
        cv = sprite_canvas(c[0], c[1], pr * 2.7)
        planet(cv, c, pr, body, hexc(rim), ring)
        add("planet%d" % k, cv, c)
    for cst in CONSTELLATIONS:
        xs = [p[0] for p in cst["pts"]]
        ys = [p[1] for p in cst["pts"]]
        c = ((min(xs) + max(xs)) / 2.0, (min(ys) + max(ys)) / 2.0)
        cv = sprite_canvas(c[0], c[1], (max(xs) - min(xs)) / 2.0 + 10.0, (max(ys) - min(ys)) / 2.0 + 10.0)
        constellation_lines(cv, cst["pts"], cst["big"])
        add("constellation_" + cst["name"], cv, c)
        cst["centre"] = c
    cv = sprite_canvas(0.0, 0.0, SPARKLE_R + 1.0)
    m = Mask(cv)
    sparkle(m, (0.0, 0.0), SPARKLE_R)
    paint(cv, hexc("#FFF6C8"), m)
    add("sparkle", cv, (0.0, 0.0))
    (mx, my), mR, _ = MEDALLION
    for part, half in (("body", mR * 1.65), ("ring_outer", mR * 0.70 + 2.5), ("ring_inner", mR * 0.40 + 2.5)):
        cv = sprite_canvas(mx, my, half)
        medallion(cv, (mx, my), mR, part)
        add("medallion_" + part, cv, (mx, my))
    (kx, ky), kh, _ = KITE
    for part in ("body", "glint"):
        cv = sprite_canvas(kx, ky, kh * 1.35)
        jewel_kite(cv, (kx, ky), kh, part)
        add("kite_" + part, cv, (kx, ky))
    br = max(r for _, r, _ in BOKEH)
    cv = sprite_canvas(0.0, 0.0, br * 3.1)
    bokeh(cv, (0.0, 0.0), br)
    add("bokeh", cv, (0.0, 0.0))
    tr = max(r for _, r, _ in TWINKLES)
    cv = sprite_canvas(0.0, 0.0, tr * 1.25)
    twinkle(cv, (0.0, 0.0), tr)
    add("twinkle", cv, (0.0, 0.0))
    p, hgt = CORONA["period"], CORONA["height"]
    cv = Canvas(p, hgt, 6, origin=(0.0, 0.0))
    scallop_band(cv, True, p, CORONA["depth"])
    add("corona_top", cv, (0.0, 0.0))
    cv = Canvas(p, hgt, 6, origin=(0.0, H - hgt))
    scallop_band(cv, False, p, CORONA["depth"])
    add("corona_bottom", cv, (0.0, H - hgt))
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


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def save(im, name):
    path = os.path.join(RES, name)
    im.save(path, optimize=True)
    return im.size, os.path.getsize(path)


def f(v):
    """A C++ float literal."""
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    if s in ("-0", ""):
        s = "0"
    return s + ("f" if "." in s else ".f")


def main():
    os.makedirs(RES, exist_ok=True)
    sizes = {}
    sizes["sky"] = save(make_sky(), "solar_sky.png")
    glow_img, disc, flames, top = make_janet()
    sizes["glow"] = save(glow_img, "solar_glow.png")
    sizes["disc"] = save(disc, "solar_disc.png")
    sizes["flames"] = save(flames, "solar_flames.png")
    sizes["top"] = save(top, "solar_top.png")
    sprites = make_sprites()
    sprite_at, sprite_h = pack([s[1] for s in sprites], 512)
    atlas = Image.new("RGBA", (512, sprite_h), (0, 0, 0, 0))
    for (name, im, off), (x, y) in zip(sprites, sprite_at):
        atlas.paste(im, (x, y))
    sizes["sprites"] = save(atlas, "solar_sprites.png")
    write_header(sizes, sprites, sprite_at)
    total = 0
    for k, ((w_, h_), b) in sizes.items():
        print("%-8s %4d x %4d  %7d bytes" % (k, w_, h_, b))
        total += b
    print("total %d bytes (%.1f KB)" % (total, total / 1024.0))


def write_header(sizes, sprites, sprite_at):
    L = []
    w = L.append
    index = {name: i for i, (name, _, _) in enumerate(sprites)}
    w("// Generated by tools/solar_art.py: don't edit it by hand, run that again.")
    w("//")
    w("// Where SolarBot's art (resources/solar/*.png) goes, in menu units at the")
    w("// menu's first size (720 x 500), and the rings the menu draws itself. The")
    w("// files are kPx pixels a unit (the glow kGlowPx); rects in them are pixels.")
    w("")
    w("#pragma once")
    w("")
    w("namespace gucci::ui::pages::solar_art {")
    w("")
    w("    inline constexpr float kPx = %s, kGlowPx = %s;" % (f(OUT), f(GLOW_OUT)))
    w("    inline constexpr float kDesignW = %s, kDesignH = %s;" % (f(W), f(H)))
    w("    inline constexpr float kPageLeft = %s;  // the page's left edge at that size" % f(PAGE_LEFT))
    w("")
    w("    struct File {")
    w("        const char* name;")
    w("        int w, h;")
    w("    };")
    names = [("kSky", "solar_sky.png", "sky"), ("kGlow", "solar_glow.png", "glow"), ("kDisc", "solar_disc.png", "disc"),
             ("kFlames", "solar_flames.png", "flames"), ("kTop", "solar_top.png", "top"),
             ("kSprites", "solar_sprites.png", "sprites")]
    for var, name, key in names:
        (pw, ph), _ = sizes[key]
        w("    inline constexpr File %s{\"%s\", %d, %d};" % (var, name, pw, ph))
    w("")
    w("    // Janet: her centre kJanetRight left of the window's right edge and")
    w("    // kJanetDown below its top (above it: it's negative), radius kJanetR. Each")
    w("    // of her pictures covers a box round her centre (left, top, right, bottom):")
    w("    // only what the window can show of her. The flames are a tile 2R tall that")
    w("    inline constexpr float kJanetRight = %s, kJanetDown = %s, kJanetR = %s;" % (f(JANET_RIGHT), f(JANET_DOWN), f(JR)))
    for var, box in (("kGlowBox", GLOW_BOX), ("kDiscBox", DISC_BOX), ("kFlameBox", FLAME_BOX), ("kTopBox", TOP_BOX)):
        w("    inline constexpr float %s[4] = {%s};" % (var, ", ".join(f(v) for v in box)))
    w("    // Her glow breathes: scale 1 + kBreathe sin(kBreatheRate t). Her flames")
    w("    // rise kFlameRise of their tile a second.")
    w("    inline constexpr float kBreathe = %s, kBreatheRate = %s;" % (f(GLOW_BREATHE[0]), f(GLOW_BREATHE[1])))
    w("    inline constexpr float kFlameRise = %s;" % f(FLAME_RISE))
    w("")
    w("    // A sprite: its rect in kSprites (pixels), and where its top-left sits")
    w("    // from its centre (units).")
    w("    struct Sprite {")
    w("        float u0, v0, u1, v1;")
    w("        float left, top;")
    w("    };")
    w("    inline constexpr Sprite kSpriteRects[] = {")
    for i, ((name, im, (ox, oy)), (x, y)) in enumerate(zip(sprites, sprite_at)):
        w("        {%d, %d, %d, %d, %s, %s},  // %d %s" % (x, y, x + im.width, y + im.height, f(ox), f(oy), i, name))
    w("    };")
    w("    inline constexpr int kSparkleSprite = %d, kBokehSprite = %d, kTwinkleSprite = %d;" % (
        index["sparkle"], index["bokeh"], index["twinkle"]))
    w("    inline constexpr int kMedallionBody = %d, kMedallionOuter = %d, kMedallionInner = %d;" % (
        index["medallion_body"], index["medallion_ring_outer"], index["medallion_ring_inner"]))
    w("    inline constexpr int kKiteBody = %d, kKiteGlint = %d;" % (index["kite_body"], index["kite_glint"]))
    w("    inline constexpr int kCoronaTop = %d, kCoronaBottom = %d;" % (index["corona_top"], index["corona_bottom"]))
    w("")
    w("    // Something placed in the menu: its sprite, its centre at the first size,")
    w("    // and how it holds when the window is another size: x from the Left or")
    w("    // Right edge or Proportionally across the page right of the sidebar, y")
    w("    // from the Top or Bottom.")
    w("    struct Place {")
    w("        int sprite;")
    w("        float x, y;")
    w("        char ax, ay;")
    w("    };")
    w("    // The constellations' lines and dots, then the medallion and the kite.")
    w("    inline constexpr Place kConstellations[] = {")
    for cst in CONSTELLATIONS:
        c = cst["centre"]
        w("        {%d, %s, %s, '%s', '%s'},  // %s" % (index["constellation_" + cst["name"]], f(c[0]), f(c[1]),
                                                  cst["anchor"][0], cst["anchor"][1], cst["name"]))
    w("    };")
    (mx, my), _, ma = MEDALLION
    (kx, ky), _, ka = KITE
    w("    inline constexpr Place kMedallion{%d, %s, %s, '%s', '%s'};" % (index["medallion_body"], f(mx), f(my), ma[0], ma[1]))
    w("    inline constexpr Place kKite{%d, %s, %s, '%s', '%s'};" % (index["kite_body"], f(kx), f(ky), ka[0], ka[1]))
    w("    // The medallion's dashed rings turn (degrees a second, clockwise).")
    w("    inline constexpr float kMedallionOuterRate = %s, kMedallionInnerRate = %s;" % (
        f(MEDALLION_RATES[0]), f(MEDALLION_RATES[1])))
    w("")
    w("    // A constellation's sparkles: from its centre (units), each kSparkleSprite")
    w("    // at r / kSparkleR of its size times 0.85 + 0.15 sin(2t + k + seed).")
    w("    struct Sparkle {")
    w("        int constellation;")
    w("        float dx, dy, r;")
    w("        int k, seed;")
    w("    };")
    w("    inline constexpr float kSparkleR = %s;" % f(SPARKLE_R))
    w("    inline constexpr Sparkle kSparkles[] = {")
    for ci, cst in enumerate(CONSTELLATIONS):
        c = cst["centre"]
        for k, (p, r) in enumerate(cst["sparkles"]):
            w("        {%d, %s, %s, %s, %d, %d}," % (ci, f(p[0] - c[0]), f(p[1] - c[1]), f(r), k, cst["seed"]))
    w("    };")
    w("")
    w("    // Small lights, each its sprite at r / the sprite's own r: a bokeh's")
    w("    // brightness 0.85 + 0.15 sin(1.6t + 2.3k), a twinkle's size 0.8 + 0.2")
    w("    // sin(2.1t + 1.3k).")
    w("    struct Light {")
    w("        float x, y, r;")
    w("        char ax, ay;")
    w("    };")
    w("    inline constexpr float kBokehR = %s, kTwinkleR = %s;" % (f(max(r for _, r, _ in BOKEH)),
                                                                  f(max(r for _, r, _ in TWINKLES))))
    w("    inline constexpr Light kBokeh[] = {")
    for (p, r, a) in BOKEH:
        w("        {%s, %s, %s, '%s', '%s'}," % (f(p[0]), f(p[1]), f(r), a[0], a[1]))
    w("    };")
    w("    inline constexpr Light kTwinkles[] = {")
    for (p, r, a) in TWINKLES:
        w("        {%s, %s, %s, '%s', '%s'}," % (f(p[0]), f(p[1]), f(r), a[0], a[1]))
    w("    };")
    w("")
    w("    // The planets ride orbits round Janet: at radius r, rocking along it,")
    w("    // angle = base + amp sin(kPlanetRockRate t + kPlanetRockStep k) degrees,")
    w("    // clockwise from her right.")
    w("    struct Planet {")
    w("        int sprite;")
    w("        float r, base, amp;")
    w("    };")
    w("    inline constexpr float kPlanetRockRate = %s, kPlanetRockStep = %s;" % (f(PLANET_ROCK[0]), f(PLANET_ROCK[1])))
    w("    inline constexpr Planet kPlanets[] = {")
    for k, (r, base, amp, pr, body, rim, ring) in enumerate(PLANETS):
        w("        {%d, %s, %s, %s}," % (index["planet%d" % k], f(r), f(base), f(amp)))
    w("    };")
    w("")
    w("    // Rings round Janet the menu draws itself. Orbits: (radius, width, alpha)")
    w("    // in kOrbitColor. Dotted rings: kDotArc-degree dashes kDotWidth thick,")
    w("    // `count` round the ring, turning `rate` degrees a second clockwise, in")
    w("    // kDotColor at kDotAlpha. RGB.")
    w("    struct Orbit {")
    w("        float r, width, alpha;")
    w("    };")
    w("    inline constexpr Orbit kOrbits[] = {")
    for (r, wd, a) in ORBITS:
        w("        {%s, %s, %s}," % (f(r), f(wd), f(a)))
    w("    };")
    w("    struct Dots {")
    w("        float r;")
    w("        int count;")
    w("        float rate;")
    w("    };")
    w("    inline constexpr Dots kDots[] = {")
    for (r, n, rate) in DOTS:
        w("        {%s, %d, %s}," % (f(r), n, f(rate)))
    w("    };")
    oc, dc = hexc(ORBIT_COL), hexc(DOT_COL)
    w("    inline constexpr float kOrbitColor[3] = {%s};  // %s" % (", ".join(f(v) for v in oc), ORBIT_COL))
    w("    inline constexpr float kDotColor[3] = {%s};  // %s" % (", ".join(f(v) for v in dc), DOT_COL))
    w("    inline constexpr float kDotAlpha = %s, kDotArc = %s, kDotWidth = %s;" % (f(DOT_ALPHA), f(DOT_ARC), f(DOT_W)))
    w("")
    w("    // Where the rings go quiet (to kQuietFloor of their alpha), behind the")
    w("    // header's words and the sidebar's: rects from the window's top-left")
    w("    // (x0, y0, x1, y1; a negative y1 is from the bottom), softened by a")
    w("    // Gaussian kQuietBlur units wide.")
    w("    inline constexpr float kQuietZones[][4] = {")
    for (x0, y0, x1, y1) in QUIET_ZONES:
        w("        {%s, %s, %s, %s}," % (f(x0), f(y0), f(x1), f(y1 if y1 < H - 10 else -(H - y1))))
    w("    };")
    w("    inline constexpr float kQuietFloor = %s, kQuietBlur = %s;" % (f(QUIET_FLOOR), f(QUIET_BLUR)))
    w("")
    w("    // The corona bands: a tile one period wide along the top and the bottom,")
    w("    // the top's drifting left and the bottom's right at kCoronaSpeed.")
    w("    inline constexpr float kCoronaPeriod = %s, kCoronaHeight = %s, kCoronaSpeed = %s;" % (
        f(CORONA["period"]), f(CORONA["height"]), f(CORONA["speed"])))
    w("")
    w("} // namespace gucci::ui::pages::solar_art")
    with open(HEADER, "w", newline="\n") as fh:
        fh.write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
