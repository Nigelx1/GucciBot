#pragma once

// Legit only (Nigel, 2026-11-02), Mega Hack-style: one switch that turns off
// every feature safe mode counts as a cheat (hacks/safemode.cpp, cheatingNow
// and showsTheWay) and keeps them off. It stands in for "Counts as a cheat"
// labels: none are shown.
//
// While it's on:
//   - every such switch is off and its control is greyed out, with "Legit
//     only is on" under it (kit::LegitGate);
//   - its hotkey does nothing but say so, and Assistant Access's test
//     settings refuse to turn one on;
//   - a macro can't play or record, and Calculate, the pathfinders and frame
//     advance can't run: whichever is going stops;
//   - the physics rate stays at 240 TPS and the speed at 1.00x.
// Anything that gets one turned on anyway (a settings preset loaded, a
// setting read back later) is turned off again before the next tick, and
// what it asked for is kept for when Legit only goes off.
//
// The user's own switches are never overwritten: each one's value is held
// aside (in memory, never saved), the saved setting stays as it was, and
// turning Legit only off puts every value back. Settings saved as a group
// (the autoclicker's, the hitborders', a trainer's, a settings preset) save
// the held value, not the forced-off one (kept()).
//
// Three of them only count as a cheat in their trainer's own level, and are
// held off only there: a trainer page's click bar, and each trainer's Video
// Mode. Force platformer is a switch for the levels opened after it: a level
// already opened as a platformer stays one until it's left (GD settles it
// while the level loads), so Legit only turns the switch off and that level
// still counts as a cheat until it's reopened.
//
// Saved (legit_only), off by default. The switch sits under Safe mode on the
// Hacks page.

namespace gucci::legit {

    bool on();
    void setOn(bool on);

    // Every frame (a drawScene hook): turns off whatever is on.
    void enforce();

    // A switch's value as the user left it: the held value while Legit only
    // holds it off, else the switch itself. For settings saved as a group.
    bool kept(bool const& field);
    double kept(double const& field);

    // The words a greyed-out control shows under it.
    inline constexpr const char* kReason = "Legit only is on";

    // A hotkey for a cheat pressed while it's on: says so, and true (the
    // key does nothing else).
    bool blockKey();

} // namespace gucci::legit
