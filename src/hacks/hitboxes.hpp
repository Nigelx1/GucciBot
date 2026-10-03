#pragma once

// Show Hitboxes: collision boxes drawn over the level, in normal play,
// practice, recording and playback alike. A port of Silicate's hitbox drawing
// (silicate/src/assist/hitboxes.cpp, by peony, GPL-3.0 - GucciBot's licence
// too); the drawing, the trail and the hooks that drive them are in
// hitboxes.cpp, the Hacks page card in ui/pages/hitboxes_card.cpp.
//
// The switches are the engine's (GucciEngine: showHitboxes, hitboxOnDeath,
// hitboxTrail, hitboxTrailLength) and are read live every frame, so the
// keybind and the card both act at once. The look (line width, fill, which
// parts are drawn and in which colour) is kept here.

#include <Geode/Geode.hpp>
using namespace geode::prelude;

namespace gucci {

    class HitboxOverlay {
    public:
        static HitboxOverlay* get();

        // What can be drawn, in the card's order. The saved names follow it.
        enum Part : int {
            Player,              // the player's box
            PlayerRotated,       // the same box turned with the icon
            PlayerInner,         // the small inner box
            PlayerCircle,        // the player's circle
            PlayerDead,          // drawn over the box once the player has died
            Solid,
            Passable,
            Hazard,
            Interactable,        // orbs, pads, portals, touch triggers
            InteractableActive,  // one of those the player is touching now
            PartCount
        };

        struct PartStyle {
            bool on = true;
            float rgba[4] = {1.f, 1.f, 1.f, 1.f};
        };

        struct Style {
            float lineWidth = 0.5f;  // in screen points, at any zoom
            float fill = 0.f;        // inside of each box: 0..1 of its colour's alpha
            PartStyle parts[PartCount];
        };

        // Trail length limits, in ticks (the engine's hitboxTrailLength).
        static constexpr int kMinTrailTicks = 1;
        static constexpr int kMaxTrailTicks = 4800;

        // The card edits this directly, then calls saveStyle().
        Style style;
        static Style defaults();  // Silicate's colours
        static const char* partLabel(Part part);
        static const char* partHint(Part part);  // may be null

        // At mod load: the engine's hitbox keys its own loader does not read
        // (hack_hitbox_death, hack_hitbox_trail, hack_hitbox_trail_len) and
        // the style. hack_hitboxes is engine_core's.
        void loadSettings();
        void saveStyle() const;

        // From PlayLayer::updateVisibility (hooks/hook_playlayer.cpp), every
        // frame: puts the overlay back on this level if it is not there. The
        // drawing itself happens in the overlay node's visit(), just before
        // the frame is drawn, so it also shows - and follows the switches -
        // while the game is paused.
        void draw(PlayLayer* pl);
        // Attach the overlay to this level (a no-op when it already is).
        void init(PlayLayer* pl);
        // Take it off the level and free it; the trail goes with it.
        void destroy();
        // A new attempt (resetLevel, an editor playtest).
        void clearTrail();

        // For the hooks in hitboxes.cpp.
        void sampleTick(GJBaseGameLayer* layer);  // once per physics tick
        void paint(GJBaseGameLayer* layer);       // once per drawn frame
        void raiseGameUI(PlayLayer* pl);          // keep the progress bar and % above it
        void raiseAbove(cocos2d::CCNode* node);
        bool attachedTo(cocos2d::CCNode* layer) const;

    private:
        HitboxOverlay();
    };

} // namespace gucci
