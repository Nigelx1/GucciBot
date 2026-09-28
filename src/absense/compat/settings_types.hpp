#pragma once

// Absense's settings for the parts of it GucciBot carries -- its trajectory
// (the look-ahead the pathfinder decides with), its pathfinder, and the hitbox
// colours the trajectory draws with. Copied field-for-field from Absense's
// settings.hpp (2026-09-24 drop) so the ported code reads them unchanged.
// analysis/ac/shim.hpp hangs them on SLSettings under Absense's own names.

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace absense_settings {

    struct TrajectorySettings {
        struct State {
            bool enabled = false;
            std::array<float, 4> colors = {0.0, 1.0, 0.0, 1.0};
        };

        enum Mode {
            Hold = 0x1,
            Swift = 0x2,
            Release = 0x4,

            Left = 0x8,
            Right = 0x10,

            Player1 = 0x20,
            Player2 = 0x40,

            FollowPlayer = 0x80,
            FollowOpposite = 0x100,

            Platformer = 0x200,

            Double = 0x400,
            Tap = 0x800,
            DoubleHeld = 0x1000,
        };

        bool enabled = false;
        double width = 0.5;
        double length = 1.0;
        double refreshRate = 0.0;

        int updatesPerSecond() const {
            return refreshRate > 0.0 ? (int)std::lround(std::min(refreshRate, 60.0)) : 0;
        }

        static constexpr std::array<float, 4> HOLD_COLOR = {0.25f, 1.0f, 0.4f, 1.0f};
        static constexpr std::array<float, 4> SWIFT_COLOR = {1.0f, 0.85f, 0.2f, 1.0f};
        static constexpr std::array<float, 4> RELEASE_COLOR = {1.0f, 0.3f, 0.3f, 1.0f};
        static constexpr std::array<float, 4> DOUBLE_COLOR = {0.35f, 0.8f, 1.0f, 1.0f};
        static constexpr std::array<float, 4> TAP_COLOR = {1.0f, 0.55f, 0.15f, 1.0f};
        static constexpr std::array<float, 4> DOUBLE_HELD_COLOR = {0.7f, 0.4f, 1.0f, 1.0f};

        std::unordered_map<int, State> categories = {
            {Mode::Hold, {true, HOLD_COLOR}},
            {Mode::Release, {true, RELEASE_COLOR}},
            {Mode::Tap, {false, TAP_COLOR}},
            {Mode::Swift, {false, SWIFT_COLOR}},
            {Mode::Double, {false, DOUBLE_COLOR}},
            {Mode::DoubleHeld, {false, DOUBLE_HELD_COLOR}},
        };
    };

    struct PathfinderSettings {
        double lookahead = 0.75;      // seconds simulated ahead per decision
        int effort = 1;               // 0 low, 1 normal, 2 high: how many ideas per decision
        bool doubleClicks = true;     // try two clicks in one tick as well
        double frameBudgetMs = 33.0;  // time thinking per frame while the game is stopped
        uint32_t anchorEvery = 10;    // ticks between kept game states
        uint32_t maxAnchors = 6000;   // clamped to 8..64 in pathfinder.cpp
        bool saveWhenDone = true;     // write the replay when the level is done
        bool holdInputs = true;       // allow holds (off: taps and doubles only)
        double maxBackSeconds = 3.0;  // how far back a dead end may send the search
        bool smooth = true;           // keep the frame rate while it thinks
        bool learn = true;            // remember what worked, per level and across them
        // Absense scores ideas on the graphics card in a separate program
        // (gpuapp) first. GucciBot does not ship that program, so this is off
        // and every idea goes through the game's own physics.
        bool gpu = false;
        uint32_t gpuScripts = 8000;
    };

    struct HitboxSettings {
        bool enabled = true;
        double width = 0.5;
        bool trailEnabled = false;
        bool playerAbove = true;
        bool rotatedAbove = true;
        float optimizationFactor = 2.f;
        bool trailShowP1 = true;
        bool trailShowP2 = true;

        struct TrailColors {
            std::array<float, 4> player = {0.0f, 1.0f, 1.0f, 1.0f};
            std::array<float, 4> inner = {1.0f, 1.0f, 0.0f, 1.0f};
            std::array<float, 4> rotated = {0.5f, 1.0f, 1.0f, 1.0f};
            std::array<float, 4> circle = {0.0f, 1.0f, 1.0f, 1.0f};
        };
        TrailColors trailP2Colors;

        enum Type {
            Player,
            PlayerRotated,
            PlayerInner,
            PlayerCircle,

            Solid,
            Hazard,
            Passable,
            Interactable,
            InteractableActive,

            PlayerDead
        };

        struct HBState {
            bool enabled = true;
            double fillOpacity = 0.00;
            std::array<float, 4> colors;
        };

        std::unordered_map<int, HBState> categories = {
            {Type::Player, {true, 0.0, {1.0, 0.0, 0.0, 1.0}}},
            {Type::PlayerInner, {true, 0.0, {0.0, 0.0, 1.0, 1.0}}},
            {Type::PlayerRotated, {true, 0.0, {0.5, 0.0, 0.0, 1.0}}},
            {Type::PlayerCircle, {true, 0.0, {1.0, 0.0, 0.0, 1.0}}},
            {Type::Solid, {true, 0.0, {0.0, 0.0, 1.0, 1.0}}},
            {Type::Hazard, {true, 0.0, {1.0, 0.0, 0.0, 1.0}}},
            {Type::Passable, {true, 0.0, {0.0, 1.0, 1.0, 1.0}}},
            {Type::Interactable, {true, 0.0, {1.0, 1.0, 0.0, 1.0}}},
            {Type::InteractableActive, {true, 0.0, {0.2, 1.0, 0.0, 1.0}}},
            {Type::PlayerDead, {true, 0.0, {1.0, 0.4, 0.0, 1.0}}},
        };
    };

}  // namespace absense_settings
