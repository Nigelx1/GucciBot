#pragma once

// Hitborders (Nigel, 2026-11-02; the idea is NaN GD's "accurate gaps"):
// every hitbox grown by the player's own, so the player is a point and the
// gaps it has are what is drawn. Red is where the player's center dies, blue
// where its box touches something solid (lands, slides, snaps), green orbs,
// pads, portals and touch triggers. The player's path and clicks are drawn in
// yellow behind it (the player as a dot too, if asked). Drawn the way NaN GD
// draws his (2026-11-10, Nigel: "jus do it like him"): used orbs, pads and
// portals stay drawn, the path is half as thick as the outlines, no dot by
// default, and the level darkened under the borders so they stand out.
//
// The shapes come from tools/hitborder_geom (GD 2.2081's own collision tests,
// read from the binary); hitborders.cpp reads the level for it once a drawn
// frame, for the player's current mode, size, gravity and tilt (both players
// in dual), and draws the result in the object layer just under the icon.
// The Macro page card is ui/pages/hitborders_card.cpp.
//
// A cheat while Show hitborders is on, as Show hitboxes is (hacks/safemode.cpp,
// Nigel, 2026-11-02): the Demonlist's sheet marks live hitboxes Not Allowed.
// Show on death alone isn't one; the sheet allows on-death hitboxes.

#include <Geode/Geode.hpp>

#include "tools/hitborder_geom.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gucci {

    class HitborderOverlay {
    public:
        static HitborderOverlay* get();

        // The colors: the three kinds of region and the player's marks.
        enum Part : int { Lethal, Touch, Special, Marks, PartCount };

        struct Settings {
            bool show = false;      // hack_hitborders
            bool onDeath = false;   // hack_hitborders_death: once the player dies, even with show off
            bool hazards = true;    // spikes, saws, hazard slopes
            bool blocks = true;
            bool exactBlocks = true;  // the red core where the inner box kills; off, the touch zone only
            bool slopes = true;
            bool specials = true;   // orbs, pads, portals, touch triggers
            bool hideUsed = false;  // ... except the ones every player has used (NaN's stay drawn)
            bool ground = true;     // the floor and, in a corridor, the ceiling
            bool useTilt = true;    // rotated objects against the icon's tilt
            bool merged = true;     // one outline around overlapping regions
            bool player2 = true;    // player 2's own borders, as outlines, where they differ from player 1's
            bool point = false;     // the player as a yellow dot (NaN's videos have none)
            bool path = true;       // the player's own path, one point a tick
            int pathTicks = 240;
            bool clicks = true;     // a dot where the player pressed, a ring where it let go
            float lineWidth = 2.f;  // screen points, at any zoom; the path is half as thick
            float fill = 0.5f;      // inside of each region, 0..1 of its color's alpha
            bool darken = true;     // a black layer over the level, under the borders, while they show
            float darkness = 0.6f;  // that layer's opacity, 0..1
            float rgba[PartCount][4] = {};
        };

        static constexpr int kMinPathTicks = 10;
        static constexpr int kMaxPathTicks = 4800;

        // The card edits these directly, then calls save().
        Settings settings;
        static Settings defaults();
        static const char* partLabel(Part part);
        void load();
        void save() const;
        // Show hitborders, flipped by the keybind and saved.
        void toggleShown();

        // From PlayLayer::updateVisibility (hooks/hook_playlayer.cpp), every
        // frame: puts the overlay back on this level if it isn't there. The
        // drawing itself happens in the nodes' visit(), so it also shows (and
        // follows the switches) while the game is paused.
        void draw(PlayLayer* pl);
        void init(PlayLayer* pl);
        void destroy();
        // A new attempt (resetLevel): the path and the clicks start again.
        void clearPath();

        // For the hooks in hitborders.cpp.
        void sampleTick(GJBaseGameLayer* layer);                  // once per physics tick
        void onButton(PlayerObject* player, bool pressed);        // a jump press or release
        void paint(GJBaseGameLayer* layer);                       // once per drawn frame
        bool drawMerged();                                        // the borders' one-pass outline
        bool attachedTo(GJBaseGameLayer* layer) const;
        // The merged pass's texture and framebuffer, freed while the GL
        // context is still there (the app going to the background); the next
        // frame makes them again.
        void releaseGpu();

        // The accuracy audit, for Assistant Access (gucci_hitborders): over
        // every collision pass of a real player, whether GD's own pass killed
        // it, against whether its center, where the pass left it, sat in a
        // red region the overlay draws (every kind on, blocks Exact, the
        // icon's tilt used). Off until asked for, whether or not the borders
        // are shown; it only reads.
        struct Audit {
            bool on = false;
            uint64_t passes = 0;       // collision passes watched
            uint64_t deaths = 0;       // ... in which GD called destroyPlayer
            uint64_t agreed = 0;       // deaths with the center in red
            uint64_t missed = 0;       // deaths with the center outside every red region
            uint64_t falseAlarms = 0;  // passes lived through with the center in red
            std::deque<std::string> notes;  // the latest disagreements, oldest first
        };
        Audit const& audit() const;
        void setAudit(bool on);  // turning it on starts the counts again
        // For the audit's hooks: around one collision pass, and GD's
        // destroyPlayer inside it.
        bool auditBefore(GJBaseGameLayer* layer, PlayerObject* player);
        void auditAfter(GJBaseGameLayer* layer, PlayerObject* player);
        void auditDeath(GJBaseGameLayer* layer, PlayerObject* player, GameObject* by);
        // Player 1 and the regions within `radius` of it right now (every
        // kind on, blocks Exact), for gucci_hitborders.
        matjson::Value describe(float radius);

    private:
        HitborderOverlay();
        bool shownFor(GJBaseGameLayer* layer, PlayerObject* p1, PlayerObject* p2) const;
    };

    // The overlay's own reading of the level, for the accuracy probe
    // (hacks/hitborder_probe.cpp, gucci_hitborder_probe), so what it tests is
    // exactly what is drawn.
    struct HitborderRead {
        enum class Type : uint8_t { Hazard, Special, Block, Slope };
        Type type = Type::Hazard;
        hitborder::Collider collider;  // a hazard's, or an orb's, pad's, portal's or trigger's
        hitborder::Block block;
        hitborder::Slope slope;
    };
    // A player as paint() reads one: as the next collision pass will test it.
    hitborder::Player hitborderReadPlayer(PlayerObject* p);
    // An object as paint() reads it; false for one GD's collision pass skips
    // or the overlay doesn't draw (spent, a trigger nothing touches, a player).
    bool hitborderReadObject(GJBaseGameLayer* pl, GameObject* object, HitborderRead& out);
    // Its regions for `p`, every kind on (blocks Exact when opt.core), as the
    // audit and gucci_hitborders have them.
    void hitborderRegions(HitborderRead const& read, hitborder::Player const& p, hitborder::Options const& opt,
                          std::vector<hitborder::Shape>& out);

} // namespace gucci
