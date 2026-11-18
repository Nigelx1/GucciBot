#pragma once

// The queues (Juice's idea, relayed by Nigel 2026-11-17: "enter IDs into the
// queue and just leave ur pc on and get working bots from them, and doing the
// same for FW counters"). Two of them, one running at a time, both playing
// the game for real, one level after another:
//
// - Pathfinder queue (Windows on GD 2.2081, where the trigger model runs):
//   level IDs. Each level is downloaded if need be (GD's own download, as
//   clicking it does), opened, and the Pathfinder (the search) plays it from
//   the start for up to the time limit. A win is saved as "<level> (<id>)",
//   otherwise the furthest attempt as "<level> (<id>) furthest"; with "Count
//   frame windows when beaten", Calculate then counts the win's windows.
// - Frame-window queue (wherever Calculate runs): saved macros. Each one's
//   level (from its name's "(<id>)", or the level name it was recorded on)
//   is opened, Calculate counts it, and its windows are saved with it, as a
//   Calculate run always saves them.
//
// Nothing new plays the game: it drives the Pathfinder's Start and Stop
// (absense/glue.hpp), the level opening of Assistant Access's open_level and
// Calculate's own run (GucciEngine::analyzeFrameWindows). Safe mode is on for
// the whole queue (safemode::setQueueHold); Legit only refuses to start one.
// The queue and its results are saved to the mod's save folder after every
// step (queue_pathfinder.txt, queue_frame_windows.txt; tools/queue_core.hpp
// has the format), so the next launch shows them and offers to continue.

#include "tools/queue_core.hpp"

#include <matjson.hpp>

#include <string>
#include <vector>

namespace gucci::queue {

    // The Pathfinder queue runs on this build (GB_NATIVE_ENGINE).
    bool pathfinderAvailable();

    State const& state(Kind kind);
    bool running(Kind kind);
    bool anyRunning();
    // One line on what it is doing now ("Downloading level 3 of 5..."), or "".
    std::string activity(Kind kind);
    // The level working on now (index into state().items), or -1.
    int currentIndex(Kind kind);
    // Seconds spent on the current item so far.
    double currentSeconds(Kind kind);

    // Pathfinder queue: adds the IDs in `text` (queue_core parseLevelIds).
    // Returns how many were added; `bad` gets the pieces that aren't IDs.
    int addLevelIds(std::string const& text, std::vector<std::string>* bad = nullptr);
    // Frame-window queue: adds saved macros by name (no extension).
    int addMacros(std::vector<std::string> const& names);
    void setLimitMinutes(int minutes);
    void setCountWindows(bool on);
    // Takes out the item at `index` (not the one running).
    void remove(Kind kind, int index);
    // Empties the queue (not while it runs).
    void clear(Kind kind);
    // Takes out the finished ones, keeps what's left to do.
    void clearFinished(Kind kind);

    // "" when it started, otherwise why not (Legit only, the other queue, a
    // render, nothing to do...).
    std::string start(Kind kind);
    // Stops after putting the game back (the run going is stopped; the level
    // it was on goes back in line).
    void stop(Kind kind);

    // A queue the last launch left with levels to go: the page asks
    // "Continue the queue (3 left)?" until answered.
    bool continueOffered(Kind kind);
    void answerContinue(Kind kind, bool yes);

    // For Assistant Access (gucci_queue_read).
    matjson::Value toJson(Kind kind);

}  // namespace gucci::queue
