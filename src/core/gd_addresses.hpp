#pragma once

// GucciBot's raw addresses in GD's Windows executable, for GD 2.2081 (Geode 5)
// and GD 2.2091 (Geode 6). One place lists every one, what it is, and where
// the 2.2091 number comes from (docs/PORT_2209_ASSESSMENT.md, section 2A).
//
// The 2.2081 sites are placed where they always were (engine_updater.cpp,
// hooks/hook_gjbasegamelayer.cpp, hooks/hook_playerobject.cpp,
// hooks/hook_playlayer.cpp); their numbers are repeated here only as the
// reference the 2.2091 ones were matched against.
//
// 2.2091 sources:
//  - Silicate 1.1.2 (peony and contributors, GPL-3; its port to GD 2.2091,
//    tag 1.1.2 of git.puppy.lgbt/silicate/silicate). Addresses AND the bytes
//    each site must hold before it is touched are Silicate's: GucciBot checks
//    those bytes first and leaves the site alone on a mismatch (Silicate's
//    util::expectBytes / guardedPatch), so a wrong number logs an error in
//    place of corrupting GD's code.
//  - The Geode bindings for 2.2091 (geode-sdk/bindings, pinned commit in
//    CMakeLists.txt), which give each function's start. Checked 2026-11-16:
//    every Silicate site below lies inside the function it is named for, and
//    each call the patches remove lands on the function it should
//    (PlayerObject::releaseAllButtons = 0x4fd450, UILayer::resetUINodeState
//    = 0x6710a0, the four shake calls all through one import slot).
//
// Nothing here has run in a GD 2.2091 game yet: there is no 2.2091 install to
// test on (port-2209 lane, 2026-11-16).

#include <cstdint>
#include <initializer_list>
#include "core/platform.hpp"      // GB_GD2091_SITES

// Only the GD 2.2091 build uses these tables (every user is under
// GB_GD2091_SITES), so the GD 2.2081 build compiles none of them.
#if GB_GD2091_SITES
namespace gucci::gdaddr {

    // A byte patch with the bytes it expects to find first.
    struct GuardedPatch {
        uintptr_t offset;
        std::initializer_list<uint8_t> expect;
        std::initializer_list<uint8_t> write;  // empty: as many NOPs as `expect`
        char const* name;
    };

    // A midhook with the bytes it expects at its spot.
    struct GuardedSite {
        uintptr_t offset;
        std::initializer_list<uint8_t> expect;
        char const* name;
    };

    namespace v2091 {

        // ---- Byte patches (2.2081 had 4; 2.2091 needs 9 for the same 4 jobs)

        // GJBaseGameLayer::resetLevelVariables (0x344fa0): don't release both
        // players' buttons on a reset. Two `mov rcx, player; call
        // releaseAllButtons` pairs, NOP'd. 2.2081: 0x23B4EA, 24 bytes, the
        // same job. Silicate 1.1.2, src/hooks/GJBaseGameLayer.cpp.
        inline const GuardedPatch kResetLevelVariables{
            0x34549c,
            {0x48, 0x8b, 0x8e, 0xb8, 0x0d, 0x00, 0x00, 0xe8, 0xa8, 0x7f, 0x1b, 0x00,
             0x48, 0x8b, 0x8e, 0xc0, 0x0d, 0x00, 0x00, 0xe8, 0x9c, 0x7f, 0x1b, 0x00},
            {},
            "GJBaseGameLayer::resetLevelVariables keep held buttons"};

        // The restart keys don't forget the held touch. 2.2081 did this with
        // one 19-byte NOP inside UILayer::handleKeypress (0x4cd95c); in 2.2091
        // handleKeypress has no address of its own (inline in the bindings)
        // and each restart key has its own `mov [rdi+0xf0], -1; lea rcx, ..;
        // call UILayer::resetUINodeState`. Silicate 1.1.2 patches all four
        // keys and the start-pos cycle's reset call; so does GucciBot.
        inline const GuardedPatch kRestartKey{
            0x67170a,
            {0x48, 0xc7, 0x87, 0xf0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x48,
             0x8d, 0x8f, 0xa8, 0xfe, 0xff, 0xff, 0xe8, 0x7f, 0xf9, 0xff, 0xff},
            {},
            "restart key keeps the held touch"};
        inline const GuardedPatch kFullRestartKey{
            0x671809,
            {0x48, 0xc7, 0x87, 0xf0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x48,
             0x8d, 0x8f, 0xa8, 0xfe, 0xff, 0xff, 0xe8, 0x80, 0xf8, 0xff, 0xff},
            {},
            "full restart key keeps the held touch"};
        inline const GuardedPatch kStartPosRestartKey{
            0x6727b9,
            {0x48, 0xc7, 0x87, 0xf0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x48,
             0x8d, 0x8f, 0xa8, 0xfe, 0xff, 0xff, 0xe8, 0xd0, 0xe8, 0xff, 0xff},
            {},
            "start pos restart key keeps the held touch"};
        inline const GuardedPatch kStartPosCycleKey{
            0x672a0f,
            {0x48, 0xc7, 0x87, 0xf0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff},
            {},
            "start pos cycle key keeps the held touch"};
        inline const GuardedPatch kStartPosCycleReset{
            0x672a24,
            {0xe8, 0x77, 0xe6, 0xff, 0xff},
            {},
            "start pos cycle key skips the touch reset"};

        // PlayLayer::resetLevel (0x518c00): always take the one-player
        // held-touch path. 2.2081: 0x3B994C, a 2-byte `jmp short`; 2.2091's
        // jump is longer (resetLevel grew ~1 KB), a 5-byte `jmp near` over
        // the `jz` and the start of the compare. Silicate 1.1.2.
        inline const GuardedPatch kResetLevelHeld{
            0x5199ff,
            {0x74, 0x27, 0x80, 0xbe, 0xe0},
            {0xe9, 0x41, 0x02, 0x00, 0x00},
            "PlayLayer::resetLevel one-player held path"};

        // PlayLayer::resumeAndRestart (0x51a750): don't release either
        // player's buttons. 2.2081: one 5-byte NOP at 0x3BA508; 2.2091 calls
        // releaseAllButtons once per player. Silicate 1.1.2.
        inline const GuardedPatch kResumeAndRestartP1{
            0x51a8a8, {0xe8, 0xa3, 0x2b, 0xfe, 0xff}, {}, "PlayLayer::resumeAndRestart keeps P1's buttons"};
        inline const GuardedPatch kResumeAndRestartP2{
            0x51a8b9, {0xe8, 0x92, 0x2b, 0xfe, 0xff}, {}, "PlayLayer::resumeAndRestart keeps P2's buttons"};

        // ---- Midhooks

        // GJBaseGameLayer::updateCamera (0x345c60): the screen shake's four
        // rand() calls (`call [rand]`, 6 bytes; the handler skips them).
        // 2.2081: 0x23E173 / 0x23E1A1 / 0x23E1CB / 0x23E1E9. Silicate 1.1.2.
        inline const GuardedSite kShake1{0x34808a, {0xff, 0x15, 0x20, 0x66, 0x38, 0x00}, "shakeRandom1"};
        inline const GuardedSite kShake2{0x3480b8, {0xff, 0x15, 0xf2, 0x65, 0x38, 0x00}, "shakeRandom2"};
        inline const GuardedSite kShake3{0x3480e2, {0xff, 0x15, 0xc8, 0x65, 0x38, 0x00}, "shakeRandom3"};
        inline const GuardedSite kShake4{0x348100, {0xff, 0x15, 0xaa, 0x65, 0x38, 0x00}, "shakeRandom4"};

        // GJBaseGameLayer::teleportPlayer (0x315f20): right after the random
        // pick's rand(), where `movd xmm1, eax` reads it (the handler sets
        // rax, no skip). 2.2081: 0x20FEDC. Silicate 1.1.2.
        inline const GuardedSite kTeleportRandom{
            0x316053, {0x66, 0x0f, 0x6e, 0xc8, 0x0f, 0x5b, 0xc9}, "teleportRandomOverride"};

        // ---- Known but NOT placed on 2.2091 (reasons in each comment)

        // PlayerObject::tryPlaceCheckpoint (0x501880) +0x3f1: its call to
        // PlayLayer::markCheckpoint (0x516630, checked in the bindings).
        // 2.2081: 0x3A3657. Silicate 1.1.2 has it, but GucciBot replaces
        // tryPlaceCheckpoint on every platform (hooks/hook_playerobject.cpp)
        // and never calls GD's, so GD's code this sits in doesn't run. Kept
        // for reference only.
        inline const GuardedSite kCheckpointPlacement{
            0x501c71, {0xe8, 0xba, 0x49, 0x01, 0x00}, "checkpointPlacement"};

        // libcocos2d.dll: CCActionManager::update. 2.2081: 0x38B90. Silicate
        // 1.1.2: 0x392d0. Not used: on Geode 6 GucciBot hooks it by name
        // (GBPortableActionManager, engine_updater.cpp), which needs no number.
        inline constexpr uintptr_t kCocosActionManagerUpdate = 0x392d0;

        // UILayer::onCheck, the checkpoint key/button. 2.2081: 0x4ce060 (an
        // address hook, because the bindings didn't name it). 2.2091: 0x673050
        // in both the bindings and Silicate 1.1.2, so on Geode 6 GucciBot
        // hooks it through the bindings (GBPortableCheckKey,
        // hook_playlayer.cpp). No raw number used.
        inline constexpr uintptr_t kUILayerOnCheck = 0x673050;

        // GD's fast-rand state. 2.2081: 0x6c2e90. 2.2091: 0x90bf68, in both
        // Silicate 1.1.2 (ReplaySystem::getCurrentRandomState) and the
        // bindings' inline GameToolbox::getfast_srand / fast_srand, which is
        // what GucciBot calls on Geode 6 (core/platform.hpp). No raw number.
        inline constexpr uintptr_t kFastRandState = 0x90bf68;

        // NOT FOUND for 2.2091 (needs the 2.2091 executable to find):
        //  - the tick loop's 5 midhooks inside GJBaseGameLayer::update
        //    (0x3406d0): physDt, physStepCount, earlyUpdate, frameUpdate,
        //    restorePhysDt (2.2081: 0x237A7C, 0x237DCE, 0x237E42, 0x238BAA,
        //    0x238F6E). Silicate 1.1.2 doesn't have them any more: it replaced
        //    them with ordinary hooks (update, getModifiedDelta,
        //    processCommands) and ONE byte-checked patch at 0x3408f0 that
        //    makes GD's update take its step count from the bot. The 2.2091
        //    build keeps GucciBot's off-Windows engine (240 TPS) until that
        //    design is ported (STATUS.md, the next job).
        //  - the y-velocity rounding midhook in PlayerObject::updateJump
        //    (2.2081: 0x38c315, resume 0x38c34d; 2.2091 function 0x4e9b80).
        //    Silicate 1.1.2 has no such midhook, so High TPS Precision stays
        //    off on 2.2091 (as on Mac and Android).

        // What the launch self-check expects placed on 2.2091 (core/engine_core.cpp).
        inline constexpr int kExpectedMidhooks = 5;  // 4 shake + teleport
        inline constexpr int kExpectedPatches = 9;

    } // namespace v2091

} // namespace gucci::gdaddr
#endif // GB_GD2091_SITES
