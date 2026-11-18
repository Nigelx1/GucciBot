#pragma once

// The GD version this game is (2026-11-17), for what a macro records it was
// made on. Geode's own getter on both SDKs (Geode 5 for GD 2.2081, Geode 6
// for 2.2091). It reads the game itself, but its answer is not always in the
// "2.2081" form (on Android, Geode 5 hands back a bare version code for a GD
// it has no name for, and an old launcher gives nothing), and a mod only ever loads into the GD version it was built
// for, so anything else falls back to the version this build targets.

#include <Geode/loader/Loader.hpp>

#include <cctype>
#include <string>

namespace gucci {

    inline bool looksLikeGdVersion(std::string const& v) {
        // "2." and then three or four digits: 2.206, 2.2081, 2.2091.
        if (v.size() < 5 || v.size() > 6 || v[0] != '2' || v[1] != '.')
            return false;
        for (size_t i = 2; i < v.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(v[i])))
                return false;
        return true;
    }

    inline std::string const& currentGdVersion() {
        static std::string const version = [] {
            std::string v = geode::Loader::get()->getGameVersion();
            return looksLikeGdVersion(v) ? v : std::string(GEODE_GD_VERSION_STRING);
        }();
        return version;
    }

} // namespace gucci
