#pragma once

#include "core/bot_switch.hpp"
#include "core/platform.hpp"
#include "core/standdown.hpp"

#include <Geode/Geode.hpp>
#if GB_NATIVE_ENGINE
#include <safetyhook.hpp>
#endif
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gucci {

    // The self-check reads these on every platform; off Windows they stay 0
    // (core/platform.hpp: no midhooks or patches there).
    inline int g_midhookAttempts = 0;
    inline int g_midhookFailures = 0;
    inline int g_patchAttempts = 0;
    inline int g_patchFailures = 0;

#if GB_NATIVE_ENGINE
    // Every midhook and byte patch GucciBot asks for is written down here,
    // whether or not it went in, so the master switch (core/bot_switch.hpp)
    // can take them all out and put them back. A deque, not a vector: a
    // placed MidHook is never moved once it is live.
    //
    // Launched with the switch saved off, nothing is placed at all -- the
    // site is only recorded, and the first time the switch goes on places it.
    // Standing down for another bot, a site is not even recorded: nothing of
    // GucciBot's goes into the game's code that launch (core/standdown.hpp).
    struct MidhookSite {
        uintptr_t address = 0;
        std::string name;
        safetyhook::MidHookFn fn = nullptr;
        safetyhook::MidHook hook;  // empty until placed
    };
    struct PatchSite {
        uintptr_t address = 0;
        std::vector<uint8_t> bytes;
        std::string name;
        geode::Patch* patch = nullptr;  // null until placed
    };

    // Function-local statics: the $execute blocks that fill these run during
    // static initialisation, in whatever order the linker picked.
    inline std::deque<MidhookSite>& midhookSites() {
        static std::deque<MidhookSite> s;
        return s;
    }
    inline std::deque<PatchSite>& patchSites() {
        static std::deque<PatchSite> s;
        return s;
    }

    // The switch as it was saved. Readable this early: Geode loads a mod's
    // saved values before it loads the mod's binary.
    inline bool engineCodeWantedAtLaunch() {
        return geode::Mod::get()->getSavedValue<bool>(botswitch::kSaveKey, true);
    }

    inline bool placeMidhook(MidhookSite& site) {
        ++g_midhookAttempts;
        site.hook = safetyhook::create_mid(reinterpret_cast<void*>(site.address), site.fn);
        if (!site.hook) {
            ++g_midhookFailures;
            geode::log::error("[GucciBot] Failed to install midhook '{}'", site.name);
            return false;
        }
        geode::log::info("[GucciBot] Installed midhook '{}'", site.name);
        return true;
    }

    inline bool placePatch(PatchSite& site) {
        ++g_patchAttempts;
        auto res = geode::Mod::get()->patch(reinterpret_cast<void*>(site.address), site.bytes);
        if (res.isErr()) {
            ++g_patchFailures;
            geode::log::error("[GucciBot] Failed to apply patch '{}': {}", site.name, res.unwrapErr());
            return false;
        }
        site.patch = res.unwrap();
        return true;
    }

    inline bool util_midhook(uintptr_t address, const std::string& name, safetyhook::MidHookFn fn) {
        // Silicate places these same midhooks; two on one instruction crash
        // the game. Standing down, none go in (core/standdown.hpp).
        if (auto const* bot = standDownBot()) {
            geode::log::warn("[GucciBot] Skipped midhook '{}': {} is enabled", name, bot->name);
            return false;
        }
        auto& site = midhookSites().emplace_back();
        site.address = address;
        site.name = name;
        site.fn = fn;
        if (!engineCodeWantedAtLaunch()) {
            geode::log::info("[GucciBot] Midhook '{}' waits: GucciBot is switched off", name);
            return false;
        }
        return placeMidhook(site);
    }

    // A byte patch, the same way: placed now, or held back while the switch
    // is off (the caller has already checked standDownBot()).
    inline bool util_patch(uintptr_t address, std::vector<uint8_t> bytes, const std::string& name) {
        auto& site = patchSites().emplace_back();
        site.address = address;
        site.bytes = std::move(bytes);
        site.name = name;
        if (!engineCodeWantedAtLaunch())
            return false;
        return placePatch(site);
    }

    // The master switch's half of this file: every midhook and patch out
    // (`in` false) or back in. Out goes midhooks first, then patches; in, the
    // reverse. Called from the ImGui draw, where nothing that runs through
    // these addresses (GJBaseGameLayer::update and below) is on the stack.
    inline void setEngineCodeIn(bool in) {
        if (in) {
            for (auto& site : patchSites()) {
                if (!site.patch) {
                    placePatch(site);
                } else if (!site.patch->isEnabled()) {
                    if (auto res = site.patch->enable(); res.isErr())
                        geode::log::error("[GucciBot] Patch '{}' would not go back in: {}", site.name,
                                          res.unwrapErr());
                }
            }
            for (auto& site : midhookSites()) {
                if (!site.hook) {
                    placeMidhook(site);
                } else if (!site.hook.enabled()) {
                    if (!site.hook.enable())
                        geode::log::error("[GucciBot] Midhook '{}' would not go back in", site.name);
                }
            }
            return;
        }
        for (auto& site : midhookSites()) {
            if (site.hook && site.hook.enabled() && !site.hook.disable())
                geode::log::error("[GucciBot] Midhook '{}' would not come out", site.name);
        }
        for (auto& site : patchSites()) {
            if (site.patch && site.patch->isEnabled()) {
                if (auto res = site.patch->disable(); res.isErr())
                    geode::log::error("[GucciBot] Patch '{}' would not come out: {}", site.name, res.unwrapErr());
            }
        }
    }
#endif

} // namespace gucci
