#pragma once

// Auto safe mode (dihmaster500's ask; Nigel's spec, 2026-10-12). While a cheat
// is on, a run doesn't count: finishing the level, a new best and a death's %
// save nothing and send nothing, and GD's stats, achievements and the level's
// attempt and jump counts don't move (hacks/safemode.cpp has how; GD's own
// test-mode flag, which start positions use, is never left changed). The Safe
// mode switch (Hacks > Gameplay) does the same, cheat or not. Invisible: no
// label.
//
// With auto safe mode off, a small red dot shows in the top-left corner while a
// cheat is on (or was, this attempt), drawn into the game so recordings and
// renders show it; a green one, if switched on, while none is. Hiding the red
// dot takes the owner's password. Only a salted, 10,000-round SHA-256 of it is
// in this (public) code, and an unlocked machine keeps the 5,000-round midpoint,
// which can't be worked out from the code. Anyone building GucciBot from source
// can take the lock out: it keeps the normal download honest.
//
// A cheat: a macro, Calculate or the pathfinder playing; noclip; speedhack;
// physics at a TPS other than 240; the autoclicker; Prevent Death; frame
// advance (the engine's pause). One at any point taints the whole attempt.
//
// Forced safe mode (2026-10-31) is the switch's effect for one launch, for
// Claude's test runs over Assistant Access, which play on the owner's real
// account: on while the game was started with GUCCIBOT_FORCE_SAFE_MODE set
// (tools/assist.py does that), or once the gucci_force_safe_mode tool has
// turned it on. It only turns on, and it is never saved: the switch, the
// hotkey and the saved choice stay the user's, and the next normal launch is
// back to them. The red dot stays hidden while it is on.

#include <string>

namespace gucci::safemode {

    bool forced();
    void forceForSession();

    bool autoSafeMode();
    void setAutoSafeMode(bool on);

    bool unlocked();
    bool unlock(std::string const& password); // false: wrong password (or none set yet)
    void lock();

    bool dotHidden();
    void setDotHidden(bool hidden); // only while unlocked

    // A green dot while no cheat is on (off by default: nothing shows then).
    bool greenDot();
    void setGreenDot(bool on);

    bool cheatingNow();
    // This attempt has had something on that counts as cheating (auto safe
    // mode keeps it from counting). For Assistant Access's get_state.
    bool tainted();

} // namespace gucci::safemode
