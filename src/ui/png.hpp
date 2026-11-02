#pragma once

// A small PNG reader for the menu's art (ui/art.hpp): our own rather than
// cocos's CCImage, which premultiplies the alpha (ImGui blends straight alpha:
// soft edges and glows would come out dark). No GL and no Geode, so the
// offline tests read the same pictures (tools/run_art_tests.cpp).

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

    // Decodes a PNG of 8-bit RGB or RGBA (colour types 2 and 6, not
    // interlaced: what tools/jupiter_art.py writes). False, with why, for
    // anything else.
    bool decode(uint8_t const* data, size_t size, Pixels& out, std::string& error);

} // namespace gucci::ui::png
