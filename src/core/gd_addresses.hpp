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
        //    build keeps GucciBot's off-Windows engine (240, or 162 TPS and
        //    up) until that design is ported (STATUS.md, the next job).
        //  - the y-velocity rounding midhook in PlayerObject::updateJump
        //    (2.2081: 0x38c315, resume 0x38c34d; 2.2091 function 0x4e9b80).
        //    Silicate 1.1.2 has no such midhook, so High TPS Precision stays
        //    off on 2.2091 (as on Mac and Android).

        // Only in the Geode 6 build, and only where core/geode_compat.hpp was
        // included first (it defines GB_GEODE6); the 2.2081 build never sees it.
#if defined(GB_GEODE6) && GB_GEODE6
        // ==== Lane B (safety on 2.2091; hacks/safemode_2209.cpp) ====
        //
        // 2.209 added death tracking and uploads, playtime, platformer saves
        // and leaderboard times with an input recording, GD's own speed
        // slider and keys, and GD's own Ignore Damage. The numbers below were
        // read (2026-11-16, safety2209 lane) from a copy of the GD 2.2091
        // Windows executable, read only, and match Silicate 1.1.2's where it
        // has them (its src/hooks/SafeMode.cpp and TimeSpeedControl.cpp).
        // Each raw field is used only after the bytes below are found where
        // they should be; on a mismatch the feature that needs it stays off
        // and the log says so.

        // PlayLayer fields GD's own no-progress checks read: destroyPlayer
        // (+0x52b / +0x6eb), levelComplete and dtCommitPlaytime skip the
        // death tracker, playtime and progress while either is set, and
        // updateTestModeLabel shows "Used Ignore Damage" / "Used Speed
        // Change" from them. Silicate 1.1.2 raises 0x39fa (its
        // PLAYLAYER_SPEED_CHANGED) around a death and a completion; GucciBot
        // does the same (its NoProgress).
        inline constexpr uintptr_t kPlIgnoreDamageUsed = 0x39f9;  // this attempt uses Ignore Damage
        inline constexpr uintptr_t kPlSpeedChanged = 0x39fa;      // this attempt changed GD's speed
        // GJBaseGameLayer: GD's game speed and music speed (the speed slider
        // and keys write them). Silicate 1.1.2: GD_SPEED_OFFSET / GD_MUSIC_SPEED_OFFSET.
        inline constexpr uintptr_t kGblSpeed = 0x3560;
        inline constexpr uintptr_t kGblMusicSpeed = 0x3564;
        // GJBaseGameLayer: Ignore Damage asked for (the pause menu's side
        // button / keybind), and PlayerObject's own flag. resetLevel turns
        // Ignore Damage off by clearing 0x39f9, 0x8a2 and both players' 0x7ed.
        inline constexpr uintptr_t kGblIgnoreDamage = 0x8a2;
        inline constexpr uintptr_t kPlayerIgnoreDamage = 0x7ed;

        // PlayLayer::updateTestModeLabel (0x505ed0) +0x33: `cmp byte [rbx+0x39f9], 0;
        // jne; lea rdi, [rbx+0x39fa]` -- both flags where they're expected.
        inline const GuardedSite kCheckFlags{
            0x505f03,
            {0x80, 0xbb, 0xf9, 0x39, 0x00, 0x00, 0x00, 0x75, 0x2c, 0x48, 0x8d, 0xbb, 0xfa, 0x39, 0x00, 0x00},
            "PlayLayer no-progress flags (0x39f9 / 0x39fa)"};
        // Same function +0x51: `movss xmm0, [rbx+0x3560]` -- GD's speed.
        inline const GuardedSite kCheckSpeed{
            0x505f21, {0xf3, 0x0f, 0x10, 0x83, 0x60, 0x35, 0x00, 0x00}, "GD speed (0x3560)"};
        // TimeSpeedControl::timeSliderChanged (0x66dc50) +0xb4: `movss [rax+0x3564], xmm0`.
        inline const GuardedSite kCheckMusicSpeed{
            0x66dd04, {0xf3, 0x0f, 0x11, 0x80, 0x64, 0x35, 0x00, 0x00}, "GD music speed (0x3564)"};
        // PlayLayer::resetLevel (0x518c00) +0xc2: GD turning Ignore Damage
        // off, which GucciBot's Legit only repeats.
        inline const GuardedSite kCheckIgnoreDamageOff{
            0x518cc2,
            {0x44, 0x38, 0xa6, 0xf9, 0x39, 0x00, 0x00, 0x74, 0x37, 0x84, 0xc0, 0x75, 0x33, 0x88, 0x86,
             0xf9, 0x39, 0x00, 0x00, 0x44, 0x88, 0xa6, 0xa2, 0x08, 0x00, 0x00, 0x48, 0x8b, 0x86, 0xb8,
             0x0d, 0x00, 0x00, 0x44, 0x88, 0xa0, 0xed, 0x07, 0x00, 0x00, 0x48, 0x8b, 0x86, 0xc0, 0x0d,
             0x00, 0x00, 0x44, 0x88, 0xa0, 0xed, 0x07, 0x00, 0x00},
            "Ignore Damage fields (0x39f9 / 0x8a2 / player 0x7ed)"};

        // GJGameLevel's practice attempt count (new in 2.209; Silicate 1.1.2's
        // LEVEL_PRACTICE_ATTEMPTS). PlayLayer::resetLevel +0xd69: `mov rax,
        // [rsi+0x878]; inc dword [rax+0x6ac]` (the level, then the count).
        inline constexpr uintptr_t kLevelPracticeAttempts = 0x6ac;
        inline const GuardedSite kCheckPracticeAttempts{
            0x519969, {0x48, 0x8b, 0x86, 0x78, 0x08, 0x00, 0x00, 0xff, 0x80, 0xac, 0x06, 0x00, 0x00},
            "GJGameLevel practice attempts (0x6ac)"};

        // GameLevelManager::uploadDeathData (0x2266f0, the bindings' number;
        // TodoReturn there, so it is hooked by address, not through
        // $modify). Its prologue. It reads only `this` (rcx) before it
        // checks GJAccountManager's account ID ([+0x168] - [+0x16c] > 0,
        // else it returns): it needs a signed-in account. GD's only caller
        // is LevelInfoLayer::onEnterTransitionDidFinish.
        inline const GuardedSite kUploadDeathData{
            0x2266f0,
            {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20,
             0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57},
            "GameLevelManager::uploadDeathData"};

        // TimeSpeedControl::timeSliderChanged (0x66dc50, the bindings'
        // number; TodoReturn there, so it can be neither hooked nor called
        // through the bindings): GD's own handler for its speed slider,
        // `void (TimeSpeedControl*, CCObject* thumb)` (it hands rdx to
        // SliderThumb::getValue). Its prologue. Called by address while
        // GucciBot is switched off.
        inline const GuardedSite kTimeSliderChanged{
            0x66dc50,
            {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x29, 0x7c, 0x24, 0x20, 0x48,
             0x8b, 0xd9, 0x48, 0x8b, 0xca},
            "TimeSpeedControl::timeSliderChanged"};

        // UILayer::handleAction: the speed up / down keybinds (10022 /
        // 10023) right before GD adds the step to its speed (`mov rax,
        // [rdi+0x100]; addss xmm0, [rax+0x3560]`), and where that path
        // ends (`mov al, 1; jmp`). Silicate 1.1.2, TimeSpeedControl.cpp
        // (SPEED_KEYBIND_OFFSET / _DONE_OFFSET), same bytes.
        inline const GuardedSite kSpeedKeybind{
            0x672c83,
            {0x48, 0x8b, 0x87, 0x00, 0x01, 0x00, 0x00, 0xf3, 0x0f, 0x58, 0x80, 0x60, 0x35, 0x00, 0x00},
            "speedKeybind"};
        inline const GuardedSite kSpeedKeybindDone{0x672da4, {0xb0, 0x01, 0xeb, 0x02}, "speedKeybindDone"};

#endif  // GB_GEODE6 (Lane B)

        // What the launch self-check expects placed on 2.2091 (core/engine_core.cpp).
        inline constexpr int kExpectedMidhooks = 6;  // 4 shake + teleport + speed keys
        inline constexpr int kExpectedPatches = 9;

    } // namespace v2091

} // namespace gucci::gdaddr
#endif // GB_GD2091_SITES
