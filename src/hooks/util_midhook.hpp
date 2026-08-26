#pragma once

#include <Geode/Geode.hpp>
#include <safetyhook.hpp>
#include <string>
#include <unordered_map>

namespace gucci {

    static std::unordered_map<std::string, safetyhook::MidHook> g_midHooks;

    inline int g_midhookAttempts = 0;
    inline int g_midhookFailures = 0;
    inline int g_patchAttempts = 0;
    inline int g_patchFailures = 0;

    inline bool util_midhook(uintptr_t address, const std::string& name, safetyhook::MidHookFn fn) {
        ++g_midhookAttempts;
        auto hook = safetyhook::create_mid(reinterpret_cast<void*>(address), fn);
        if (!hook) {
            ++g_midhookFailures;
            geode::log::error("[GucciBot] Failed to install midhook '{}'", name);
            return false;
        }
        g_midHooks.emplace(name, std::move(hook));
        geode::log::info("[GucciBot] Installed midhook '{}'", name);
        return true;
    }

} // namespace gucci
