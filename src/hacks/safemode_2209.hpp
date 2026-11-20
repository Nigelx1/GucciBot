#pragma once

// Safe mode's GD 2.2091 half (Geode 6, Windows). 2.209 added systems 2.2081
// doesn't have: death tracking and its upload, playtime, platformer saves,
// platformer leaderboard times with an input recording, GD's own speed slider
// and keys, and GD's own Ignore Damage. hacks/safemode_2209.cpp keeps each one
// to Nigel's rules (2026-11-16):
//  1. death data, playtime and platformer leaderboard times (with their input
//     recordings) follow safe mode, the same as a completion;
//  2. GD's speed slider and keys drive GucciBot's speed, the way Silicate 1.1.2
//     does it; GD's own speed is otherwise held at 1x, and any speed but 1x is
//     a cheat;
//  3. GD's Ignore Damage is a cheat (red dot, safe mode, Legit only);
//  4. no platformer save is written for an attempt with the bot or a cheat on
//     in it; legit runs save as normal. A full restart with every cheat off
//     starts a clean attempt that saves again; a respawn at a checkpoint
//     doesn't (Nigel, 2026-11-19; until then the whole visit stayed held).
// None of it is in the GD 2.2081 file: everything here is behind
// GB_SAFETY_2209, which is 0 there.

#include "core/geode_compat.hpp"  // GB_GEODE6

#include <Geode/Geode.hpp>

#if GB_GEODE6 && defined(GEODE_IS_WINDOWS)
#define GB_SAFETY_2209 1
#else
#define GB_SAFETY_2209 0
#endif

#if GB_SAFETY_2209

#include <string>

namespace gucci::safemode {

    // safemode.cpp: the run is kept from counting (the Safe mode switch, a
    // test launch's forced safe mode, or auto safe mode with a cheat on in
    // this attempt).
    bool runShielded();

    // GD's own Ignore Damage is on in this attempt, or GD's own speed isn't
    // 1x (safemode.cpp's cheatingNow reads it).
    bool gdOwnCheat();

    // Legit only (hacks/legit.cpp): GD's Ignore Damage on, and turning it off
    // the way GD's resetLevel does.
    bool gdIgnoreDamageOn();
    void gdIgnoreDamageOff();

    // GD's no-progress flag (PlayLayer +0x39fa, "Used Speed Change") raised
    // while one of these is alive, and m_isTestMode with it when asked: GD's
    // own death tracker, playtime and progress then skip (Silicate 1.1.2's
    // NoProgress). Nothing happens if `on` is false or the byte check failed.
    class NoProgress {
    public:
        NoProgress(PlayLayer* pl, bool on, bool testMode = true);
        ~NoProgress();
        NoProgress(NoProgress const&) = delete;
        NoProgress& operator=(NoProgress const&) = delete;

    private:
        PlayLayer* m_pl = nullptr;
        bool m_raisedTest = false;
        bool m_wasTest = false;
        bool m_wasSpeed = false;
    };

    // For Assistant Access's safety2209_probe key (mcp/test_settings.cpp):
    // what each guard let through or held back this launch.
    std::string probeCounts();
    // What GD keeps about the level being played and the player's stats, for a
    // test to compare before and after (the death tracker, playtime, attempts,
    // percent, stars, achievements, GD's own speed and Ignore Damage).
    std::string probeLevel();
    // A test launch's probe actions (the caller checks it is one); returns ""
    // when done, else why not. "upload" calls GD's death-data upload through
    // GucciBot's hook (always held back in a test launch, so nothing leaves the
    // PC); the rest are listed in safemode_2209.cpp.
    std::string probeAction(std::string const& action);

}  // namespace gucci::safemode

#endif
