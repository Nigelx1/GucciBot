// The PNG reader for the menu's art (ui/png.hpp).

#include "ui/png.hpp"

#include <zlib.h>

#include <cstdlib>
#include <cstring>

namespace gucci::ui::png {

    namespace {

        uint32_t be32(uint8_t const* p) {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        // PNG's Paeth predictor: whichever of left, up and up-left is
        // nearest left + up - up-left.
        uint8_t paeth(int a, int b, int c) {
            int const p = a + b - c;
            int const pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }

        // Puts back a row the encoder filtered: `cur` in place, `up` the row
        // above as it was before filtering (zeros for the first), `bpp` bytes
        // a pixel.
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

    } // namespace

    bool decode(uint8_t const* data, size_t size, Pixels& out, std::string& error) {
        static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        if (!data || size < 8 || std::memcmp(data, kSignature, 8) != 0) {
            error = "not a PNG";
            return false;
        }
        uint32_t width = 0, height = 0;
        size_t channels = 0;
        std::vector<uint8_t> packed;
        // Chunks: length, type, data, CRC.
        for (size_t at = 8; at + 12 <= size;) {
            uint32_t const length = be32(data + at);
            if (length > size - at - 12) {
                error = "a chunk runs past the end of the file";
                return false;
            }
            uint8_t const* type = data + at + 4;
            uint8_t const* body = data + at + 8;
            if (std::memcmp(type, "IHDR", 4) == 0) {
                if (length < 13) {
                    error = "a short header";
                    return false;
                }
                width = be32(body);
                height = be32(body + 4);
                uint8_t const depth = body[8], color = body[9];
                if (depth != 8 || (color != 2 && color != 6) || body[10] != 0 || body[11] != 0 || body[12] != 0) {
                    error = "not 8-bit RGB or RGBA without interlacing";
                    return false;
                }
                if (width == 0 || height == 0 || width > 8192 || height > 8192) {
                    error = "an unlikely size";
                    return false;
                }
                channels = color == 6 ? 4 : 3;
            } else if (std::memcmp(type, "IDAT", 4) == 0) {
                packed.insert(packed.end(), body, body + length);
            } else if (std::memcmp(type, "IEND", 4) == 0) {
                break;
            }
            at += 12 + static_cast<size_t>(length);
        }
        if (channels == 0 || packed.empty()) {
            error = "no picture in it";
            return false;
        }

        // Each row is led by its filter's byte.
        size_t const stride = static_cast<size_t>(width) * channels;
        std::vector<uint8_t> raw((stride + 1) * height);
        uLongf rawSize = static_cast<uLongf>(raw.size());
        if (uncompress(raw.data(), &rawSize, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
            rawSize != raw.size()) {
            error = "its picture data didn't unpack";
            return false;
        }
        out.width = static_cast<int>(width);
        out.height = static_cast<int>(height);
        out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
        std::vector<uint8_t> const zeros(stride, 0);
        for (size_t y = 0; y < height; ++y) {
            uint8_t* row = raw.data() + y * (stride + 1);
            uint8_t const* up = y ? raw.data() + (y - 1) * (stride + 1) + 1 : zeros.data();
            if (!unfilter(row[0], row + 1, up, stride, channels)) {
                error = "a row filtered in a way PNG doesn't have";
                return false;
            }
            uint8_t* dst = out.rgba.data() + y * width * 4;
            if (channels == 4) {
                std::memcpy(dst, row + 1, stride);
            } else {
                for (size_t x = 0; x < width; ++x) {
                    dst[x * 4 + 0] = row[1 + x * 3 + 0];
                    dst[x * 4 + 1] = row[1 + x * 3 + 1];
                    dst[x * 4 + 2] = row[1 + x * 3 + 2];
                    dst[x * 4 + 3] = 255;
                }
            }
        }
        return true;
    }

} // namespace gucci::ui::png
