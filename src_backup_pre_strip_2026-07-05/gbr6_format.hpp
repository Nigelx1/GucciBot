#pragma once
// gbr6_format.hpp — GucciBot 10.0
// Hypercompressed replay format.
// 2 bytes per input entry. Matches yBot file sizes for human gameplay.
// Wave/ship sections are 2x more efficient via tap encoding.
// Autoclicker patterns of any length store in ~10 bytes total.
//
// Entry layout (always 2 bytes):
//
//  Type A — Normal input  (b0 & 0x80 != 0)
//    b0 = 0x80 | (player2 << 4) | (button << 1) | pressed
//    b1 = delta frames since last entry (0x00-0xFE)
//    b1 = 0xFF means extended delta: add 255 to the NEXT entry's delta
//
//  Type B — Tap entry  (b0 != 0x00, b0 & 0x80 == 0)
//    b0 = hold duration in frames (1-127)
//    b1 = total gap in frames (1-255)
//    Decoded as: press at start, hold for b0 frames, release
//    Used for wave/ship where press+release pairs can be merged
//
//  Type C — Autoclicker run  (b0 == 0x00)
//    b1 encodes the autoclicker pattern:
//      bits 7-4 = holdTicks  (1-15 frames)
//      bits 3-0 = releaseTicks (1-15 frames)
//    The run ends at the next non-autoclicker entry or end of stream.
//    The frame counter advances by (holdTicks + releaseTicks) per cycle.
//    A run of N cycles stores as 1 entry + 1 count entry:
//      entry[i]   = Type C  (b0=0x00, b1=pattern)
//      entry[i+1] = Type A  (b0=0x00 special, b1 = high byte of cycle count)
//                 + entry[i+2] = b1 = low byte of cycle count
//    Actually simpler: one Type C entry followed by one uint16_t cycle count
//    packed as two bytes (big-endian count, always present after Type C)
//
// File layout:
//   [0]  magic:    "GBR6"  (4 bytes)
//   [4]  version:  uint8   = 1
//   [5]  flags:    uint8   (bit0=twoPlayer, bit1=fromStartPos, bit2=rngLocked, bit3=platformer)
//   [6]  tps:      float32 (little-endian)
//  [10]  timestamp: int64  (unix seconds, little-endian)
//  [18]  rng_seed:  uint64 (little-endian)
//  [26]  p1_count:  uint32 (number of P1 entries, little-endian)
//  [30]  p2_count:  uint32 (number of P2 entries, little-endian)
//  [34]  name_len:  uint16
//  [36]  name:      UTF-8 string (name_len bytes)
//  [36+name_len]  P1 entries (p1_count * 2 bytes each)
//  [...]           P2 entries (p2_count * 2 bytes each)
//
// Legacy .brrr files (magic "BRR") load via the old deserializer automatically.

#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>

#define GBR6_MAGIC "GBR6"
#define GBR6_VERSION 1
#define GBR6_HEADER_BASE 36  // bytes before the variable-length name

// ─────────────────────────────────────────────────────────────────────────────
// GBR6 flags
// ─────────────────────────────────────────────────────────────────────────────

enum GBR6Flags : uint8_t {
    GBR6_TWO_PLAYER     = 1 << 0,
    GBR6_FROM_START_POS = 1 << 1,
    GBR6_RNG_LOCKED     = 1 << 2,
    GBR6_PLATFORMER     = 1 << 3,
    GBR6_HAS_LEVELNAME  = 1 << 4,  // v8.5: header carries a levelName string
    GBR6_HAS_DEATHS     = 1 << 5,  // v10.3: trailing death/restart-marker block (P1 fix)
};

// v10.3 (P1 fix): an intentional death (or restart marker) recorded mid-macro.
// These are non-input actions (ActionType::Death/Restart/RestartFull) that can't
// ride through the 2-byte input encoder, so they're stored in a separate
// flag-gated trailing block and restored as Death actions on load. Without this,
// a recorded intentional death is silently dropped at save time and never plays
// back from a file.
struct GBR6Death {
    uint32_t frame = 0;   // absolute frame the death/marker occurs at
    uint8_t  type  = 0;   // ActionType value (Death=10, Restart=11, RestartFull=12)
};

// ─────────────────────────────────────────────────────────────────────────────
// Decoded input event (internal representation after decoding)
// ─────────────────────────────────────────────────────────────────────────────

struct GBR6Input {
    uint32_t frame   = 0;    // absolute frame number
    uint8_t  button  = 1;    // 1=jump, 2=left, 3=right
    bool     pressed = true;
    bool     player2 = false;

    // For autoclicker runs only
    bool     isAutoclicker = false;
    uint8_t  acHoldTicks   = 0;
    uint8_t  acRelTicks    = 0;
    uint32_t acCycles      = 0;  // how many hold+release cycles
};

// ─────────────────────────────────────────────────────────────────────────────
// Encoder: converts a list of GBR6Input → raw bytes
// ─────────────────────────────────────────────────────────────────────────────

class GBR6Encoder {
public:
    // Encode a sorted list of inputs for one player into a byte stream.
    // tap_threshold: if a press-release pair's hold duration <= this many frames
    //                AND the gap fits in a byte, encode as a tap entry.
    //                Set to 0 to disable tap encoding.
    static std::vector<uint8_t> encode(
        const std::vector<GBR6Input>& inputs,
        uint8_t tap_threshold = 127);
};

// ─────────────────────────────────────────────────────────────────────────────
// Decoder: converts raw bytes → list of GBR6Input
// ─────────────────────────────────────────────────────────────────────────────

class GBR6Decoder {
public:
    static std::vector<GBR6Input> decode(
        const uint8_t* data, size_t size,
        bool is_player2 = false);
};

// ─────────────────────────────────────────────────────────────────────────────
// GBR6File — full file read/write
// ─────────────────────────────────────────────────────────────────────────────

struct GBR6Header {
    uint8_t  version    = GBR6_VERSION;
    uint8_t  flags      = 0;
    float    tps        = 240.0f;
    int64_t  timestamp  = 0;
    uint64_t rngSeed    = 0;
    uint32_t p1Count    = 0;
    uint32_t p2Count    = 0;
    std::string name;
    std::string levelName;  // v8.5: populated on record from the current level

    bool twoPlayer()    const { return flags & GBR6_TWO_PLAYER; }
    bool fromStartPos() const { return flags & GBR6_FROM_START_POS; }
    bool rngLocked()    const { return flags & GBR6_RNG_LOCKED; }
    bool platformer()   const { return flags & GBR6_PLATFORMER; }
};

class GBR6File {
public:
    GBR6Header              header;
    std::vector<uint8_t>    p1Raw;   // raw encoded P1 bytes
    std::vector<uint8_t>    p2Raw;   // raw encoded P2 bytes

    // Decoded views (populated by decode())
    std::vector<GBR6Input>  p1Inputs;
    std::vector<GBR6Input>  p2Inputs;

    // v10.3 (P1 fix): intentional-death / restart markers. Populated by
    // deserialize() when GBR6_HAS_DEATHS is set; written by serialize() when
    // non-empty. Empty for ordinary (no-intentional-death) macros.
    std::vector<GBR6Death>  deaths;

    void decode();  // decode p1Raw/p2Raw into p1Inputs/p2Inputs

    // Serialize to bytes (for writing to disk)
    std::vector<uint8_t> serialize() const;

    // Deserialize from bytes
    static std::optional<GBR6File> deserialize(const uint8_t* data, size_t size);

    // Convenience I/O
    bool saveToPath(const std::filesystem::path& path) const;
    static std::optional<GBR6File> loadFromPath(const std::filesystem::path& path);

    // Build from decoded inputs (encodes automatically)
    static GBR6File fromInputs(
        GBR6Header hdr,
        std::vector<GBR6Input> p1,
        std::vector<GBR6Input> p2 = {});

    // Convert from old BRRMacro data
    static GBR6File fromLegacy(
        const std::string& name,
        float tps,
        const std::vector<std::pair<uint32_t, bool>>& p1FramePressed,  // (frame, pressed)
        const std::vector<std::pair<uint32_t, bool>>& p2FramePressed = {});
};
