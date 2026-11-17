#pragma once

// Geode 5 (GD 2.2081) and Geode 6 (GD 2.2091) from one source tree.
//
// GucciBot builds twice from the same code (Nigel, 2026-11-16): against
// Geode 5 for GD 2.2081, which is what 2.0 ships, and against Geode 6 for GD
// 2.2091. A .geode loads on only one Geode major, so each SDK makes its own
// file (CMakeLists.txt). Everything that differs between the two Geode APIs
// goes through this header, so the rest of the code is written once:
//
// - GB_GEODE6: 1 when building against Geode 6 (GD 2.2091 or newer).
//   Both SDKs define GEODE_COMP_GD_VERSION (22081 on Geode 5, 22091 on 6).
// - gd::string, gd::vector, gd::map, ...: Geode 6 removed them (GD's own
//   types are plain std:: ones there); here they are aliases of std::.
// - Paths: Geode 6's folder getters, file helpers and file picker use
//   asp::fs::Path instead of std::filesystem::path. GucciBot keeps
//   std::filesystem everywhere and converts at the edge:
//     gucci::stdPath(x)    anything Geode hands back -> std::filesystem::path
//     gucci::geodePath(p)  std::filesystem::path -> what Geode's helpers take
//     gucci::pathToString  a path as UTF-8 (Geode 6 dropped pathToString)
// - sha256 (utils/hash.hpp on Geode 5, utils/crypto.hpp on 6).
// - ZipUtils::compressString (Geode 6 added a compression level).
//
// Keep this header small: an API difference gets one helper here, not an
// #if at every call site.

#include <Geode/platform/cplatform.h>
#include <Geode/utils/string.hpp>

#include <filesystem>
#include <string>

#if !defined(GEODE_COMP_GD_VERSION)
#error "GEODE_COMP_GD_VERSION is not defined: build GucciBot through the Geode SDK's CMake"
#endif

#if GEODE_COMP_GD_VERSION >= 22090
#define GB_GEODE6 1
#else
#define GB_GEODE6 0
#endif

#if GB_GEODE6
#include <Geode/utils/crypto.hpp>
#include <asp/fs/Path.hpp>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Geode 6 removed gd::string and the other gd:: containers: on GD 2.2091 they
// are the standard ones. The aliases keep code shared with the Geode 5 build
// (where gd:: is GD's own ABI) compiling unchanged.
namespace gd {
    using std::string;
    template <class T, class A = std::allocator<T>>
    using vector = std::vector<T, A>;
    template <class K, class V, class C = std::less<K>, class A = std::allocator<std::pair<K const, V>>>
    using map = std::map<K, V, C, A>;
    template <class K, class C = std::less<K>, class A = std::allocator<K>>
    using set = std::set<K, C, A>;
    template <class K, class V, class H = std::hash<K>, class E = std::equal_to<K>,
              class A = std::allocator<std::pair<K const, V>>>
    using unordered_map = std::unordered_map<K, V, H, E, A>;
    template <class K, class H = std::hash<K>, class E = std::equal_to<K>, class A = std::allocator<K>>
    using unordered_set = std::unordered_set<K, H, E, A>;
}
#else
#include <Geode/utils/hash.hpp>
#endif

namespace gucci {

    // A path as UTF-8 text (Geode 5's utils::string::pathToString, which
    // Geode 6 removed; this is what it did).
    inline std::string pathToString(std::filesystem::path const& path) {
#if defined(GEODE_IS_WINDOWS)
        return geode::utils::string::wideToUtf8(path.native());
#else
        return path.string();
#endif
    }

#if GB_GEODE6
    inline std::filesystem::path stdPath(asp::fs::PathView path) {
        return std::filesystem::path(path.native());
    }
    inline std::filesystem::path stdPath(asp::fs::Path const& path) {
        return std::filesystem::path(path.native());
    }
    inline std::filesystem::path stdPath(std::filesystem::path path) {
        return path;
    }
    inline asp::fs::Path geodePath(std::filesystem::path const& path) {
        return asp::fs::Path::fromStdPath(path);
    }
    inline std::string pathToString(asp::fs::PathView path) {
        return path.string();
    }
    inline std::string pathToString(asp::fs::Path const& path) {
        return path.string();
    }
#else
    inline std::filesystem::path stdPath(std::filesystem::path path) {
        return path;
    }
    inline std::filesystem::path const& geodePath(std::filesystem::path const& path) {
        return path;
    }
#endif

    // A level string packed the way GD saves levels (gzip, base64, no XOR).
    // Geode 6 added a compression level to ZipUtils::compressString, and its
    // header and the 2.2091 bindings disagree on the order of the last two
    // arguments, so both get 9 (zlib's best): with encryption off the key is
    // never used. I'm not sure which order GD itself has; this is the one call
    // (Assistant Access's level loader, test launches only).
    template <class Zip>
    std::string compressLevelString(std::string const& raw) {
#if GB_GEODE6
        return std::string(Zip::compressString(raw, false, 9, 9));
#else
        return std::string(Zip::compressString(gd::string(raw), false, 0));
#endif
    }

} // namespace gucci
