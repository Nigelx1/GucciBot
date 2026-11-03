#pragma once

// The AI pilot's per-tick record and its 494-float expansion (AI contract v2,
// section 3, layout v3: the continuous geometry block added 2026-11-03),
// with no game code in it: the observer (src/ai/observe.*) fills
// a Record, the networks (src/ai/net.*) read the expansion, and the offline
// test (tools/run_ai_net_tests.cpp) checks the expansion against the
// trainer's golden record bit for bit.

#include <cstdint>

namespace gucci::ai::obs {

    constexpr int kStateDim = 40;
    constexpr int kColumns = 40;
    constexpr int kScanDim = 4 * kColumns;
    constexpr int kTokens = 8;
    constexpr int kTokenBytes = 6;
    constexpr int kTokDim = kTokens * kTokenBytes;
    constexpr int kTokenTypes = 32;  // one-hot width; type 0 = empty slot
    constexpr int kMetaDim = 8;
    // Layout v3 (2026-11-03): exact distances, so the pilot can tell which tick
    // to click (a 0.5-block scan column holds an obstacle for ~11 ticks at 1x).
    // [0-8]  dx to the first DEADLY hitborder along a horizontal ray from the
    //        player's center at heights -2..+2 blocks (step 0.5, gravity-relative)
    // [9-17] the same to the first SOLID hitborder
    // [18]   a floor under the player now (0/1); [19] dx to where it ends, or
    //        with none, to where the next one starts
    // [20]   a ceiling over the player now (0/1); [21] dx to where it ends, or
    //        to where the next one starts
    // dx in blocks, 0..kGeoReach; kGeoNone = nothing within reach.
    constexpr int kRays = 9;
    constexpr int kGeoDim = 2 * kRays + 4;  // 22
    constexpr float kGeoReach = 20.0f;
    constexpr float kGeoNone = 20.5f;
    constexpr int kGeoOffset = kStateDim + kScanDim + kTokens * (2 + kTokenTypes);  // 472
    constexpr int kObsDim = kGeoOffset + kGeoDim;  // 494
    static_assert(kObsDim == 494);

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

    struct Record {
        float state[kStateDim] = {};
        uint8_t scan[kScanDim] = {};
        uint8_t tok[kTokDim] = {};
        float meta[kMetaDim] = {};
        float geo[kGeoDim] = {};
    };

    // The 494 floats, the same bits as tools/ai/gbai.py's expand_np:
    // [state 40 as-is][scan 160: 255 -> 12.5, else v / 8][per token: dx, dy,
    // one-hot(type, 32), index 0 unused; a slot whose type is 0 or outside
    // 1..31 is all zeros][geo 22 as-is].
    void expand(Record const& r, float* out);

    // float <-> IEEE half (round to nearest even), as numpy's float16.
    uint16_t toHalf(float f);
    float fromHalf(uint16_t h);

} // namespace gucci::ai::obs
