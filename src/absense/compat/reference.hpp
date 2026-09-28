#pragma once

// Stands in for Absense's replay/reference.hpp: its reader for Silicate .slc
// macro files, which the pathfinder uses to find a human's run of the level
// by file name. GucciBot does not read .slc files (the site's converter does),
// so this never finds one, and human.cpp falls through to its other source --
// the macro loaded in GucciBot -- which is how GucciBot's own pathfinder
// already did it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace absense::reference {

    struct Event {
        uint64_t frame = 0;
        uint8_t type = 0;  // 1 jump, 2 left, 3 right, 4 restart, 5 restart full, 6 death, 7 tps, 8 bugpoint
        bool holding = false;
        bool player2 = false;
        double tps = 0.0;
    };

    struct Replay {
        double tps = 240.0;
        uint64_t seed = 0;
        std::string format;
        std::vector<Event> events;
        uint64_t lastFrame() const { return events.empty() ? 0 : events.back().frame; }
    };

    inline std::optional<Replay> load(std::filesystem::path const&) { return std::nullopt; }

    struct Button {
        bool held = false;
        int presses = 0;
    };

}  // namespace absense::reference
