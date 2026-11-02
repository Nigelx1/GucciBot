// The JupiterBot / JMF Trainer art and the PNG reader that loads it
// (src/ui/png.cpp). Run from the repo root (run_tests.bat).
//
// 1. The art: the pictures in resources/jupiter (as tools/jupiter_art.py
//    writes them) and in tools/testdata/png/packaged (the same art as Geode's
//    packager stores it in the .geode: palettes of 4-bit indexes with tRNS,
//    which is what the game reads) decode at the sizes their table
//    (src/ui/pages/jupiter_art.hpp) says, the sky is opaque, and every part
//    the tables name has art in it.
// 2. The reader against Pillow: every picture tools/make_png_tests.py made
//    (tools/testdata/png/manifest.txt: every color type, bit depth and
//    interlace, with and without tRNS) decodes to what Pillow decodes it to,
//    pixel for pixel, and every broken file is refused for the right reason.
// 3. SolarBot's art: the pictures in resources/solar (as tools/solar_art.py
//    writes them) and in tools/testdata/png/packaged (as the packager stores
//    them) decode at the sizes their table (src/ui/pages/solar_art.hpp) says,
//    each of Janet's is the size of its box, the sky is opaque, and every part
//    the tables name has art in it and is on the sheet.

#include "ui/pages/jupiter_art.hpp"
#include "ui/pages/solar_art.hpp"
#include "ui/png.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace ja = gucci::ui::pages::jupiter_art;
namespace sa = gucci::ui::pages::solar_art;
namespace png = gucci::ui::png;

namespace {

    char const* const kData = "tools/testdata/png/";

    int g_failed = 0;

    void fail(std::string const& what) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failed;
    }

    std::vector<uint8_t> readFile(std::string const& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    uint64_t fnv1a64(std::vector<uint8_t> const& bytes) {
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint8_t b : bytes)
            h = (h ^ b) * 0x100000001B3ull;
        return h;
    }

    template <class File>
    bool load(std::string const& dir, File const& file, png::Pixels& px) {
        std::string const path = dir + file.name;
        std::vector<uint8_t> const bytes = readFile(path);
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

    // Something in the rect at least `least` opaque (a glow never is opaque).
    void hasArt(png::Pixels const& px, float u0, float v0, float u1, float v1, std::string const& what, int least = 250) {
        int lo = 0, hi = 0;
        alphaRange(px, u0, v0, u1, v1, lo, hi);
        if (hi < least)
            fail(what + ": nothing drawn in its rect");
    }

    // The table checks on one copy of the art.
    void checkArt(std::string const& dir) {
        png::Pixels sky, dial, cw, ccw, city, sprites;
        if (load(dir, ja::kSky, sky)) {
            int lo = 0, hi = 0;
            alphaRange(sky, 0, 0, (float)sky.width, (float)sky.height, lo, hi);
            if (lo != 255)
                fail(dir + ": the sky has see-through pixels");
        }
        if (load(dir, ja::kDial, dial))
            hasArt(dial, 0, 0, (float)dial.width, (float)dial.height, dir + ": the dial");
        if (load(dir, ja::kDialCw, cw))
            hasArt(cw, 0, 0, (float)cw.width, (float)cw.height, dir + ": the dial's clockwise rings");
        if (load(dir, ja::kDialCcw, ccw))
            hasArt(ccw, 0, 0, (float)ccw.width, (float)ccw.height, dir + ": the dial's other rings");
        if (load(dir, ja::kCity, city)) {
            ja::Part const parts[] = {ja::kCityLeft, ja::kCityRight, ja::kCityMiddle};
            char const* const names[] = {"the skyline's left piece", "its right piece", "its middle strip"};
            for (int i = 0; i < 3; ++i) {
                auto const& p = parts[i];
                hasArt(city, p.u0, p.v0, p.u1, p.v1, dir + ": " + names[i]);
                if ((p.u1 - p.u0) / ja::kPx != p.w || (p.v1 - p.v0) / ja::kPx != p.h)
                    fail(std::string(names[i]) + ": its rect in the file isn't its size in the menu");
            }
        }
        if (load(dir, ja::kSprites, sprites)) {
            int i = 0;
            for (auto const& s : ja::kSpriteRects) {
                hasArt(sprites, s.u0, s.v0, s.u1, s.v1, dir + ": sprite " + std::to_string(i));
                if (s.u1 > sprites.width || s.v1 > sprites.height)
                    fail("sprite " + std::to_string(i) + " runs off the sheet");
                ++i;
            }
            for (auto const& star : ja::kStars)
                if (star.sprite < 0 || star.sprite >= (int)std::size(ja::kSpriteRects))
                    fail("a star names a sprite that isn't there");
        }
    }

    // SolarBot's art on one copy of it.
    void checkSolar(std::string const& dir) {
        png::Pixels sky, glow, disc, flames, top, sprites;
        if (load(dir, sa::kSky, sky)) {
            int lo = 0, hi = 0;
            alphaRange(sky, 0, 0, (float)sky.width, (float)sky.height, lo, hi);
            if (lo != 255)
                fail(dir + ": the sky has see-through pixels");
            if (sky.width != sa::kDesignW * sa::kPx || sky.height != sa::kDesignH * sa::kPx)
                fail(dir + ": the sky isn't the first-size menu");
        }
        // Each of Janet's pictures covers its box round her centre.
        // Her glow and her flames are see-through all over (at most 69% and
        // 73%).
        struct Hers {
            sa::File file;
            float const* box;
            float px;
            png::Pixels* pixels;
            char const* what;
            int least;
        };
        Hers const hers[] = {{sa::kGlow, sa::kGlowBox, sa::kGlowPx, &glow, "her glow", 150},
                             {sa::kDisc, sa::kDiscBox, sa::kPx, &disc, "her disc", 250},
                             {sa::kFlames, sa::kFlameBox, sa::kPx, &flames, "her flames", 150},
                             {sa::kTop, sa::kTopBox, sa::kPx, &top, "her limb and face", 250}};
        for (auto const& her : hers) {
            if (!load(dir, her.file, *her.pixels))
                continue;
            hasArt(*her.pixels, 0, 0, (float)her.pixels->width, (float)her.pixels->height, dir + ": " + her.what,
                   her.least);
            if ((her.box[2] - her.box[0]) * her.px != her.file.w || (her.box[3] - her.box[1]) * her.px != her.file.h)
                fail(std::string(her.what) + ": the picture isn't the size of its box");
        }
        if (sa::kFlameBox[3] - sa::kFlameBox[1] != 2.f * sa::kJanetR)
            fail("her flames' tile isn't 2R tall, which its wrap needs");
        if (load(dir, sa::kSprites, sprites)) {
            int const count = (int)std::size(sa::kSpriteRects);
            int i = 0;
            for (auto const& s : sa::kSpriteRects) {
                // The kite's glint is a 32% glow.
                hasArt(sprites, s.u0, s.v0, s.u1, s.v1, dir + ": SolarBot sprite " + std::to_string(i),
                       i == sa::kKiteGlint ? 60 : 250);
                if (s.u1 > sprites.width || s.v1 > sprites.height)
                    fail("SolarBot sprite " + std::to_string(i) + " runs off the sheet");
                ++i;
            }
            std::vector<int> named = {sa::kSparkleSprite, sa::kBokehSprite,  sa::kTwinkleSprite, sa::kMedallionBody,
                                      sa::kMedallionOuter, sa::kMedallionInner, sa::kKiteBody,     sa::kKiteGlint,
                                      sa::kCoronaTop,     sa::kCoronaBottom,   sa::kMedallion.sprite, sa::kKite.sprite};
            for (auto const& p : sa::kPlanets)
                named.push_back(p.sprite);
            for (auto const& c : sa::kConstellations)
                named.push_back(c.sprite);
            for (int n : named)
                if (n < 0 || n >= count)
                    fail("a SolarBot table names a sprite that isn't there");
            for (int band : {sa::kCoronaTop, sa::kCoronaBottom}) {
                auto const& s = sa::kSpriteRects[band];
                if ((s.u1 - s.u0) / sa::kPx != sa::kCoronaPeriod || (s.v1 - s.v0) / sa::kPx != sa::kCoronaHeight)
                    fail("a corona band's tile isn't one period wide and the band's height");
                int lo = 0, hi = 0;
                alphaRange(sprites, s.u0, s.v0, s.u1, s.v1, lo, hi);
                // Its black edge reaches the window's: opaque at that side.
                float const edgeV = band == sa::kCoronaTop ? s.v0 : s.v1 - 1.f;
                alphaRange(sprites, s.u0, edgeV, s.u1, edgeV + 1.f, lo, hi);
                if (lo != 255)
                    fail("a corona band's edge row isn't opaque");
            }
            for (auto const& sp : sa::kSparkles)
                if (sp.constellation < 0 || sp.constellation >= (int)std::size(sa::kConstellations))
                    fail("a sparkle names a constellation that isn't there");
        }
    }

    // One picture against Pillow's decode: its pixels in expected.rgba (from
    // `offset`), or only their hash for the big packaged ones. A packaged one
    // that differs is written to build\ for a closer look.
    void checkPicture(std::string const& path, int w, int h, std::string const& offset, uint64_t hash,
                      std::vector<uint8_t> const& expected) {
        std::vector<uint8_t> const bytes = readFile(kData + path);
        if (bytes.empty()) {
            fail(path + ": missing");
            return;
        }
        png::Pixels px;
        std::string error;
        if (!png::decode(bytes.data(), bytes.size(), px, error)) {
            fail(path + ": " + error);
            return;
        }
        if (px.width != w || px.height != h) {
            fail(path + ": " + std::to_string(px.width) + "x" + std::to_string(px.height) + ", Pillow's is " +
                 std::to_string(w) + "x" + std::to_string(h));
            return;
        }
        size_t const n = (size_t)w * h * 4;
        if (offset != "-") {
            size_t const at = std::stoull(offset);
            if (at + n > expected.size()) {
                fail(path + ": expected.rgba is too short for it (run tools/make_png_tests.py)");
                return;
            }
            size_t differ = 0, first = 0;
            for (size_t i = 0; i < n; i += 4) {
                if (std::equal(px.rgba.begin() + i, px.rgba.begin() + i + 4, expected.begin() + at + i))
                    continue;
                if (differ++ == 0)
                    first = i;
            }
            if (differ) {
                auto const rgba = [](uint8_t const* p) {
                    char text[32];
                    std::snprintf(text, sizeof(text), "(%d,%d,%d,%d)", p[0], p[1], p[2], p[3]);
                    return std::string(text);
                };
                size_t const pixel = first / 4;
                fail(path + ": " + std::to_string(differ) + " pixel(s) differ from Pillow's, first at (" +
                     std::to_string(pixel % w) + "," + std::to_string(pixel / w) + "): " +
                     rgba(px.rgba.data() + first) + ", Pillow " + rgba(expected.data() + at + first));
                return;
            }
        }
        if (fnv1a64(px.rgba) != hash) {
            std::string const dump = "build/png_mismatch_" + path.substr(path.find_last_of('/') + 1) + ".rgba";
            std::ofstream(dump, std::ios::binary).write(reinterpret_cast<char const*>(px.rgba.data()),
                                                        (std::streamsize)px.rgba.size());
            fail(path + ": differs from Pillow's decode (ours is in " + dump + ")");
        }
    }

} // namespace

int main() {
    checkArt("resources/jupiter/");
    checkArt(std::string(kData) + "packaged/");
    checkSolar("resources/solar/");
    checkSolar(std::string(kData) + "packaged/");

    // Every picture in the manifest.
    std::vector<uint8_t> const expected = readFile(std::string(kData) + "expected.rgba");
    std::ifstream manifest(std::string(kData) + "manifest.txt");
    if (!manifest || expected.empty())
        fail(std::string(kData) + ": no manifest.txt or expected.rgba (run tools/make_png_tests.py)");
    int pictures = 0, refused = 0;
    std::string line;
    while (std::getline(manifest, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        std::string first;
        in >> first;
        if (first == "refuse") {
            std::string path, words;
            in >> path;
            std::getline(in >> std::ws, words);
            std::vector<uint8_t> const bytes = readFile(kData + path);
            png::Pixels px;
            std::string error;
            if (bytes.empty())
                fail(path + ": missing");
            else if (png::decode(bytes.data(), bytes.size(), px, error))
                fail(path + ": decoded, when it should be refused (" + words + ")");
            else if (error.find(words) == std::string::npos)
                fail(path + ": refused with \"" + error + "\", not for \"" + words + "\"");
            else
                ++refused;
            continue;
        }
        int w = 0, h = 0;
        std::string offset, hash;
        in >> w >> h >> offset >> hash;
        int const before = g_failed;
        checkPicture(first, w, h, offset, std::stoull(hash, nullptr, 16), expected);
        if (g_failed == before)
            ++pictures;
    }

    // Junk isn't a PNG.
    {
        png::Pixels px;
        std::string error;
        uint8_t const junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        if (png::decode(junk, sizeof(junk), px, error))
            fail("junk decoded as a PNG");
    }

    // What loading the art as packaged costs the frame it's first drawn in.
    {
        std::vector<std::vector<uint8_t>> files;
        for (ja::File const& f : {ja::kSky, ja::kDial, ja::kDialCw, ja::kDialCcw, ja::kCity, ja::kSprites})
            files.push_back(readFile(std::string(kData) + "packaged/" + f.name));
        auto const start = std::chrono::steady_clock::now();
        for (auto const& bytes : files) {
            png::Pixels px;
            std::string error;
            png::decode(bytes.data(), bytes.size(), px, error);
        }
        double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("art: the six layers as packaged decode in %.1f ms\n", ms);
    }
    {
        std::vector<std::vector<uint8_t>> files;
        for (sa::File const& f : {sa::kSky, sa::kGlow, sa::kDisc, sa::kFlames, sa::kTop, sa::kSprites})
            files.push_back(readFile(std::string(kData) + "packaged/" + f.name));
        auto const start = std::chrono::steady_clock::now();
        for (auto const& bytes : files) {
            png::Pixels px;
            std::string error;
            png::decode(bytes.data(), bytes.size(), px, error);
        }
        double const ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("art: SolarBot's six layers as packaged decode in %.1f ms\n", ms);
    }

    if (g_failed) {
        std::printf("%d art check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("art: all checks passed (%d pictures match Pillow pixel for pixel, %d broken files refused)\n",
                pictures, refused);
    return 0;
}
