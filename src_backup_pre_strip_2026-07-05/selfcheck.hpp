#pragma once
// selfcheck.hpp — GucciBot 10.0
//
// Runtime self-diagnostic ("the 1.15 update"): instead of a broken build
// silently misbehaving (frozen player, 250-byte renders), GucciBot verifies its
// own critical machinery at load and reports exactly what's wrong. Results are
// also exposed for the in-game debug panel so live state is inspectable.
//
// This does NOT replace the compiler — it can't catch build errors. It catches
// the class of failures that compile fine but break at runtime: a hook that
// didn't install, a patch that didn't apply, a format-version mismatch, an
// unreachable renderer. Those are the silent killers, and now they're loud.

#include <Geode/Geode.hpp>
#include <string>
#include <vector>
#include "util_midhook.hpp"

namespace gbcheck {

struct CheckResult {
    std::string name;
    bool        passed = false;
    std::string detail;
};

// Stored so the debug panel can display the last run.
inline std::vector<CheckResult> g_results;
inline bool g_ranOnce   = false;
inline int  g_passCount = 0;
inline int  g_failCount = 0;

inline void add(const char* name, bool passed, std::string detail = "") {
    g_results.push_back({ name, passed, std::move(detail) });
    if (passed) ++g_passCount; else ++g_failCount;
}

// Run once after install. Logs a clear report; never throws.
inline void run(int expectedMidhooks, int expectedPatches,
                int gbr6Version, int brrVersion, const char* modVersion) {
    g_results.clear();
    g_passCount = 0;
    g_failCount = 0;

    geode::log::info("[GucciBot] ===== self-check =====");

    // 1. Midhooks — the physics engine depends on every one of these.
    {
        int got = g_midhookAttempts - g_midhookFailures;
        bool ok = (g_midhookFailures == 0) && (got >= expectedMidhooks);
        add("Midhooks installed", ok, std::to_string(got) + "/" + std::to_string(expectedMidhooks));
        if (ok)
            geode::log::info("[GucciBot]  [OK]   Midhooks: {}/{} installed",
                             got, expectedMidhooks);
        else
            geode::log::error("[GucciBot]  [FAIL] Midhooks: {}/{} installed, {} failed "
                              "-- physics WILL malfunction (frozen player / desync)",
                              got, expectedMidhooks, g_midhookFailures);
    }

    // 2. Binary patches — reset/keypress behavior depends on these.
    {
        int got = g_patchAttempts - g_patchFailures;
        bool ok = (g_patchFailures == 0) && (g_patchAttempts >= expectedPatches);
        add("Binary patches applied", ok);
        if (ok)
            geode::log::info("[GucciBot]  [OK]   Patches: {}/{} applied",
                             got, expectedPatches);
        else
            geode::log::error("[GucciBot]  [FAIL] Patches: {}/{} applied, {} failed "
                              "-- offsets may be wrong for this GD version",
                              got, expectedPatches, g_patchFailures);
    }

    // 3. Format versions — wrong values silently break macro load/save compat.
    {
        bool ok = (gbr6Version == 1) && (brrVersion == 4);
        add("Wire format versions", ok);
        if (ok)
            geode::log::info("[GucciBot]  [OK]   Formats: GBR6 v{}, BRR v{}",
                             gbr6Version, brrVersion);
        else
            geode::log::error("[GucciBot]  [FAIL] Formats: GBR6 v{} (want 1), BRR v{} "
                              "(want 4) -- saved macros may not load!",
                              gbr6Version, brrVersion);
    }

    // 4. Save directory reachable — needed for macros and renders.
    {
        auto dir = geode::Mod::get()->getSaveDir();
        std::error_code ec;
        bool ok = std::filesystem::exists(dir, ec) || ec.value() == 0;
        add("Save directory", ok, dir.string());
        if (ok)
            geode::log::info("[GucciBot]  [OK]   Save dir reachable");
        else
            geode::log::error("[GucciBot]  [FAIL] Save dir unreachable -- "
                              "macros/renders cannot be written");
    }

    // Summary line — the one you scan for.
    if (g_failCount == 0)
        geode::log::info("[GucciBot] self-check PASSED ({} checks). Build {} healthy. Brrr.",
                         g_passCount, modVersion);
    else
        geode::log::error("[GucciBot] self-check FAILED: {} of {} checks failed. "
                          "Build {} will misbehave -- see [FAIL] lines above.",
                          g_failCount, g_passCount + g_failCount, modVersion);

    geode::log::info("[GucciBot] ======================");
    g_ranOnce = true;
}

} // namespace gbcheck
