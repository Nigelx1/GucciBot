#pragma once

// Auto safe mode (Dihmaster500's ask; Nigel's spec, 2026-10-12). While a cheat
// is on, a run doesn't count: the attempt is marked a test run (GD's own
// m_isTestMode, the flag a start position sets), so finishing the level or a
// new best saves nothing and sends nothing. GD's "Testmode" label keeps
// showing only what GD itself would show: safe mode is invisible.
//
// With auto safe mode off, a small red dot shows in the corner while a cheat
// is on, drawn into the game so recordings and renders show it. Hiding the
// dot as well takes the owner's password. Only a salted, 10,000-round SHA-256
// of it is in this (public) code, and an unlocked machine keeps the 5,000-round
// midpoint, which can't be worked out from the code. Anyone building GucciBot
// from source can take the lock out: it keeps the normal download honest.
//
// A cheat: a macro, Calculate or the pathfinder playing; noclip; speedhack;
// physics at a TPS other than 240; the autoclicker; Prevent Death; frame
// advance (the engine's pause). One at any point taints the whole attempt.

#include <string>

namespace gucci::safemode {

    bool autoSafeMode();
    void setAutoSafeMode(bool on);

    bool unlocked();
    bool unlock(std::string const& password); // false: wrong password (or none set yet)
    void lock();

    bool dotHidden();
    void setDotHidden(bool hidden); // only while unlocked

    bool cheatingNow();

} // namespace gucci::safemode
