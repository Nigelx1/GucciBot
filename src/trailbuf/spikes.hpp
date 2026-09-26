// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#ifndef TRAILBUF_SPIKES_HPP
#define TRAILBUF_SPIKES_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "generator.hpp"

class LevelEditorLayer;

namespace tbuf {

constexpr int MAX_CLICK_RADIUS = 240;
constexpr int MAX_EVERY_NTH = 60;
constexpr float MIN_SPIKE_GAP = 0.01f;
constexpr float MAX_SPIKE_GAP = 2.f;

struct SpikeSettings {
    int objectId = 8;
    float gap = 0.1f;
    std::array<bool, 4> sides{true, true, true, true};
    int everyNth = 1;
    bool aroundClicks = false;
    int clickRadius = 2;
};

struct SpikeReport {
    bool ok = false;
    int placed = 0;
    int tried = 0;
    int noRoom = 0;
    int removed = 0;
    std::string message;
};

SpikeReport placeSpikes(LevelEditorLayer* editor,
                        std::vector<Sample> const& chosen,
                        std::vector<Sample> const& other,
                        std::vector<uint32_t> const& clickFrames,
                        SpikeSettings const& settings);

}  // namespace tbuf

#endif  // TRAILBUF_SPIKES_HPP
