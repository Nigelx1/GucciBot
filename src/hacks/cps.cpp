// The CPS counter (see cps.hpp). Silicate's CPSCounter, by peony, GPL-3.0:
// the same queues of press times on the level's clock, the same one-second
// window, the same running maximum.

#include "hacks/cps.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <deque>

using namespace geode::prelude;

namespace gucci::cps {

    namespace {

        struct Counter {
            std::deque<double> times;
            int most = 0;
        };
        Counter g_players[2];

        // GD's level time: it runs with the TPS and the game speed, so a
        // second here is a second of the level, as in Silicate.
        bool levelTime(double& out) {
            auto* gl = GJBaseGameLayer::get();
            if (!gl)
                return false;
            out = gl->m_gameState.m_levelTime;
            return true;
        }

        void prune(Counter& c, double now) {
            // A clock that went back (a reset that didn't reach clear(), a
            // checkpoint) can't leave presses from "the future" counting.
            while (!c.times.empty() && (now - c.times.front() > 1.0 || c.times.front() > now))
                c.times.pop_front();
        }

    } // namespace

    void push(bool player2) {
        double now;
        if (!levelTime(now))
            return;
        auto& c = g_players[player2 ? 1 : 0];
        prune(c, now);
        c.times.push_back(now);
        c.most = std::max(c.most, static_cast<int>(c.times.size()));
    }

    int now(bool player2) {
        auto& c = g_players[player2 ? 1 : 0];
        double t;
        if (levelTime(t))
            prune(c, t);
        return static_cast<int>(c.times.size());
    }

    int most(bool player2) {
        return g_players[player2 ? 1 : 0].most;
    }

    void clear() {
        for (auto& c : g_players) {
            c.times.clear();
            c.most = 0;
        }
    }

} // namespace gucci::cps
