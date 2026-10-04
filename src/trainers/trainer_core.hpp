#pragma once

// The engine half of the two practice trainers: Nigel's Jupiter My Favourite
// Trainer (one fixed level, its macro bundled with the mod) and the Trainer
// (any saved macro). Each has a click bar running on its own clock, the
// player's own presses and releases laid against the macro's, Click
// Indicators scoring, segments with real practice-checkpoint looping, session
// stats and synced music.
//
// The state itself lives on GucciEngine (its jupiter* and trainer* fields);
// this is what moves it. The pages are ui/pages/trainer_pages.cpp; the ghosts
// and both music players are trainers/jupiterghost.cpp and trainerghost.cpp.
// Written fresh for 2.0 (the old menu and drawing went with build -bz).

#include "core/GucciBot.hpp"

#include <filesystem>
#include <string>
#include <vector>

class PlayLayer;

namespace gucci::trainers {

    enum class Kind { Jupiter, Any };

    // One trainer's fields on GucciEngine, by reference, so the clock, the
    // scoring and the page are written once for both trainers.
    struct State {
        GucciEngine::TrainerMacroData& macro;
        bool& barEnabled;
        float& barWindowSec;   // seconds of macro shown across the bar
        bool& paused;
        double& posSec;        // the clock: macro time at the bar's centre line
        double& lastRealTime;  // steady-clock seconds of the last tick
        bool& barLoop;
        bool& pageVisible;     // the page was drawn this frame or the one before
        std::vector<double>& myPresses;
        std::vector<double>& myReleases;
        ClickIndicatorScore& score;
        bool& musicEnabled;
        float& musicOffsetSec;
        int& attempts;
        float& sessionBestPct;
        std::vector<float>& deathPcts;
        bool& loopEnabled;
        int& loopStartIdx;
        int& loopEndIdx;
        bool& ghost;
        bool& bestGhost;
        bool& scrubActive;
        float& scrubPercent;
        std::string& notes;
        std::string& segmentsRaw;
    };
    State state(Kind k);

    // Saved settings, read once. The clock's first tick does it; the pages
    // call it too, harmlessly. (The Trainer's saved macro comes back a little
    // later, when a level opens or a trainer page is drawn.)
    void ensureLoaded();
    void saveSettings(Kind k);
    // The Perfect / OK windows both trainers score with.
    void saveScoreWindows();

    // ---- the clock
    //
    // While a level of this trainer is open, the bar shows the game's own
    // time (frame + 1, the frame GucciBot's recorder files a press under, so
    // a press on the macro's frame sits exactly on its mark). Anywhere else
    // it runs in real time while unpaused, whether or not the page is open,
    // so anything riding it (Video Mode) sees one consistent clock.

    void tick();  // once a frame, from CCDirector::drawScene
    void markPageDrawn(Kind k);
    bool followsLevel(Kind k);
    double levelTimeSec();
    // The macro's last release plus a second, so the last hold is seen out.
    double duration(Kind k);
    void setPaused(Kind k, bool paused);
    // Skim: moving back drops the comparison after the new position.
    void seek(Kind k, double sec);
    // Back to the start, paused, comparison cleared.
    void restart(Kind k);
    // Drops the player's clicks and the score.
    void clearComparison(Kind k);
    // The part of the macro the bar loops over: the looped segments when
    // segment looping is on, otherwise the whole macro.
    void barBounds(Kind k, double& lo, double& hi);

    // ---- the player's input

    // A real jump press/release that reached GD (GJBaseGameLayer::handleButton,
    // button 1, not the bot's). Counts for a trainer whose level is open.
    void onLevelInput(bool down);
    // Space, up or W from the keyboard dispatcher. Counts for a trainer whose
    // page is open with its bar running and no level of its own open (the
    // level path above takes those).
    void onKeyInput(bool down);

    // ---- segments

    struct Segment {
        std::string name;
        double startSec = 0.0;
        double endSec = 0.0;
        std::string note;
    };
    // Parsed from the trainer's segmentsRaw; edit in place, then saveSegments.
    std::vector<Segment>& segments(Kind k);
    void saveSegments(Kind k);
    // Puts the list in time order (after an add) and saves it; the loop
    // stays on the segments it was on.
    void sortSegments(Kind k);
    void removeSegment(Kind k, int index);
    void saveNotes(Kind k);
    // Stretches with the most presses per second that no segment covers yet.
    std::vector<Segment> suggestSegments(Kind k);
    std::string exportCode(Kind k);
    // Adds the code's segments to the list. False (and why) when it doesn't read.
    bool importCode(Kind k, std::string const& code, std::string& error);
    // The looped range in seconds, when the indices name real segments.
    bool loopRange(Kind k, double& startSec, double& endSec);
    // Whether the loop's checkpoint is down in the open level.
    bool loopArmed(Kind k);
    // Times the segment loop sent the player back without a death.
    int cleanLaps(Kind k);

    // ---- stats

    void resetStats(Kind k);

    // ---- the Trainer's macro and track

    // Loads a saved macro by name (stem) into the Trainer with its segments
    // and notes. Keeps the previous one if it doesn't load.
    bool selectMacro(std::string const& stem);
    std::filesystem::path importedTrackPath();
    bool importTrack(std::filesystem::path const& from, std::string& error);
    void removeTrack();

    // ---- the JMF macro's ghost path

    // The bundled JMF macro has no positions in it; this gives its ghost the
    // path of the macro the bot has loaded (recorded or played through).
    void useLoadedPathForJupiter();
    void clearJupiterPath();

} // namespace gucci::trainers
