#pragma once

// The level tape (stage B, 2026-11-02; AI contract v2 section 5): the whole
// level as columns of half a block, the level reader's input. Per column:
// which height bands hold something deadly, which hold something solid (one
// byte, 0 or 1, per band), then how many of each orb, pad and portal type
// stand in it. Static objects only (v1): what the level's triggers move or
// toggle is left out. Objects as the level puts them (their own hitboxes,
// not grown by the player's: the tape does not depend on the player's size).

#include "ai/observe.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <matjson.hpp>

namespace gucci::ai::tape {

    constexpr float kColumnWidth = 15.0f;
    constexpr float kX0 = 0.0f;
    constexpr float kY0 = 90.0f;  // the ground line
    constexpr float kBandHeight = 15.0f;
    constexpr int kBands = 80;     // 40 blocks up from the ground
    constexpr int kTokenChannels = obs::kTokenCount - 1;  // types 1..31
    constexpr int kChannels = 2 * kBands + kTokenChannels;

    std::string layoutHash();
    // {tape_channels, tape_layout, tape_layout_hash} for gucci_ai_layout.
    matjson::Value layoutJson();

    struct Tape {
        int columns = 0;
        std::vector<uint8_t> data;  // [columns][kChannels]
    };
    Tape build(obs::LevelIndex const& index);

    // <dir>/<level_id>.json + <level_id>.tape.u8. False with why.
    bool write(std::filesystem::path const& dir, obs::LevelIndex const& index, std::string const& levelName,
               Tape const& tape, std::string& why);

} // namespace gucci::ai::tape
