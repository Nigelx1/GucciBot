"""Test pictures for GucciBot's PNG reader (src/ui/png.cpp), which
tools/run_art_tests.cpp checks (run_tests.bat).

Writes tools/testdata/png/:
  packaged/       the JupiterBot art exactly as Geode's packager stores it in
                  the .geode. It re-saves pictures with few colors as palettes
                  of 4- or 8-bit indexes with a tRNS chunk, which build
                  2026-11-02-a's reader refused, so the art never showed.
  variants/       small pictures of every color type, bit depth and interlace
                  PNG has, with and without tRNS. Pillow saves the kinds it can
                  (pil_*); a small writer here saves the rest (Adam7, 2- and
                  4-bit gray, 16-bit color, gray with alpha at 16 bits), using
                  all five filters, IDATs split up and the padding bits set.
  bad/            files the reader must refuse, each with the words its error
                  has to contain.
  expected.rgba   what each variant decodes to, RGBA, one after another.
  manifest.txt    what the test reads: see its header.

The reference is Pillow's decode, as RGBA. Where Pillow's own RGBA conversion
goes against the PNG spec, the spec is applied by hand to the samples Pillow
decoded:
  16-bit gray        Pillow clips it to 255. Here: the high byte, as Pillow
                     itself does for every other 16-bit type.
  2- and 4-bit gray  Pillow compares a tRNS key with the stretched gray (85 for
  with a key         a 2-bit 1) and never matches. Here: the stored sample.
  16-bit RGB with    Pillow keeps only the high bytes, so it can't match a
  a key              16-bit key. Here: the writer's own samples, after checking
                     Pillow's high bytes against them.
Every variant the writer made is also checked against its own samples, so a
mistake in the writer stops the script instead of becoming a wrong reference.

Usage, from the repo root:
  python -I tools/make_png_tests.py [path to an installed nigelx1.guccibot.geode]
Without a .geode (the installed one is looked for), packaged/ stays as it is.
"""

import io
import os
import struct
import sys
import zipfile
import zlib

import numpy as np
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "png")
DEFAULT_GEODE = (r"C:\Program Files (x86)\Steam\steamapps\common\Geode Geometry Dash"
                 r"\geode\mods\nigelx1.guccibot.geode")
SIGNATURE = b"\x89PNG\r\n\x1a\n"
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
DEPTHS = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}

rng = np.random.default_rng(20261102)


# ---- a small PNG writer ---------------------------------------------------

def chunk(kind, body, crc=None):
    if crc is None:
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)


def ihdr(width, height, depth, color, interlace=0):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace))


def pack_row(row, depth):
    """One row of samples (w, c) as stored; under 8 bits the unused low bits
    of the last byte are set, which a reader must ignore."""
    flat = row.reshape(-1).astype(np.uint32)
    if depth == 16:
        return flat.astype(">u2").tobytes()
    if depth == 8:
        return flat.astype(np.uint8).tobytes()
    per = 8 // depth
    out = bytearray((len(flat) * depth + 7) // 8)
    for i, v in enumerate(flat):
        out[i // per] |= int(v) << (8 - depth * (i % per + 1))
    used = len(flat) * depth % 8
    if used:
        out[-1] |= (1 << (8 - used)) - 1
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filter_row(kind, cur, prev, bpp):
    out = bytearray(len(cur))
    for i in range(len(cur)):
        a = cur[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        predict = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        out[i] = (cur[i] - predict) & 0xFF
    return bytes([kind]) + bytes(out)


def scanlines(samples, depth, interlace):
    """The filtered data: every pass's rows (one pass when not interlaced),
    the filters taking turns row by row so each is used, a pass's first row
    included."""
    h, w, c = samples.shape
    bpp = max(1, c * depth // 8)
    raw = bytearray()
    n = 0
    for x0, y0, dx, dy in (ADAM7 if interlace else [(0, 0, 1, 1)]):
        sub = samples[y0::dy, x0::dx]
        if sub.shape[0] == 0 or sub.shape[1] == 0:
            continue  # an empty pass isn't stored at all
        prev = bytes(len(pack_row(sub[0], depth)))
        for y in range(sub.shape[0]):
            cur = pack_row(sub[y], depth)
            raw += filter_row(n % 5, cur, prev, bpp)
            n += 1
            prev = cur
    return bytes(raw)


def write_png(samples, depth, color, interlace=0, plte=None, trns=None, split=None, extra=True,
              raw=None, bad_text_crc=False):
    h, w, _ = samples.shape
    out = SIGNATURE + ihdr(w, h, depth, color, interlace)
    if extra:  # chunks the reader has to step over
        out += chunk(b"gAMA", struct.pack(">I", 45455))
        out += chunk(b"tEXt", b"Comment\0GucciBot test picture",
                     crc=0x12345678 if bad_text_crc else None)
    if plte is not None:
        out += chunk(b"PLTE", plte)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    data = zlib.compress(raw if raw is not None else scanlines(samples, depth, interlace), 9)
    if split:
        pieces = [data[i:i + split] for i in range(0, len(data), split)]
        pieces.insert(1, b"")  # an empty IDAT is allowed
        for piece in pieces:
            out += chunk(b"IDAT", piece)
    else:
        out += chunk(b"IDAT", data)
    if extra:
        out += chunk(b"gbTs", b"a private chunk after the picture")
    return out + chunk(b"IEND", b"")


# ---- what a picture should decode to ----------------------------------------

def header_of(data):
    w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", data[16:29])
    return w, h, depth, color, interlace


def to8(v, depth):
    v = v.astype(np.uint32)
    if depth == 16:
        return (v >> 8).astype(np.uint8)
    return (v * (255 // ((1 << depth) - 1))).astype(np.uint8)


def from_samples(samples, depth, color, plte=None, trns=None):
    """The RGBA the spec gives the writer's samples: the writer's own check."""
    h, w, _ = samples.shape
    rgba = np.zeros((h, w, 4), np.uint8)
    if color == 3:
        table = np.zeros((256, 4), np.uint8)
        table[:, 3] = 255
        entries = np.frombuffer(plte, np.uint8).reshape(-1, 3)
        table[:len(entries), :3] = entries
        if trns:
            table[:len(trns), 3] = np.frombuffer(trns, np.uint8)
        return table[samples[..., 0]]
    gray = color in (0, 4)
    for c in range(3):
        rgba[..., c] = to8(samples[..., 0 if gray else c], depth)
    if color in (4, 6):
        rgba[..., 3] = to8(samples[..., -1], depth)
    else:
        rgba[..., 3] = 255
        if trns is not None:
            key = np.array(struct.unpack(">%dH" % (len(trns) // 2), trns), np.int64)
            rgba[..., 3][np.all(samples == key, axis=-1)] = 0
    return rgba


def reference(data, samples=None):
    """Pillow's decode as RGBA, with the spec applied where Pillow's own
    conversion goes against it (see the top of this file)."""
    w, h, depth, color, _ = header_of(data)
    im = Image.open(io.BytesIO(data))
    im.load()
    key = im.info.get("transparency") if color in (0, 2) else None
    if color == 0 and depth == 16:
        raw = np.array(im).astype(np.uint32)
        rgba = np.repeat((raw >> 8).astype(np.uint8)[..., None], 4, axis=2)
        rgba[..., 3] = 255
        if key is not None:
            rgba[..., 3][raw == key] = 0
    elif color == 0 and depth in (2, 4) and key is not None:
        assert im.mode == "L"
        gray = np.array(im)  # stretched: a 2-bit 1 is 85
        rgba = np.stack([gray, gray, gray, np.full_like(gray, 255)], axis=-1)
        raw = gray.astype(np.uint32) // (255 // ((1 << depth) - 1))
        rgba[..., 3][raw == (key & ((1 << depth) - 1))] = 0
    elif color == 2 and depth == 16 and key is not None:
        assert im.mode == "RGB"
        high = np.array(im)
        assert samples is not None, "a 16-bit RGB key needs the writer's samples"
        assert np.array_equal(high, (samples >> 8).astype(np.uint8)), "Pillow's high bytes"
        rgba = np.concatenate([high, np.full(high.shape[:2] + (1,), 255, np.uint8)], axis=-1)
        rgba[..., 3][np.all(samples == np.array(key), axis=-1)] = 0
    else:
        rgba = np.array(im.convert("RGBA"))
    assert rgba.shape == (h, w, 4) and rgba.dtype == np.uint8
    return rgba


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---- the pictures -------------------------------------------------------------

def random_samples(h, w, color, depth, palette_size=None):
    top = palette_size if color == 3 and palette_size else 1 << depth
    s = rng.integers(0, top, size=(h, w, CHANNELS[color]), dtype=np.int64)
    if color in (4, 6):  # some fully clear and fully solid pixels
        s[0, 0, -1] = 0
        s[-1, -1, -1] = (1 << depth) - 1
    return s


def random_palette(n):
    return rng.integers(0, 256, size=n * 3, dtype=np.int64).astype(np.uint8).tobytes()


def with_key(samples, depth):
    """A key for a gray or RGB picture, put in about a quarter of its pixels."""
    key = rng.integers(0, 1 << depth, size=samples.shape[-1], dtype=np.int64)
    mask = rng.random(samples.shape[:2]) < 0.25
    samples[mask] = key
    return key


class Set:
    def __init__(self):
        self.lines = []
        self.expected = bytearray()

    def add(self, name, data, samples=None, check=None, reference_data=None):
        """A variant: saved, decoded by Pillow (from `reference_data` when Pillow
        won't read this one), checked against the writer's samples."""
        path = "variants/" + name + ".png"
        with open(os.path.join(ROOT, path), "wb") as f:
            f.write(data)
        rgba = reference(reference_data or data, samples)
        if check is not None:
            assert np.array_equal(rgba, check), name + ": Pillow's decode isn't what was written"
        w, h = header_of(data)[:2]
        self.lines.append("%s %d %d %d %016x" % (path, w, h, len(self.expected), fnv1a64(rgba.tobytes())))
        self.expected += rgba.tobytes()

    def made(self, name, samples, depth, color, interlace=0, plte=None, trns=None, **kw):
        data = write_png(samples, depth, color, interlace, plte, trns, **kw)
        check = from_samples(samples, depth, color, plte, trns)
        self.add(name, data, samples, check)

    def pillow(self, name, im, depth, color, **save):
        buf = io.BytesIO()
        im.save(buf, "PNG", **save)
        data = buf.getvalue()
        got = header_of(data)
        assert (got[2], got[3]) == (depth, color), "%s: Pillow wrote %s" % (name, got)
        self.add(name, data)

    def refuse(self, name, data, words):
        path = "bad/" + name + ".png"
        with open(os.path.join(ROOT, path), "wb") as f:
            f.write(data)
        self.lines.append("refuse %s %s" % (path, words))


def variants(out):
    # Every color type and depth, as a plain picture and interlaced.
    for color, depths in DEPTHS.items():
        for depth in depths:
            for interlace in (0, 1):
                h, w = (19, 21) if interlace else (11, 13)
                s = random_samples(h, w, color, depth)
                plte = random_palette(1 << depth) if color == 3 else None
                out.made("c%d_d%d_%s" % (color, depth, "adam7" if interlace else "plain"), s, depth, color,
                         interlace, plte, split=7 if (color + depth + interlace) % 3 == 0 else None)

    # tRNS: gray and RGB keys at every depth, palettes with fewer alphas
    # than colors (the rest solid).
    for color, depth, interlace in [(0, 1, 0), (0, 2, 0), (0, 4, 0), (0, 8, 0), (0, 16, 0), (0, 2, 1),
                                    (0, 16, 1), (2, 8, 0), (2, 16, 0), (2, 8, 1), (2, 16, 1)]:
        h, w = (19, 21) if interlace else (11, 13)
        s = random_samples(h, w, color, depth)
        key = with_key(s, depth)
        if color == 2 and depth == 16:
            # The key's high bytes with other low bytes: solid, as only all
            # 16 bits of each sample count.
            near = key.copy()
            near[0] ^= 1
            s[0, 1:4] = near
        trns = struct.pack(">%dH" % len(key), *[int(k) for k in key])
        out.made("c%d_d%d_key_%s" % (color, depth, "adam7" if interlace else "plain"), s, depth, color, interlace,
                 trns=trns)
    for depth, interlace in [(1, 0), (2, 0), (4, 0), (8, 0), (4, 1), (8, 1)]:
        h, w = (19, 21) if interlace else (11, 13)
        n = 1 << depth
        s = random_samples(h, w, 3, depth)
        alphas = rng.integers(0, 256, size=max(1, n - 1 - n // 4), dtype=np.int64)
        alphas[0] = 0
        out.made("c3_d%d_alpha_%s" % (depth, "adam7" if interlace else "plain"), s, depth, 3, interlace,
                 plte=random_palette(n), trns=alphas.astype(np.uint8).tobytes())
    # A palette shorter than its depth allows, as the packager writes them.
    s = random_samples(11, 13, 3, 8, palette_size=13)
    out.made("c3_d8_short_palette", s, 8, 3, plte=random_palette(13), trns=bytes([0, 64, 128, 200]))
    s = random_samples(11, 13, 3, 4, palette_size=11)
    out.made("c3_d4_short_palette", s, 4, 3, plte=random_palette(11), trns=bytes(range(0, 250, 25)))

    # Adam7 on small pictures, where some passes are empty.
    for w, h in [(1, 1), (1, 9), (9, 1), (2, 2), (3, 3), (5, 7), (8, 8), (9, 9)]:
        out.made("c6_d8_adam7_%dx%d" % (w, h), random_samples(h, w, 6, 8), 8, 6, 1)
    for color, depth, (w, h) in [(0, 1, (5, 7)), (0, 1, (9, 9)), (3, 2, (3, 3)), (3, 2, (9, 1)), (0, 4, (7, 1)),
                                 (2, 16, (2, 2)), (4, 16, (3, 5)), (3, 4, (1, 1))]:
        plte = random_palette(1 << depth) if color == 3 else None
        out.made("c%d_d%d_adam7_%dx%d" % (color, depth, w, h), random_samples(h, w, color, depth), depth, color, 1,
                 plte)

    # Things a reader has to step over: an ancillary chunk with a wrong CRC
    # (libpng lets it go; Pillow refuses the file, so its pixels come from the
    # same picture without the damage), and more picture data than needed.
    s = random_samples(11, 13, 6, 8)
    good = write_png(s, 8, 6)
    out.add("text_bad_crc", write_png(s, 8, 6, bad_text_crc=True), s, from_samples(s, 8, 6), reference_data=good)
    s = random_samples(11, 13, 2, 8)
    extra = scanlines(s, 8, 0) + bytes([0]) + bytes(13 * 3)
    out.add("extra_data", write_png(s, 8, 2, raw=extra), s, from_samples(s, 8, 2),
            reference_data=write_png(s, 8, 2))

    # Saved by Pillow.
    size = (13, 11)

    def image(mode, array):
        return Image.frombytes(mode, size, np.ascontiguousarray(array).tobytes())

    gray = rng.integers(0, 256, size=(11, 13), dtype=np.int64).astype(np.uint8)
    out.pillow("pil_1", Image.fromarray(gray > 127), 1, 0)
    out.pillow("pil_l", image("L", gray), 8, 0)
    out.pillow("pil_l_key", image("L", gray), 8, 0, transparency=int(gray[3, 4]))
    la = rng.integers(0, 256, size=(11, 13, 2), dtype=np.int64).astype(np.uint8)
    out.pillow("pil_la", image("LA", la), 8, 4)
    rgb = rng.integers(0, 256, size=(11, 13, 3), dtype=np.int64).astype(np.uint8)
    rgb[5:8, 2:6] = rgb[0, 0]
    out.pillow("pil_rgb", image("RGB", rgb), 8, 2)
    out.pillow("pil_rgb_key", image("RGB", rgb), 8, 2, transparency=tuple(int(v) for v in rgb[0, 0]))
    out.pillow("pil_rgb_optimized", image("RGB", rgb), 8, 2, optimize=True)
    rgba = rng.integers(0, 256, size=(11, 13, 4), dtype=np.int64).astype(np.uint8)
    out.pillow("pil_rgba", image("RGBA", rgba), 8, 6)
    for depth in (1, 2, 4, 8):
        n = 1 << depth
        idx = rng.integers(0, n, size=(11, 13), dtype=np.int64).astype(np.uint8)
        im = image("P", idx)
        im.putpalette(list(random_palette(n)))
        out.pillow("pil_p%d" % depth, im, depth, 3)
        alphas = rng.integers(0, 256, size=n, dtype=np.int64).astype(np.uint8).tobytes()
        out.pillow("pil_p%d_alpha" % depth, im, depth, 3, transparency=alphas)
        out.pillow("pil_p%d_clear_index" % depth, im, depth, 3, transparency=int(idx[2, 2]))
    wide = rng.integers(0, 65536, size=(11, 13), dtype=np.int64).astype("<u2")
    out.pillow("pil_i16", image("I;16", wide), 16, 0)
    # RGBA with few colors made a palette with alphas by Pillow's quantizer,
    # the kind of file the packager makes of the art.
    few = np.array([[0, 0, 0, 0], [249, 245, 88, 255], [249, 245, 88, 90], [14, 165, 249, 255],
                    [7, 4, 63, 255], [255, 255, 255, 30], [28, 163, 198, 200], [2, 1, 20, 255],
                    [74, 65, 184, 128]], np.uint8)
    picture = few[rng.integers(0, len(few), size=(11, 13))]
    pal = image("RGBA", picture).quantize(colors=16, method=Image.Quantize.FASTOCTREE)
    out.pillow("pil_rgba_as_palette", pal, 4, 3)


def refusals(out):
    s = random_samples(11, 13, 6, 8)
    good = write_png(s, 8, 6, extra=False)
    data = zlib.compress(scanlines(s, 8, 0), 9)

    def with_idat(payload, depth=8, color=6, interlace=0, before=b""):
        return SIGNATURE + before + ihdr(13, 11, depth, color, interlace) + chunk(b"IDAT", payload) + chunk(b"IEND", b"")

    out.refuse("short_data", with_idat(data[:len(data) // 2]), "stops short")
    out.refuse("garbage_data", with_idat(data[:2] + bytes([0xFF] * 40) + data[42:]), "is damaged")
    flipped = bytearray(good)
    flipped[good.index(b"IDAT") + 10] ^= 0x40
    out.refuse("damaged_idat", bytes(flipped), "damaged IDAT")
    out.refuse("cut_in_half", good[:len(good) // 2], "past the end")
    out.refuse("no_palette", with_idat(zlib.compress(scanlines(random_samples(11, 13, 3, 4), 4, 0)), 4, 3),
               "without a palette")
    out.refuse("palette_16_bit", with_idat(data, 16, 3), "doesn't have")
    out.refuse("rgb_4_bit", with_idat(data, 4, 2), "doesn't have")
    out.refuse("interlace_2", with_idat(data, interlace=2), "interlace method")
    out.refuse("zero_wide", SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", 0, 11, 8, 6, 0, 0, 0)) +
               chunk(b"IDAT", data) + chunk(b"IEND", b""), "unlikely size")
    bad_filter = bytearray(scanlines(s, 8, 0))
    bad_filter[(13 * 4 + 1) * 3] = 5  # row 3's filter byte
    out.refuse("filter_5", with_idat(zlib.compress(bytes(bad_filter), 9)), "filtered")
    out.refuse("cgbi", with_idat(data, before=chunk(b"CgBI", b"\x50\x00\x20\x06")), "CgBI")
    out.refuse("idat_first", SIGNATURE + chunk(b"IDAT", data) + ihdr(13, 11, 8, 6) + chunk(b"IEND", b""),
               "before its header")
    out.refuse("header_only", SIGNATURE + ihdr(13, 11, 8, 6) + chunk(b"IEND", b""), "no picture")


def packaged(out, geode):
    """The art as the packager stores it, from an installed .geode."""
    folder = os.path.join(ROOT, "packaged")
    if geode and os.path.exists(geode):
        with zipfile.ZipFile(geode) as z:
            names = [n for n in z.namelist()
                     if n.startswith("resources/nigelx1.guccibot/jupiter_") and n.endswith(".png")]
            assert names, "no JupiterBot art in " + geode
            for n in names:
                with open(os.path.join(folder, os.path.basename(n)), "wb") as f:
                    f.write(z.read(n))
        print("packaged/: from", geode)
    else:
        print("packaged/: kept (no .geode at %s)" % geode)
    for name in sorted(os.listdir(folder)):
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        w, h, depth, color, interlace = header_of(data)
        rgba = reference(data)
        out.lines.append("packaged/%s %d %d - %016x" % (name, w, h, fnv1a64(rgba.tobytes())))
        print("  %s %dx%d color type %d, %d-bit%s" % (name, w, h, color, depth, ", Adam7" if interlace else ""))


def main():
    for sub in ("packaged", "variants", "bad"):
        os.makedirs(os.path.join(ROOT, sub), exist_ok=True)
    for sub in ("variants", "bad"):
        for name in os.listdir(os.path.join(ROOT, sub)):
            os.remove(os.path.join(ROOT, sub, name))
    out = Set()
    packaged(out, sys.argv[1] if len(sys.argv) > 1 else DEFAULT_GEODE)
    variants(out)
    refusals(out)
    with open(os.path.join(ROOT, "expected.rgba"), "wb") as f:
        f.write(out.expected)
    with open(os.path.join(ROOT, "manifest.txt"), "w", newline="\n") as f:
        f.write("# Made by tools/make_png_tests.py: don't edit it by hand, run that again.\n"
                "# <png> <width> <height> <offset of its RGBA in expected.rgba, or - for none> <FNV-1a 64 of it>\n"
                "# refuse <png> <words its error has to contain>\n")
        f.write("\n".join(out.lines) + "\n")
    shown = sum(1 for line in out.lines if not line.startswith("refuse"))
    print("%d pictures, %d refusals, expected.rgba %d bytes" % (shown, len(out.lines) - shown, len(out.expected)))


if __name__ == "__main__":
    main()
