#pragma once

// Default Look (Nigel's ask, 2026-09-13, in 1.7; rebuilt for 2.0's overlay on
// 2026-10-27): one switch that makes the frame-window overlay look like the
// counter in NaN GD's videos. It's a fixed set of seven bands the overlay draws
// with instead of the player's own:
//  - a plain ring per input in NaN's color ramp (GucciEngine::fwDefaultLookColor),
//    with the number on its left (2.0 already puts it there);
//  - the 9-10 / 7-8 / 5-6 / 4 / 3 / 2 / 1 legend (fwDefaultLookRows);
//  - the Brrr, or with Bells on, each row's own bell (fw_1.wav ... fw_9_12.wav,
//    bundled in resources).
// The player's bands are never written, and L* and Copy info keep using them.
// A window over 10 has no row, so it draws nothing, like the default bands.

#include "analysis/ac/shim.hpp"
#include "core/GucciBot.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace gucci::defaultlook {

    inline bool on() {
        return GucciEngine::get()->fwDefaultLook;
    }

    // The bell for one legend row: fw_1.wav, ..., fw_5_6.wav, fw_7_8.wav, fw_9_12.wav.
    inline std::string bellFor(int lo, int hi) {
        if (lo >= 9)
            return "fw_9_12.wav";
        if (lo == hi)
            return "fw_" + std::to_string(lo) + ".wav";
        return "fw_" + std::to_string(lo) + "_" + std::to_string(hi) + ".wav";
    }

    // The seven bands, coarsest first like NaN's legend. Built once per Bells
    // setting; the ids start at 1001 so they never meet the player's.
    inline std::vector<::FrameWindowTier> const& bands() {
        static std::vector<::FrameWindowTier> cache[2];
        bool const bells = GucciEngine::get()->fwDefaultLookBells;
        auto& out = cache[bells ? 1 : 0];
        if (out.empty()) {
            auto const& rows = GucciEngine::fwDefaultLookRows();
            for (size_t i = 0; i < rows.size(); i++) {
                ::FrameWindowTier t;
                t.id = 1001 + static_cast<int>(i);
                t.minWindow = static_cast<float>(rows[i].lo);
                t.maxWindow = static_cast<float>(rows[i].hi);
                auto const c = GucciEngine::fwDefaultLookColor(rows[i].lo);
                t.color = {c.r, c.g, c.b, c.a};
                t.audioPath = bells ? bellFor(rows[i].lo, rows[i].hi) : std::string("fw_default.mp3");
                t.showInHud = true;
                out.push_back(t);
            }
        }
        return out;
    }

    // The band a whole window lands in. A 0 (nothing survives) counts in the
    // red 1 row, as the default 0-1 band does; past 10 there's none.
    inline ::FrameWindowTier const* bandFor(int window) {
        int const w = std::max(window, 1);
        for (auto const& t : bands()) {
            if (static_cast<float>(w) >= t.minWindow && static_cast<float>(w) <= t.maxWindow)
                return &t;
        }
        return nullptr;
    }

} // namespace gucci::defaultlook
