#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>

#define GBR6_MAGIC "GBR6"
#define GBR6_VERSION 1
#define GBR6_HEADER_BASE 36

namespace gucci {

    enum GBR6Flags : uint8_t {
        GBR6_TWO_PLAYER = 1 << 0,
        GBR6_FROM_START_POS = 1 << 1,
        GBR6_RNG_LOCKED = 1 << 2,
        GBR6_PLATFORMER = 1 << 3,
        GBR6_HAS_LEVELNAME = 1 << 4,
        GBR6_HAS_DEATHS = 1 << 5,
        // Sub-tick offsets (anticroom's SCBF), appended after the deaths.
        // Readers from before this flag stop at the deaths and ignore the rest,
        // so an older GucciBot still loads the macro -- with every input on the
        // tick edge.
        GBR6_HAS_SUBTICK = 1 << 6,
    };

    struct GBR6Death {
        uint32_t frame = 0;
        uint8_t type = 0;
    };

    // Keyed by the input itself rather than by position in a stream, so it
    // does not depend on how the stream encoder groups taps or autoclicks.
    // (frame, button, pressed, player2) is unique: the action list already
    // refuses an exact duplicate on the same frame.
    struct GBR6Subtick {
        uint32_t frame = 0;
        uint8_t button = 1;
        bool pressed = true;
        bool player2 = false;
        double offset = 0.0;
    };

    struct GBR6Input {
        uint32_t frame = 0;
        uint8_t button = 1;
        bool pressed = true;
        bool player2 = false;

        bool isAutoclicker = false;
        uint8_t acHoldTicks = 0;
        uint8_t acRelTicks = 0;
        uint32_t acCycles = 0;
    };

    class GBR6Encoder {
    public:
        static std::vector<uint8_t> encode(const std::vector<GBR6Input>& inputs,
                                           uint8_t tap_threshold = 127);
    };

    class GBR6Decoder {
    public:
        static std::vector<GBR6Input>
        decode(const uint8_t* data, size_t size, bool is_player2 = false);
    };

    struct GBR6Header {
        uint8_t version = GBR6_VERSION;
        uint8_t flags = 0;
        float tps = 240.0f;
        int64_t timestamp = 0;
        uint64_t rngSeed = 0;
        uint32_t p1Count = 0;
        uint32_t p2Count = 0;
        std::string name;
        std::string levelName;

        bool twoPlayer() const {
            return flags & GBR6_TWO_PLAYER;
        }
        bool fromStartPos() const {
            return flags & GBR6_FROM_START_POS;
        }
        bool rngLocked() const {
            return flags & GBR6_RNG_LOCKED;
        }
        bool platformer() const {
            return flags & GBR6_PLATFORMER;
        }
    };

    class GBR6File {
    public:
        GBR6Header header;
        std::vector<uint8_t> p1Raw;
        std::vector<uint8_t> p2Raw;

        std::vector<GBR6Input> p1Inputs;
        std::vector<GBR6Input> p2Inputs;

        std::vector<GBR6Death> deaths;
        std::vector<GBR6Subtick> subticks;

        void decode();

        std::vector<uint8_t> serialize() const;

        static std::optional<GBR6File> deserialize(const uint8_t* data, size_t size);

        bool saveToPath(const std::filesystem::path& path) const;
        static std::optional<GBR6File> loadFromPath(const std::filesystem::path& path);

        static GBR6File
        fromInputs(GBR6Header hdr, std::vector<GBR6Input> p1, std::vector<GBR6Input> p2 = {});

        static GBR6File
        fromLegacy(const std::string& name,
                   float tps,
                   const std::vector<std::pair<uint32_t, bool>>& p1FramePressed,
                   const std::vector<std::pair<uint32_t, bool>>& p2FramePressed = {});
    };

} // namespace gucci
