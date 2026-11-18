#pragma once

// The Trainer's lane: the trainers' click bar redrawn as an osu!taiko-style
// lane (Juice's design, 2026-11-17). The macro's clicks are notes scrolling
// right to left into a fixed hit marker near the left end; a note's look says
// how tight Calculate counted its window (a small red note for 1 frame, an
// orange ring round a red center for 2, a big yellow one for 3, a wider one
// past that), a hold whose release matters is a slider with a window mark at
// each end, and every press you make is judged on the spot (PERFECT, EARLY
// 1f, LATE 2f, MISS) with the hit marker pulsing in the verdict's color.
//
// This half is the state: the notes built from the trainer's macro, which of
// them you've answered and how, the verdict to show, and the lane's settings.
// The drawing is ui/pages/trainer_lane.cpp. trainers/trainer_core feeds it
// every press and release the bar takes, and every rewind.
//
// The old bar stays, as "Classic" (Settings: style).

#include "trainers/lane_rules.hpp"
#include "trainers/trainer_core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gucci::trainers::lane {

    enum class Style { Lane = 0, Classic = 1 };
    // Auto (the default): Top or Bottom, whichever band the player is
    // farther from, decided again at each gamemode portal or when the
    // player comes near the lane's band.
    enum class Place { Top = 0, Bottom = 1, Auto = 2 };

    // Shared by both trainers. Saved as trainer_lane_*.
    struct Settings {
        Style style = Style::Lane;
        // Ticks of lead: how far ahead of the hit marker the lane shows, at
        // the macro's tick rate (or the game's, in the level).
        int leadTicks = 300;
        float size = 1.f;          // the lane's height: 1x = 12% of the screen's height
        bool inLevel = true;       // drawn over the trainer's level while you play it
        float levelWidth = 0.64f;  // of the screen's width, in the level
        Place place = Place::Auto;  // in the level
        bool judgments = true;     // the PERFECT / EARLY 1f words
    };
    Settings& settings();
    void saveSettings();

    // One note: one of the macro's clicks, in press order.
    struct Note {
        double pressSec = 0.0;
        double releaseSec = 0.0;
        lanerules::Tier pressTier = lanerules::Tier::Unknown;
        lanerules::Tier releaseTier = lanerules::Tier::Unknown;
        lanerules::Allow pressAllow;
        lanerules::Allow releaseAllow;
        float pressWindow = 0.f;    // ticks, for the tooltip; 0 = not counted
        float releaseWindow = 0.f;
        // What its tail is: none (a tap), a slider (the release is timed), a
        // plain hold (long, its release not counted), a free hold (counted,
        // and any release lived).
        enum class Tail : uint8_t { None, Slider, Hold, FreeHold } tail = Tail::None;
        bool player2 = false;
    };

    // How one note went.
    struct Result {
        bool pressed = false;
        bool released = false;
        bool missed = false;         // passed the hit marker unanswered
        bool releaseMissed = false;  // a slider let go of with no release judged
        int8_t pressDelta = 0;
        int8_t releaseDelta = 0;
        lanerules::Verdict pressVerdict = lanerules::Verdict::Missed;
        lanerules::Verdict releaseVerdict = lanerules::Verdict::Missed;
    };

    struct Flash {
        std::string text;
        lanerules::Verdict verdict = lanerules::Verdict::Perfect;
        lanerules::Tier tier = lanerules::Tier::Unknown;
        double atReal = -100.0;  // steady seconds it happened
        bool release = false;
    };

    struct Tally {
        int perfect = 0;  // on the macro's frame
        int inside = 0;   // off it, inside the window
        int outside = 0;  // outside the window (or the OK range)
        int missed = 0;   // passed unanswered
    };

    struct View {
        std::vector<Note> const* notes = nullptr;
        std::vector<Result> const* results = nullptr;
        double maxHoldSec = 0.0;
        int counted = 0;    // notes with a counted press window
        int holding = -1;   // the note held now, or -1
        Flash flash;
        Tally tally;
        double tps = 240.0; // the lane's tick rate now
        // The last judged presses and releases, oldest first (at most 12):
        // ticks off, and how each went.
        struct Recent {
            int8_t delta = 0;
            lanerules::Verdict verdict = lanerules::Verdict::Perfect;
            double atReal = 0.0;
        };
        std::vector<Recent> const* recent = nullptr;
    };
    // The notes for this trainer's macro (built again when the macro
    // changes) and how they went.
    View view(Kind k);

    // The lane's clock: the bar's position as of this moment, not the start
    // of the frame. In the trainer's level, the level's own time read now
    // (the press GD just took shows on the frame it moved the player); away
    // from it, the bar's clock run on to now.
    double now(Kind k);
    // The bar's clock at an earlier steady-clock moment (a key's own time).
    double at(Kind k, double steadySec);

    // ---- fed by trainers/trainer_core
    void onInput(Kind k, bool down, double atSec, double tps);
    void rewind(Kind k, double fromSec);
    void clear(Kind k);
    // Once a frame from the drawing: notes the clock passed unanswered.
    void advance(Kind k, double nowSec);

    // Assistant Access (test launches): make the last verdict this many
    // seconds old, so a shot catches its pop, pulse or fade on purpose.
    // `hold`: keep it that old (the next verdict too) until a call without.
    void ageFlashForTest(Kind k, double ageSec, bool hold);

    // Where the lane sits over the level now (the drawing keeps it; the
    // Assistant Access tool reads it). band: 0 top, 1 bottom; slide: 0 at
    // the top .. 1 at the bottom (mid-slide in between); alpha: the whole
    // lane's opacity (0.3 while a player is inside its band); moves: how
    // many times Auto moved it this launch.
    struct Placement {
        int band = 0;
        float slide = 0.f;
        float alpha = 1.f;
        int moves = 0;
        float y = 0.f, h = 0.f;  // the box's top and height on screen, in pixels
        float floorY = -1.f, ceilY = -1.f;  // GD's ground line and ceiling line on screen (-1: not shown)
        int fill = 0;            // the box's fill alpha before the near-player easing
        bool bottomFits = false; // the bottom band fits wholly under the ground line
    };
    Placement& placement(Kind k);

    // Drawn over the trainer's level this frame (safe mode counts it).
    void markShownInLevel(Kind k);
    bool shownInLevel(Kind k);

    // The steady clock the trainers run on, in seconds.
    double steadyNow();

} // namespace gucci::trainers::lane
