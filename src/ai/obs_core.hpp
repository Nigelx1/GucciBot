#pragma once

// The AI pilot's per-tick record and its 472-float expansion (AI contract v2,
// section 3), with no game code in it: the observer (src/ai/observe.*) fills
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

    struct Record {
        float state[kStateDim] = {};
        uint8_t scan[kScanDim] = {};
        uint8_t tok[kTokDim] = {};
        float meta[kMetaDim] = {};
    };

    // The 472 floats, the same bits as tools/ai/gbai.py's expand_np:
    // [state 40 as-is][scan 160: 255 -> 12.5, else v / 8][per token: dx, dy,
    // one-hot(type, 32), index 0 unused; a slot whose type is 0 or outside
    // 1..31 is all zeros].
    void expand(Record const& r, float* out);

    // float <-> IEEE half (round to nearest even), as numpy's float16.
    uint16_t toHalf(float f);
    float fromHalf(uint16_t h);

} // namespace gucci::ai::obs
