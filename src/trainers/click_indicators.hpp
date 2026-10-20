#pragma once

// Click Indicators: where the macro clicks, shown in the level and down a
// rhythm lane at the edge of the screen, instead of clicking for you. A
// practice tool: it never touches input. Its own feature since 2026-10-19
// (Nigel: not tied to the trainers' click bar in the menu), with the options
// of Bogdaner09's Click Indicators mod (github.com/Bogdaner09/mod, credited on
// the Credits page). His source carries no licence, so nothing here is his
// code: it is written fresh from what his settings describe.
//
// Which macro: in a trainer's level (JMF's, or the Trainer's), that trainer's
// macro; anywhere else, the macro loaded on the Macro page when it belongs to
// the open level (or doesn't say which level it belongs to). Shown only while
// GucciBot is neither playing nor recording.

#include <array>

class PlayLayer;

namespace gucci::clickind {

    struct Settings {
        bool enabled = true;

        // ---- in the level
        int offsetMs = 0;          // every indicator later (+) or earlier (-)
        float lookAhead = 2.f;     // seconds of upcoming presses
        int maxNotes = 10;         // in the level and in the lane
        std::array<float, 3> color{0.208f, 0.941f, 0.478f};  // #35F07A
        int transparency = 80;     // percent; 0 solid, 100 invisible
        std::array<float, 3> p2Color{1.f, 0.388f, 0.788f};   // #FF63C9
        int p2Transparency = 80;
        float lineWidth = 4.f;     // a tap's line, in level units (a block is 30)
        int tapMs = 45;            // shorter presses draw as taps, longer as holds
        bool edgeMarker = true;    // a solid line on the press edge of a hold
        bool holdTracks = true;    // a passed press edge follows you: what's left is the hold left
        bool pressBadge = true;    // a hollow square on the press edge, level with you
        float badgeSize = 6.f;     // screen units, whatever the zoom
        bool playerSquare = true;  // the matching square on your hitbox
        bool playerLine = true;    // a line through your icon
        std::array<float, 3> playerColor{1.f, 1.f, 1.f};
        int playerTransparency = 45;

        // ---- the rhythm lane
        bool lane = true;
        int laneSide = 0;          // 0 left, 1 right, 2 middle (flat across the top)
        float midLaneHit = 0.38f;  // middle: the hit line, as a fraction of the width in from its edge
        float laneWindow = 1.2f;   // seconds from the far end to the hit line
        float laneScale = 1.f;
        float laneDepth = 1.15f;   // how hard the slabs taper away (perspective)
        int noteShape = 0;         // 0 slabs (perspective), 1 circles (flat, osu!mania)
        bool laneFlip = false;     // notes travel the other way, into the far end
        int laneTransparency = 25;
        bool p2Lane = true;        // dual macros: a column per player

        // ---- scoring
        bool accuracy = true;
        int perfectMs = 8;
        int okMs = 33;
    };

    Settings& settings();
    void load();  // once; settings() calls it
    void save();

    // Once a frame (the trainers' clock, CCDirector::drawScene): what is shown
    // in the level, and the misses the scoring counts.
    void frame();
    // Once a frame from the menu's ImGui pass: the rhythm lane and the verdict.
    void drawOverlay();
    // A real click in the level (not the bot's), for the scoring.
    void onRealClick(bool player2, bool down);

    struct Tally {
        int perfect = 0;
        int ok = 0;
        int miss = 0;
    };
    Tally tally();
    void resetTally();
    // Showing in the level this frame. Safe mode counts it as a cheat (Nigel,
    // 2026-10-19: it takes the memory out of memory levels).
    bool active();
    // What the indicators are showing right now, for the card: "" when nothing.
    const char* showing();

} // namespace gucci::clickind
