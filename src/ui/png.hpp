#pragma once

// A small PNG reader for the menu's art (ui/art.hpp): our own rather than
// cocos's CCImage, which premultiplies the alpha (ImGui blends straight alpha:
// soft edges and glows would come out dark). No GL and no Geode, so the
// offline tests read the same pictures (tools/run_art_tests.cpp).
//
// It reads any standard PNG, because what ships isn't what tools/jupiter_art.py
// wrote: Geode's packager re-saves the pictures it puts in the .geode, and a
// picture with few colors comes out as a palette of 4-bit indexes with a tRNS
// chunk (build 2026-11-02-a's art never loaded for exactly that).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gucci::ui::png {

    // A decoded picture: RGBA, 8 bits a channel, straight alpha, top row first.
    struct Pixels {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };

    // Decodes a PNG of any color type (gray, RGB, palette, gray with alpha,
    // RGBA) and bit depth (1, 2, 4, 8, 16), interlaced (Adam7) or not, with
    // tRNS transparency, into Pixels. 16-bit samples keep their high byte, as
    // libpng's strip and Pillow do; grays under 8 bits stretch to the full
    // range (a 2-bit 1 is 85). A tRNS color key is matched against the
    // samples as stored, all 16 bits of them. Gamma and color profiles are
    // left alone. False, with why, for anything that isn't a whole PNG.
    bool decode(uint8_t const* data, size_t size, Pixels& out, std::string& error);

} // namespace gucci::ui::png
