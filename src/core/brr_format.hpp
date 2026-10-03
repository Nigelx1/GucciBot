#pragma once

// The legacy BRR macro format. 2026-10-01: its reader/writer came from
// ToastyReplay, whose author withdrew permission, and was removed. GucciBot
// saves GBR6 and keeps doing so; old BRR files are not read until a new
// reader is written. These types remain so the engine's legacy paths compile
// and simply find nothing.

#include <cstdint>
#include <string>
#include <vector>

namespace gucci {

    inline constexpr int BRR_FORMAT_VERSION = 4;

    struct BRRInput {
        int32_t tick = 0;
        uint8_t actionType = 0;
        uint8_t flags = 0;
        float stepOffset = 0.0f;

        bool isPlayer2() const { return (flags & 0x01) != 0; }
        bool isPressed() const { return (flags & 0x02) != 0; }
        void setPlayer2(bool v) { flags = static_cast<uint8_t>(v ? (flags | 0x01) : (flags & ~0x01)); }
        void setPressed(bool v) { flags = static_cast<uint8_t>(v ? (flags | 0x02) : (flags & ~0x02)); }
    };

    struct BRRCheckpoint {
        int32_t tick = 0;
        uint64_t rngState = 0;
        int32_t priorTick = 0;
    };

    class BRRMacro {
    public:
        std::string name;
        std::string persistedName;
        std::string levelName;
        int32_t levelId = 0;
        double framerate = 240.0;
        std::vector<BRRInput> inputs;
        std::vector<BRRCheckpoint> checkpoints;
        std::vector<int32_t> deathFrames;
        std::vector<int32_t> anchors;
        std::vector<int32_t> attemptStartTicks;

        // No legacy reader for now: always nullptr.
        static BRRMacro* deserialize(std::vector<uint8_t> const&) { return nullptr; }
        static BRRMacro* loadFromDisk(std::string const&) { return nullptr; }
        // No legacy writer either.
        void persist() {}
    };

} // namespace gucci
