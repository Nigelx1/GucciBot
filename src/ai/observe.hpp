#pragma once

// What the AI pilot sees each tick (stage B, 2026-11-02): the compact
// per-tick record of the AI contract v2, section 3, and its expansion to the
// 472 floats every network takes. ONE function builds the record, used by the
// recorder (src/ai/record.cpp) now and by the pilot later, so what the pilot
// is trained on is exactly what it sees when it plays.
//
// The record, in short (CONTRACT.md section 3 has every slot):
//   state  40 floats   the player: mode, gravity, size, speed, velocity, the
//                      button, the floor and ceiling of its mode, player 2
//                      in dual, the orb it touches
//   scan   160 bytes   40 columns of half a block, from 1 block behind to 19
//                      ahead: how far up and down the nearest deadly and the
//                      nearest solid region is, in 1/8 block (255: nothing
//                      within 12 blocks)
//   tok    48 bytes    the 8 nearest orbs, pads and portals ahead
//   meta   8 floats    where the player is, raw, for the trainer's labels
// Deadly and solid regions are hitborders (src/tools/hitborder_geom.hpp):
// each object's collision shape grown by the player's own box at its size
// and mode, so the player is a point. Everything is gravity-relative: upside
// down, "up" and "down" swap and velocities change sign.
//
// Where objects stand: objects the level's triggers move or toggle are read
// from the AI session's own World (where the last step left them); every
// other object stands where the level put it.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <matjson.hpp>

class GJBaseGameLayer;
class GameObject;
class PlayerObject;

namespace gucci::ai::obs {

    constexpr int kStateDim = 40;
    constexpr int kColumns = 40;
    constexpr int kScanDim = 4 * kColumns;
    constexpr int kTokens = 8;
    constexpr int kTokenBytes = 6;
    constexpr int kTokDim = kTokens * kTokenBytes;
    constexpr int kTokenTypes = 32;  // one-hot width; type 0 = empty slot
    constexpr int kMetaDim = 8;
    constexpr int kObsDim = kStateDim + kScanDim + kTokens * (2 + kTokenTypes);  // 472
    static_assert(kObsDim == 472);

    constexpr uint8_t kScanNone = 255;
    constexpr int kScanMax = 96;           // 12 blocks in 1/8 block
    constexpr float kBlock = 30.0f;        // GD units in a block
    constexpr float kColumnWidth = 15.0f;  // half a block

    // Token types (CONTRACT.md section 3), defined once here.
    enum Token : uint8_t {
        kNoToken = 0,
        kYellowOrb = 1, kPinkOrb, kRedOrb, kBlueOrb, kGreenOrb, kBlackOrb, kGreenDashOrb, kPinkDashOrb,
        kSpiderOrb, kTeleportOrb,
        kYellowPad = 11, kPinkPad, kRedPad, kBluePad, kSpiderPad,
        kGravityDownPortal = 16, kGravityUpPortal, kGravityFlipPortal,
        kCubePortal = 19, kShipPortal, kBallPortal, kUfoPortal, kWavePortal, kRobotPortal, kSpiderPortal,
        kSwingPortal,
        kMiniPortal = 27, kNormalSizePortal, kSpeedPortal, kMirrorPortal, kDualPortal,
        kTokenCount = 32,
    };
    char const* tokenName(int type);
    // The token an object is, or kNoToken.
    uint8_t tokenOf(GameObject* object);

    struct Record {
        float state[kStateDim] = {};
        uint8_t scan[kScanDim] = {};
        uint8_t tok[kTokDim] = {};
        float meta[kMetaDim] = {};
    };

    // The 472 floats, the same bits as tools/ai/gbai.py's expand_np.
    void expand(Record const& r, float* out);

    // The layout's hash (16 hex): every network carries the hash of the
    // layout it was trained on, and the mod refuses one that differs.
    std::string layoutHash();
    // The tables: state slots, scan, tokens, meta, N (gucci_ai_layout).
    matjson::Value layoutJson();

    // float <-> IEEE half (round to nearest even), as numpy's float16.
    uint16_t toHalf(float f);
    float fromHalf(uint16_t h);

    // The level read once for the scan and the tokens: every object GD's
    // collision pass can touch (src/hacks/hitborders.cpp's reading of it),
    // where it stood when read. The observer buckets them by x.
    struct Item {
        GameObject* object = nullptr;
        int slot = -1;           // world::WorldDef slot (-1: none)
        enum Type : uint8_t { Hazard, Special, Block, Slope } type = Hazard;
        uint8_t token = kNoToken;
        bool mover = false;      // a trigger can move or toggle it (WorldDef kReferenced)
        bool hazardSlope = false;
        float bx = 0.0f, by = 0.0f;  // m_positionX/Y when read (a mover's pose is relative to it)
        float cx = 0.0f, cy = 0.0f;  // its center (getRealPosition)
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // its collision rect
    };
    struct LevelIndex {
        std::vector<Item> items;
        float levelLength = 0.0f;
        int64_t levelId = 0;
    };

    // One session's observer: the index, the regions cached per player size
    // and mode, and the button's history. begin() at the session's start
    // (the level read there, while the game stands at the start), then once
    // a tick: observe() before the step, after() once the step has run.
    class Observer {
    public:
        Observer();
        ~Observer();
        // Reads the level (pl must be the open level, the session open).
        bool begin(GJBaseGameLayer* pl, std::string& why);
        bool ready() const;
        // The run's tick rate (state[23] = 60 / tps). After begin().
        void setTps(double tps);
        // The record of tick t: what the pilot sees before step t. Reads
        // the session's copies and objects; the session must be open.
        void observe(Record& out);
        // Step t has run: act = player 1's button as GD applied it.
        void after(bool act);
        // Back to a saved tick (the practice loop's restore): the button's
        // history as it stood then.
        struct History {
            uint32_t tick = 0;
            bool prev = false;
            int64_t lastPress = -1;
            int64_t lastRelease = -1;
        };
        History history() const;
        void setHistory(History const& h);

        // What the level read found, and what observing has cost.
        struct Stats {
            int items = 0;        // objects in the index
            int movers = 0;       // ... the World can move or toggle
            int tokens = 0;       // ... orbs, pads, portals
            int configs = 0;      // player sizes/modes seen
            double buildMs = 0.0;
            uint64_t observed = 0;
            double observeUs = 0.0;  // total
            double maxUs = 0.0;
            uint64_t moverPoses = 0;
        };
        Stats const& stats() const;
        LevelIndex const* index() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

} // namespace gucci::ai::obs
