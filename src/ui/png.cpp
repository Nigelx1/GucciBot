// The PNG reader for the menu's art (ui/png.hpp).

#include "ui/png.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace gucci::ui::png {

    namespace {

        uint32_t be32(uint8_t const* p) {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        uint32_t be16(uint8_t const* p) {
            return uint32_t(p[0]) << 8 | uint32_t(p[1]);
        }

        bool is(uint8_t const* type, char const* name) {
            return std::memcmp(type, name, 4) == 0;
        }

        // PNG's Paeth predictor: whichever of left, up and up-left is
        // nearest left + up - up-left.
        uint8_t paeth(int a, int b, int c) {
            int const p = a + b - c;
            int const pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }

        // Puts back a row the encoder filtered: `cur` in place, `up` the row
        // above as it was before filtering (zeros for a pass's first), `bpp`
        // bytes a pixel (1 for pixels smaller than a byte).
        bool unfilter(uint8_t type, uint8_t* cur, uint8_t const* up, size_t stride, size_t bpp) {
            switch (type) {
                case 0: return true;
                case 1:
                    for (size_t i = bpp; i < stride; ++i)
                        cur[i] = static_cast<uint8_t>(cur[i] + cur[i - bpp]);
                    return true;
                case 2:
                    for (size_t i = 0; i < stride; ++i)
                        cur[i] = static_cast<uint8_t>(cur[i] + up[i]);
                    return true;
                case 3:
                    for (size_t i = 0; i < stride; ++i) {
                        int const left = i >= bpp ? cur[i - bpp] : 0;
                        cur[i] = static_cast<uint8_t>(cur[i] + (left + up[i]) / 2);
                    }
                    return true;
                case 4:
                    for (size_t i = 0; i < stride; ++i) {
                        int const left = i >= bpp ? cur[i - bpp] : 0;
                        int const upLeft = i >= bpp ? up[i - bpp] : 0;
                        cur[i] = static_cast<uint8_t>(cur[i] + paeth(left, up[i], upLeft));
                    }
                    return true;
                default: return false;
            }
        }

        // What the header, palette and tRNS chunks say: everything a pixel's
        // color depends on besides its own samples.
        struct Format {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t depth = 0;     // bits a sample: 1, 2, 4, 8 or 16
            uint32_t color = 0;     // PNG's color type: 0 gray, 2 RGB, 3 palette, 4 gray + alpha, 6 RGBA
            uint32_t channels = 0;  // samples a pixel
            bool interlaced = false;
            // Color type 3: each index's RGBA, from PLTE and tRNS. An index
            // past the palette's end is black, one past tRNS's end opaque,
            // as libpng and Pillow have them.
            std::array<uint8_t, 256 * 4> palette{};
            // Color types 0 and 2: tRNS's key, the gray or the red, green and
            // blue that are see-through, as stored.
            bool keyed = false;
            uint32_t key[3] = {};
        };

        // The bit depths each color type may have.
        bool validDepth(uint32_t color, uint32_t depth) {
            switch (color) {
                case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
                case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
                case 2:
                case 4:
                case 6: return depth == 8 || depth == 16;
                default: return false;
            }
        }

        uint32_t channelsOf(uint32_t color) {
            switch (color) {
                case 2: return 3;
                case 4: return 2;
                case 6: return 4;
                default: return 1;
            }
        }

        // The bytes in a row of `pixels` pixels, not counting its filter's.
        size_t rowBytes(Format const& f, uint32_t pixels) {
            return (static_cast<size_t>(pixels) * f.channels * f.depth + 7) / 8;
        }

        // Adam7's seven passes: each one's first pixel and the steps between
        // its pixels. A picture that isn't interlaced is one pass of it all.
        struct Pass {
            uint32_t x0, y0, dx, dy;
        };
        constexpr Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                    {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
        constexpr Pass kWhole[1] = {{0, 0, 1, 1}};

        // How many of a pass's columns (or rows) a picture `size` across has:
        // none when the pass starts past its end, and then the pass isn't
        // stored at all, not even its rows' filter bytes.
        uint32_t span(uint32_t size, uint32_t start, uint32_t step) {
            return size > start ? (size - start + step - 1) / step : 0;
        }

        // Sample `i` of an unfiltered row, as stored. Samples under 8 bits
        // are packed from each byte's high bits; 16-bit ones are big-endian.
        template <uint32_t Depth>
        uint32_t sample(uint8_t const* row, size_t i) {
            if constexpr (Depth == 8) {
                return row[i];
            } else if constexpr (Depth == 16) {
                return be16(row + i * 2);
            } else {
                size_t const bit = i * Depth;
                return (row[bit >> 3] >> (8 - Depth - (bit & 7))) & ((1u << Depth) - 1);
            }
        }

        // A sample at 8 bits: a 16-bit one's high byte, a smaller one
        // stretched over 0-255 (x255 for 1 bit, x85 for 2, x17 for 4).
        template <uint32_t Depth>
        uint8_t to8(uint32_t v) {
            if constexpr (Depth == 16)
                return static_cast<uint8_t>(v >> 8);
            else
                return static_cast<uint8_t>(v * (255 / ((1u << Depth) - 1)));
        }

        // One unfiltered row of `count` pixels, as RGBA into `dst`, each
        // pixel `step` pixels on from the last (a pass's spacing).
        template <uint32_t Depth>
        void expand(Format const& f, uint8_t const* row, uint32_t count, uint8_t* dst, size_t step) {
            size_t const advance = step * 4;
            switch (f.color) {
                case 0:
                    for (uint32_t x = 0; x < count; ++x, dst += advance) {
                        uint32_t const v = sample<Depth>(row, x);
                        dst[0] = dst[1] = dst[2] = to8<Depth>(v);
                        dst[3] = f.keyed && v == f.key[0] ? 0 : 255;
                    }
                    break;
                case 2:
                    for (uint32_t x = 0; x < count; ++x, dst += advance) {
                        uint32_t const r = sample<Depth>(row, size_t(x) * 3);
                        uint32_t const g = sample<Depth>(row, size_t(x) * 3 + 1);
                        uint32_t const b = sample<Depth>(row, size_t(x) * 3 + 2);
                        dst[0] = to8<Depth>(r);
                        dst[1] = to8<Depth>(g);
                        dst[2] = to8<Depth>(b);
                        dst[3] = f.keyed && r == f.key[0] && g == f.key[1] && b == f.key[2] ? 0 : 255;
                    }
                    break;
                case 3:
                    // (Indexes are 8 bits at most: the mask only keeps the
                    // 16-bit copy of this, which never runs, in the table.)
                    for (uint32_t x = 0; x < count; ++x, dst += advance)
                        std::memcpy(dst, &f.palette[(sample<Depth>(row, x) & 0xFF) * 4], 4);
                    break;
                case 4:
                    for (uint32_t x = 0; x < count; ++x, dst += advance) {
                        dst[0] = dst[1] = dst[2] = to8<Depth>(sample<Depth>(row, size_t(x) * 2));
                        dst[3] = to8<Depth>(sample<Depth>(row, size_t(x) * 2 + 1));
                    }
                    break;
                default:  // 6, RGBA
                    for (uint32_t x = 0; x < count; ++x, dst += advance)
                        for (size_t c = 0; c < 4; ++c)
                            dst[c] = to8<Depth>(sample<Depth>(row, size_t(x) * 4 + c));
                    break;
            }
        }

        void expandRow(Format const& f, uint8_t const* row, uint32_t count, uint8_t* dst, size_t step) {
            switch (f.depth) {
                case 1: expand<1>(f, row, count, dst, step); break;
                case 2: expand<2>(f, row, count, dst, step); break;
                case 4: expand<4>(f, row, count, dst, step); break;
                case 8: expand<8>(f, row, count, dst, step); break;
                default: expand<16>(f, row, count, dst, step); break;
            }
        }

        // Inflates the IDAT stream into all of `out`: null when it did, or
        // why not. More data than the picture needs is let go, as libpng
        // lets it go.
        char const* inflateAll(std::vector<uint8_t> const& packed, std::vector<uint8_t>& out) {
            z_stream zs{};
            if (inflateInit(&zs) != Z_OK)
                return "zlib wouldn't start";
            zs.next_in = const_cast<Bytef*>(packed.data());
            zs.avail_in = static_cast<uInt>(packed.size());
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            int const result = inflate(&zs, Z_FINISH);
            bool const full = zs.avail_out == 0;
            inflateEnd(&zs);
            if (full)
                return nullptr;
            if (result == Z_DATA_ERROR || result == Z_NEED_DICT || result == Z_MEM_ERROR)
                return "its picture data is damaged";
            return "its picture data stops short";
        }

    } // namespace

    bool decode(uint8_t const* data, size_t size, Pixels& out, std::string& error) {
        static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        if (!data || size < 8 || std::memcmp(data, kSignature, 8) != 0) {
            error = "not a PNG";
            return false;
        }
        Format f;
        bool header = false, havePalette = false;
        uint32_t paletteSize = 0;
        uint8_t paletteRgb[256 * 3] = {};
        uint8_t paletteAlpha[256];
        std::memset(paletteAlpha, 255, sizeof(paletteAlpha));
        std::vector<uint8_t> packed;

        // Chunks: length, type, data, CRC. Only the four the pixels need are
        // read (and their CRCs checked); text, gamma, color profiles and the
        // rest are passed over.
        for (size_t at = 8; at + 12 <= size;) {
            uint32_t const length = be32(data + at);
            if (length > size - at - 12) {
                error = "a chunk runs past the end of the file";
                return false;
            }
            uint8_t const* type = data + at + 4;
            uint8_t const* body = data + at + 8;
            at += 12 + static_cast<size_t>(length);
            if (is(type, "IEND"))
                break;
            if (is(type, "CgBI")) {
                error = "an Apple CgBI PNG (Xcode's iPhone format), which isn't a standard PNG";
                return false;
            }
            if (!is(type, "IHDR") && !is(type, "PLTE") && !is(type, "tRNS") && !is(type, "IDAT"))
                continue;
            if (static_cast<uint32_t>(::crc32(::crc32(0L, Z_NULL, 0), type, length + 4)) != be32(body + length)) {
                error = "a damaged " + std::string(reinterpret_cast<char const*>(type), 4) + " chunk";
                return false;
            }

            if (is(type, "IHDR")) {
                if (header) {
                    error = "two headers";
                    return false;
                }
                if (length != 13) {
                    error = "a header of the wrong size";
                    return false;
                }
                f.width = be32(body);
                f.height = be32(body + 4);
                f.depth = body[8];
                f.color = body[9];
                if (!validDepth(f.color, f.depth)) {
                    error = "color type " + std::to_string(f.color) + " at " + std::to_string(f.depth) +
                            " bits, which PNG doesn't have";
                    return false;
                }
                if (body[10] != 0 || body[11] != 0) {
                    error = "a compression or filter method PNG doesn't have";
                    return false;
                }
                if (body[12] > 1) {
                    error = "an interlace method PNG doesn't have";
                    return false;
                }
                if (f.width == 0 || f.height == 0 || f.width > 8192 || f.height > 8192) {
                    error = "an unlikely size";
                    return false;
                }
                f.channels = channelsOf(f.color);
                f.interlaced = body[12] == 1;
                header = true;
                continue;
            }
            if (!header) {
                error = "a chunk before its header";
                return false;
            }
            if (is(type, "PLTE")) {
                // RGB pictures may suggest a palette and gray ones may not
                // have one; only palette pictures use it.
                if (f.color != 3)
                    continue;
                if (length == 0 || length % 3 != 0 || length > 256 * 3) {
                    error = "a palette of the wrong size";
                    return false;
                }
                paletteSize = length / 3;
                std::memcpy(paletteRgb, body, length);
                havePalette = true;
            } else if (is(type, "tRNS")) {
                // Gray with alpha and RGBA can't have one: their alpha says it.
                if (f.color == 3) {
                    std::memcpy(paletteAlpha, body, std::min<size_t>(length, 256));
                } else if (f.color == 0 && length >= 2) {
                    f.key[0] = be16(body);
                    f.keyed = true;
                } else if (f.color == 2 && length >= 6) {
                    f.key[0] = be16(body);
                    f.key[1] = be16(body + 2);
                    f.key[2] = be16(body + 4);
                    f.keyed = true;
                }
            } else {
                packed.insert(packed.end(), body, body + length);
            }
        }
        if (!header || packed.empty()) {
            error = "no picture in it";
            return false;
        }
        if (f.color == 3) {
            if (!havePalette) {
                error = "palette colors without a palette";
                return false;
            }
            for (uint32_t i = 0; i < 256; ++i) {
                bool const listed = i < paletteSize;
                f.palette[i * 4 + 0] = listed ? paletteRgb[i * 3 + 0] : 0;
                f.palette[i * 4 + 1] = listed ? paletteRgb[i * 3 + 1] : 0;
                f.palette[i * 4 + 2] = listed ? paletteRgb[i * 3 + 2] : 0;
                f.palette[i * 4 + 3] = paletteAlpha[i];
            }
        }
        // A key below 16 bits is in the low bits of its two bytes; the high
        // ones are meant to be 0, and are let go as libpng lets them go.
        if (f.keyed && f.depth < 16)
            for (uint32_t& k : f.key)
                k &= (1u << f.depth) - 1;

        // Each pass's rows, each led by its filter's byte, one after another.
        Pass const* const passes = f.interlaced ? kAdam7 : kWhole;
        int const passCount = f.interlaced ? 7 : 1;
        size_t total = 0;
        for (int p = 0; p < passCount; ++p) {
            uint32_t const w = span(f.width, passes[p].x0, passes[p].dx);
            uint32_t const h = span(f.height, passes[p].y0, passes[p].dy);
            if (w && h)
                total += (rowBytes(f, w) + 1) * h;
        }
        std::vector<uint8_t> raw(total);
        if (char const* why = inflateAll(packed, raw)) {
            error = why;
            return false;
        }

        Pixels px;
        px.width = static_cast<int>(f.width);
        px.height = static_cast<int>(f.height);
        px.rgba.assign(static_cast<size_t>(f.width) * f.height * 4, 0);
        size_t const bpp = std::max<size_t>(1, f.channels * f.depth / 8);
        std::vector<uint8_t> const zeros(rowBytes(f, f.width), 0);
        uint8_t* row = raw.data();
        for (int p = 0; p < passCount; ++p) {
            Pass const& pass = passes[p];
            uint32_t const w = span(f.width, pass.x0, pass.dx);
            uint32_t const h = span(f.height, pass.y0, pass.dy);
            if (!w || !h)
                continue;
            size_t const stride = rowBytes(f, w);
            uint8_t const* up = zeros.data();
            for (uint32_t y = 0; y < h; ++y, row += stride + 1) {
                if (!unfilter(row[0], row + 1, up, stride, bpp)) {
                    error = "a row filtered in a way PNG doesn't have";
                    return false;
                }
                size_t const at = (static_cast<size_t>(pass.y0 + y * pass.dy) * f.width + pass.x0) * 4;
                expandRow(f, row + 1, w, px.rgba.data() + at, pass.dx);
                up = row + 1;
            }
        }
        out = std::move(px);
        return true;
    }

} // namespace gucci::ui::png
