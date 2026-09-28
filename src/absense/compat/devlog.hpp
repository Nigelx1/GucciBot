#pragma once

// Stands in for Absense's shared/devlog.hpp. Absense has a developer log with
// categories switched on from its UI; GucciBot has Geode's log, which is kept
// on disk (<GD>/geode/logs/). The pathfinder's category goes there, tagged
// [AbsPF], because that is how a search gets diagnosed here. The others --
// per-line trajectory profiling, the World's self-checks -- stay off; they
// fire hundreds of times a second.

#include <Geode/Geode.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace devlog {

    // The port renames Absense's Pathfinder class, and this category with it.
    enum class Cat { System, Trajectory, AbsensePathfinder, World, Replay, Render, Editor, Mcp, Gpu };

    inline bool on(Cat c) { return c == Cat::AbsensePathfinder; }

    inline void logf(Cat c, char const* fmt, ...) {
        if (!on(c))
            return;
        char buf[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        geode::log::info("[AbsPF] {}", buf);
    }

    inline void log(Cat c, std::string const& msg) {
        if (on(c))
            geode::log::info("[AbsPF] {}", msg);
    }

}  // namespace devlog
