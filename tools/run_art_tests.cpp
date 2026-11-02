// The JupiterBot / JMF Trainer art (resources/jupiter, made by
// tools/jupiter_art.py), read the way the mod reads it (src/ui/png.cpp): every
// picture decodes at the size its table (src/ui/pages/jupiter_art.hpp) says,
// the sky is opaque, and every part the tables name has art in it. Run from
// the repo root (run_tests.bat).

#include "ui/pages/jupiter_art.hpp"
#include "ui/png.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace ja = gucci::ui::pages::jupiter_art;
namespace png = gucci::ui::png;

namespace {

    int g_failed = 0;

    void fail(std::string const& what) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed;
    }

    bool load(ja::File const& file, png::Pixels& px) {
        std::string const path = std::string("resources/jupiter/") + file.name;
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> const bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        if (bytes.empty()) {
            fail(path + ": missing");
            return false;
        }
        if (!png::decode(bytes.data(), bytes.size(), px, error)) {
            fail(path + ": " + error);
            return false;
        }
        if (px.width != file.w || px.height != file.h)
            fail(path + ": " + std::to_string(px.width) + "x" + std::to_string(px.height) + ", its table says " +
                 std::to_string(file.w) + "x" + std::to_string(file.h));
        return true;
    }

    // The highest and lowest alpha in a rect of a picture (pixels).
    void alphaRange(png::Pixels const& px, float u0, float v0, float u1, float v1, int& lo, int& hi) {
        lo = 255;
        hi = 0;
        int const x0 = std::max(0, (int)u0), y0 = std::max(0, (int)v0);
        int const x1 = std::min(px.width, (int)u1), y1 = std::min(px.height, (int)v1);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                int const a = px.rgba[((size_t)y * px.width + x) * 4 + 3];
                lo = std::min(lo, a);
                hi = std::max(hi, a);
            }
    }

    void hasArt(png::Pixels const& px, float u0, float v0, float u1, float v1, std::string const& what) {
        int lo = 0, hi = 0;
        alphaRange(px, u0, v0, u1, v1, lo, hi);
        if (hi < 250)
            fail(what + ": nothing drawn in its rect");
    }

} // namespace

int main() {
    png::Pixels sky, dial, cw, ccw, city, sprites;
    if (load(ja::kSky, sky)) {
        int lo = 0, hi = 0;
        alphaRange(sky, 0, 0, (float)sky.width, (float)sky.height, lo, hi);
        if (lo != 255)
            fail("the sky has see-through pixels");
    }
    if (load(ja::kDial, dial))
        hasArt(dial, 0, 0, (float)dial.width, (float)dial.height, "the dial");
    if (load(ja::kDialCw, cw))
        hasArt(cw, 0, 0, (float)cw.width, (float)cw.height, "the dial's clockwise rings");
    if (load(ja::kDialCcw, ccw))
        hasArt(ccw, 0, 0, (float)ccw.width, (float)ccw.height, "the dial's other rings");
    if (load(ja::kCity, city)) {
        ja::Part const parts[] = {ja::kCityLeft, ja::kCityRight, ja::kCityMiddle};
        char const* const names[] = {"the skyline's left piece", "its right piece", "its middle strip"};
        for (int i = 0; i < 3; ++i) {
            auto const& p = parts[i];
            hasArt(city, p.u0, p.v0, p.u1, p.v1, names[i]);
            if ((p.u1 - p.u0) / ja::kPx != p.w || (p.v1 - p.v0) / ja::kPx != p.h)
                fail(std::string(names[i]) + ": its rect in the file isn't its size in the menu");
        }
    }
    if (load(ja::kSprites, sprites)) {
        int i = 0;
        for (auto const& s : ja::kSpriteRects) {
            hasArt(sprites, s.u0, s.v0, s.u1, s.v1, "sprite " + std::to_string(i));
            if (s.u1 > sprites.width || s.v1 > sprites.height)
                fail("sprite " + std::to_string(i) + " runs off the sheet");
            ++i;
        }
        for (auto const& star : ja::kStars)
            if (star.sprite < 0 || star.sprite >= (int)std::size(ja::kSpriteRects))
                fail("a star names a sprite that isn't there");
    }

    // Not a PNG, and a PNG cut short, are refused rather than read past.
    {
        png::Pixels px;
        std::string error;
        uint8_t const junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        if (png::decode(junk, sizeof(junk), px, error))
            fail("junk decoded as a PNG");
        std::ifstream in("resources/jupiter/jupiter_dial.png", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() > 200) {
            bytes.resize(bytes.size() / 2);
            if (png::decode(bytes.data(), bytes.size(), px, error))
                fail("half a PNG decoded");
        }
    }

    if (g_failed) {
        std::printf("%d art check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("art: all checks passed\n");
    return 0;
}
