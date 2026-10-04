#pragma once

// The Survival Indicator: a green / red mark on screen while you play, for
// whether clicking right now survives what is coming. Written fresh on
// 2026-10-03 (the first one was drawn by the menu code removed in build -bz).
//
// The question is the fork service's (analysis/trajectory.hpp, survivesFor):
// once per tick, after the frame's ticks have run, a copy of player 1 is run
// for the look-ahead window twice - once "clicking" (pressing if the button is
// up, letting go if it is held, and keeping that for the whole window) and
// once doing nothing. Green when the click lives through the window, red when
// it does not. Doing nothing dying inside the window is what makes a click
// needed, and how soon it dies is the margin the styles and the cue show.
//
// The click cue: when a click is needed and would be safe, a click sound plays,
// pitched higher the less room is left. With a calibration for the current
// gamemode (trainers/calibration.hpp, a metronome that measures how long you
// take to answer a sound), the cue asks whether a click your reaction time from
// now would be safe instead, so you land on the moment rather than behind it.
//
// Every real jump press or release of player 1 is held against the reading
// shown when it arrived: the flash on click, and the accuracy / streak HUD.
//
// The settings are GucciEngine's (survivalIndicator, indicator*, accuracy*,
// currentStreak, bestStreak); this module loads and saves them. The page is
// ui/pages/indicators.cpp. Main thread only.

namespace gucci::indicator {

    enum Style : int { Ring, Classic, Converge, Pulse, StyleCount };
    const char* styleName(int style);

    // The look-ahead window, in ticks.
    inline constexpr int kMinLookahead = 2;
    inline constexpr int kMaxLookahead = 120;

    // What the indicator last worked out, for the page's live line.
    struct Reading {
        bool valid = false;
        bool held = false;    // the button was down: "clicking" meant letting go
        bool safe = false;    // the click lives through the window
        bool needed = false;  // doing nothing dies inside the window
        int lookahead = 0;    // the window, in ticks
        int clickTicks = 0;   // how long the click lived (lookahead = all of it)
        int keepTicks = 0;    // how long doing nothing lived
    };
    Reading const& reading();

    // Reads the saved settings into the engine (from the mod's Loaded event),
    // and writes them back (the page, on every change).
    void loadSettings();
    void saveSettings();

    // A real jump press or release (hooks/hook_gjbasegamelayer.cpp, while the
    // indicator is on and no macro is playing).
    void onRealClick(bool p2, bool pressed);

    // The accuracy and streak start over; resetBest clears the saved best too.
    void resetStats(bool resetBest);

} // namespace gucci::indicator
