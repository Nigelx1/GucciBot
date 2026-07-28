#pragma once
// util_midhook.hpp — GucciBot 10.0
// Thin wrapper around safetyhook mid-function hooks, same pattern as Silicate.

#include <Geode/Geode.hpp>
#include <safetyhook.hpp>
#include <string>
#include <unordered_map>

static std::unordered_map<std::string, safetyhook::MidHook> g_midHooks;

// v8.10 self-check: tallies so the startup diagnostic can verify install health.
inline int  g_midhookAttempts = 0;
inline int  g_midhookFailures = 0;
inline int  g_patchAttempts   = 0;
inline int  g_patchFailures    = 0;

inline bool util_midhook(uintptr_t address, const std::string& name,
                          safetyhook::MidHookFn fn) {
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
