#pragma once

// Which GD a release's file is for, as plain functions without Geode, so the
// offline tests can check them (tools/run_update_tests.cpp). The update
// checker (core/update_check.cpp) announces a release only when it has a file
// for the GD this build runs on.
//
// Since 2.0.0 each release carries one file per GD, named for it:
// nigelx1.guccibot2208.geode (GD 2.2081) and nigelx1.guccibot2209.geode
// (GD 2.2091), decision 13 (2026-11-17). Releases before 2.0.0 had a single
// nigelx1.guccibot.geode, for GD 2.2081; no build with this check is older
// than them, so a name without its GD counts for neither. A release file must
// keep its GD in its name for the checker to see it.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace gucci::updatefiles {

    // What a file name says about its GD: "2208" or "2.208" for GD 2.2081,
    // "2209" or "2.209" for GD 2.2091, anywhere before ".geode".
    struct GdKeys {
        std::string_view packed;  // "2209"
        std::string_view dotted;  // "2.209"
    };
    inline constexpr GdKeys kGd2081{"2208", "2.208"};
    inline constexpr GdKeys kGd2091{"2209", "2.209"};

    // True for a .geode whose name names this GD (any case).
    inline bool fileIsFor(std::string name, GdKeys const& gd) {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr std::string_view ext = ".geode";
        std::string_view const n(name);
        if (n.size() <= ext.size() || n.substr(n.size() - ext.size()) != ext)
            return false;
        std::string_view const stem = n.substr(0, n.size() - ext.size());
        return stem.find(gd.packed) != std::string_view::npos || stem.find(gd.dotted) != std::string_view::npos;
    }

    // True when one of a release's files is for this GD.
    inline bool releaseHasFileFor(std::vector<std::string> const& files, GdKeys const& gd) {
        return std::any_of(files.begin(), files.end(), [&](std::string const& f) { return fileIsFor(f, gd); });
    }

} // namespace gucci::updatefiles
