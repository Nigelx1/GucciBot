#pragma once

// Macro problem check -- the "Problems" half of Absense's macro editor
// (replay/edit_core.cpp, buildModel's flags, by Absent), rewritten over
// GucciBot's actions. Pure: no game, Geode or ImGui, so tools/
// run_macrocheck_tests.cpp runs it outside the game, like edit_core.
//
// Each button of each player is a lane. Walking the actions in order:
//   - a press in a lane already held is a DoublePress,
//   - a release in a lane not held is an OrphanRelease -- unless a death or
//     restart cut that lane's hold, when it is only noted (Absense's
//     ReleaseAfterReset: the recorder writes those),
//   - an action at a lower frame than the one before is Unsorted,
//   - an unknown type, or a TPS change that is not a positive finite
//     number, is a BadValue.
// Holds still open at the end, and holds a death or restart cut, are counted
// as information, not problems.
//
// Not carried over: Absense's "action on a reset's tick" warning. Its
// reasoning is Silicate's playback order on that tick, which GucciBot's
// (inputs looked up a frame ahead) does not share, and whether GucciBot drops
// such an input is not established.

#include "core/action_types.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gucci::macrocheck {

    enum class Problem : uint8_t { DoublePress, OrphanRelease, Unsorted, BadValue };

    inline char const* name(Problem p) {
        switch (p) {
            case Problem::DoublePress: return "pressed again before a release";
            case Problem::OrphanRelease: return "release with no press before it";
            case Problem::Unsorted: return "earlier frame than the action before it";
            case Problem::BadValue: return "invalid type or TPS value";
        }
        return "?";
    }

    struct Finding {
        uint32_t index = 0;  // position in the action list
        uint32_t frame = 0;
        Problem problem = Problem::BadValue;
        int lane = -1;       // p2 * 3 + button - 1 for inputs, -1 otherwise
    };

    struct Report {
        std::vector<Finding> findings;
        uint32_t clicks = 0;             // complete press..release pairs
        uint32_t heldToEnd = 0;          // holds still open at the end
        uint32_t cutByReset = 0;         // holds a death or restart ended
        uint32_t releaseAfterReset = 0;  // the releases of those, when present
        bool ok() const { return findings.empty(); }
    };

    inline int laneOf(gb::Action const& a) {
        int const b = static_cast<int>(a.m_type);
        if (b < 1 || b > 3)
            return -1;
        return (a.m_player2 ? 3 : 0) + b - 1;
    }

    inline bool isReset(gb::Action const& a) {
        return a.m_type == gb::ActionType::Death || a.m_type == gb::ActionType::Restart ||
               a.m_type == gb::ActionType::RestartFull;
    }

    inline bool validType(gb::Action const& a) {
        switch (a.m_type) {
            case gb::ActionType::Jump:
            case gb::ActionType::Left:
            case gb::ActionType::Right:
            case gb::ActionType::Death:
            case gb::ActionType::Restart:
            case gb::ActionType::RestartFull:
                return true;
            case gb::ActionType::TPS:
                return std::isfinite(a.m_tps) && a.m_tps > 0.0;
        }
        return false;
    }

    inline Report check(std::vector<gb::Action> const& actions) {
        Report r;
        std::array<bool, 6> held{};
        std::array<bool, 6> cut{};
        uint32_t lastFrame = 0;

        for (uint32_t i = 0; i < actions.size(); i++) {
            auto const& a = actions[i];
            if (i > 0 && a.m_frame < lastFrame)
                r.findings.push_back({i, a.m_frame, Problem::Unsorted, laneOf(a)});
            lastFrame = a.m_frame;

            if (!validType(a)) {
                r.findings.push_back({i, a.m_frame, Problem::BadValue, laneOf(a)});
                continue;
            }

            if (isReset(a)) {
                for (int l = 0; l < 6; l++) {
                    if (held[l]) {
                        r.cutByReset++;
                        cut[l] = true;
                    }
                    held[l] = false;
                }
                continue;
            }

            int const lane = laneOf(a);
            if (lane < 0)
                continue;  // TPS

            if (a.m_holding) {
                if (held[lane])
                    r.findings.push_back({i, a.m_frame, Problem::DoublePress, lane});
                held[lane] = true;
                cut[lane] = false;
            } else if (held[lane]) {
                held[lane] = false;
                r.clicks++;
            } else if (cut[lane]) {
                cut[lane] = false;
                r.releaseAfterReset++;
            } else {
                r.findings.push_back({i, a.m_frame, Problem::OrphanRelease, lane});
            }
        }

        for (bool h : held)
            r.heldToEnd += h ? 1 : 0;
        return r;
    }

} // namespace gucci::macrocheck
