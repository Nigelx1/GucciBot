#include "core/setup_check.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/utils/file.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <unordered_map>

using namespace geode::prelude;

namespace gucci::setupcheck {

    namespace {

        // The checkpoint functions a practice fix changes. GucciBot's own
        // practice fix hooks the first four (hooks/hook_playlayer.cpp); the
        // rest are where a fix saves and restores more of the level and the
        // player.
        constexpr const char* kCheckpointFns[] = {
            "PlayLayer::markCheckpoint",       "PlayLayer::storeCheckpoint",  "PlayLayer::loadFromCheckpoint",
            "PlayLayer::removeCheckpoint",     "PlayLayer::createCheckpoint", "PlayerObject::saveToCheckpoint",
            "PlayerObject::loadFromCheckpoint",
        };

        bool isCheckpointFn(std::string_view name) {
            return std::any_of(std::begin(kCheckpointFns), std::end(kCheckpointFns),
                               [&](const char* fn) { return name == fn; });
        }

        struct CheckpointHook {
            Mod* mod;
            std::string fn;
            bool enabled;
        };

        // Every other mod's hook on a checkpoint function. Where the functions
        // are comes from GucciBot's own hooks on them ($modify names a hook
        // "Class::function"), so another mod's hook at one of those addresses
        // counts whatever it's called; one GucciBot doesn't hook is matched by
        // name.
        std::vector<CheckpointHook> checkpointHooks() {
            std::unordered_map<uintptr_t, std::string> where;
            for (auto* hook : Mod::get()->getHooks()) {
                auto name = hook->getDisplayName();
                if (isCheckpointFn(name))
                    where.emplace(hook->getAddress(), std::string(name));
            }
            std::vector<CheckpointHook> out;
            for (auto* mod : Loader::get()->getAllMods()) {
                if (mod == Mod::get() || mod->getID() == "geode.loader" || !mod->isLoaded())
                    continue;
                for (auto* hook : mod->getHooks()) {
                    std::string fn;
                    if (auto it = where.find(hook->getAddress()); it != where.end())
                        fn = it->second;
                    else if (isCheckpointFn(hook->getDisplayName()))
                        fn = std::string(hook->getDisplayName());
                    if (!fn.empty())
                        out.push_back({mod, std::move(fn), hook->isEnabled()});
                }
            }
            return out;
        }

        // Mega Hack v9 keeps each profile's switched-on hacks as a list:
        // v9/hackpro.json, "PROFILE" the one in use and "PROFILES" each
        // profile's list (seen in Nigel's copy, 2026-10-11). Read again only
        // when the file changes. Mega Hack writes the file itself, so a hack
        // switched on this session shows up once Mega Hack has saved it.
        struct MegaHack {
            bool practiceFix = false;
            bool frameExtrapolation = false;
        };

        MegaHack megaHack() {
            static std::filesystem::file_time_type s_stamp{};
            static MegaHack s_found{};
            auto* mod = Loader::get()->getInstalledMod("absolllute.megahack");
            if (!mod || !mod->isLoaded())
                return {};
            auto const path = mod->getSaveDir() / "v9" / "hackpro.json";
            std::error_code ec;
            auto const stamp = std::filesystem::last_write_time(path, ec);
            if (ec)
                return {};
            if (stamp == s_stamp)
                return s_found;
            s_stamp = stamp;
            s_found = {};
            auto json = file::readJson(path);
            if (json.isErr())
                return s_found;
            auto const& root = json.unwrap();
            auto profile = root["PROFILE"].asString();
            if (profile.isErr())
                return s_found;
            auto const& hacks = root["PROFILES"][profile.unwrap()];
            if (!hacks.isArray())
                return s_found;
            for (auto const& hack : hacks) {
                auto name = hack.asString();
                if (name.isErr())
                    continue;
                if (name.unwrap() == "LEVEL/HACK_PRACTICE_BUG_FIX")
                    s_found.practiceFix = true;
                else if (name.unwrap() == "DISPLAY/HACK_FRAME_EXTRAPOLATION")
                    s_found.frameExtrapolation = true;
            }
            return s_found;
        }

        // QOLMod keeps its switches as Geode saved values.
        bool qolModFrameExtrapolation() {
            auto* mod = Loader::get()->getInstalledMod("thesillydoggo.qolmod");
            return mod && mod->isLoaded() && mod->getSavedValue<bool>("frame-extrapolation_enabled", false);
        }

        struct OtherBot {
            const char* id;
            const char* name;
        };
        constexpr OtherBot kOtherBots[] = {
            {"chagh.tcbot", "TCBot"},
            {"toastexgd.toastyreplay-pro", "ToastyReplay PRO"},
        };

        bool botSwitchedOn(OtherBot const& bot) {
            // Asked of the mod list, like core/standdown.hpp.
            auto* mod = Loader::get()->getInstalledMod(bot.id);
            return mod && mod->shouldLoad() && !mod->targetsOutdatedVersion();
        }

        // Dismissed mods, comma-separated (mod ids have no commas).
        constexpr const char* kDismissedKey = "setupcheck_dismissed";

        bool isDismissed(std::string_view id) {
            auto const list = Mod::get()->getSavedValue<std::string>(kDismissedKey, "");
            size_t start = 0;
            while (start <= list.size()) {
                auto end = list.find(',', start);
                if (end == std::string::npos)
                    end = list.size();
                if (std::string_view(list).substr(start, end - start) == id)
                    return true;
                start = end + 1;
            }
            return false;
        }

        bool g_dirty = true;

        std::vector<Problem> work() {
            std::vector<Problem> out;
            if (GucciEngine::get()->standingDown || botswitch::takenOut())
                return out; // GucciBot isn't in the game: nothing here can break it

            if (GameManager::get()->getGameVariable("0027"))
                out.push_back({"Auto-Checkpoints is on",
                               "It's the game's own option: pause a level, open the settings and turn off "
                               "Auto-Checkpoints.",
                               ""});

            auto const mh = megaHack();
            if (mh.practiceFix)
                out.push_back({"Mega Hack's Practice Bug Fix is on",
                               "Turn it off in Mega Hack (Level, Practice Bug Fix). GucciBot has its own practice "
                               "fix, and two of them fight over every checkpoint.",
                               ""});

            std::vector<std::string> named;
            for (auto const& hook : checkpointHooks()) {
                if (!hook.enabled)
                    continue;
                std::string id(hook.mod->getID());
                if (std::find(named.begin(), named.end(), id) != named.end() || isDismissed(id))
                    continue;
                if (id == "absolllute.megahack" && mh.practiceFix)
                    continue; // already named above
                named.push_back(id);
                auto const name = hook.mod->getName().view();
                out.push_back({fmt::format("{} changes the game's checkpoints", name),
                               fmt::format("If that's a practice fix, turn it off in {}: GucciBot has its own, and "
                                           "two of them fight over every checkpoint. If it's something else, press "
                                           "\"It's fine\" on GucciBot's Macro page.",
                                           name),
                               id});
            }

            if (mh.frameExtrapolation)
                out.push_back({"Mega Hack's Frame Extrapolation is on",
                               "Turn it off in Mega Hack (Display, Frame Extrapolation).", ""});
            if (qolModFrameExtrapolation())
                out.push_back({"QOLMod's Frame Extrapolation is on", "Turn it off in QOLMod's menu.", ""});

            for (auto const& bot : kOtherBots)
                if (botSwitchedOn(bot))
                    out.push_back({fmt::format("{} is turned on", bot.name),
                                   "Two bots at once fight over your inputs. Turn one of them off in the Geode mod "
                                   "list and restart the game.",
                                   ""});
            return out;
        }

    } // namespace

    std::vector<Problem> const& current() {
        static std::vector<Problem> s_problems;
        static std::chrono::steady_clock::time_point s_at{};
        auto const now = std::chrono::steady_clock::now();
        if (g_dirty || now - s_at >= std::chrono::seconds(1)) {
            s_problems = work();
            s_at = now;
            g_dirty = false;
        }
        return s_problems;
    }

    void dismissMod(std::string const& id) {
        if (id.empty() || isDismissed(id))
            return;
        auto list = Mod::get()->getSavedValue<std::string>(kDismissedKey, "");
        list += list.empty() ? id : "," + id;
        Mod::get()->setSavedValue(kDismissedKey, list);
        g_dirty = true;
        log::info("[SETUP-CHECK] Not warning about {}'s checkpoint hooks any more", id);
    }

    void showLaunchPopupOnce(cocos2d::CCNode* scene) {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;

        // What was found, for the logs, whether or not it's a problem: a mod
        // that hooks the checkpoints but has the hook switched off is logged
        // too, which is how a new practice fix gets recognised from a log.
        log::info("[SETUP-CHECK] Auto-Checkpoints (game variable 0027): {}",
                  GameManager::get()->getGameVariable("0027") ? "on" : "off");
        for (auto const& hook : checkpointHooks())
            log::info("[SETUP-CHECK] {} hooks {} ({})", hook.mod->getID().view(), hook.fn,
                      hook.enabled ? "on" : "off");
        auto const mh = megaHack();
        log::info("[SETUP-CHECK] Mega Hack: Practice Bug Fix {}, Frame Extrapolation {}; QOLMod Frame Extrapolation {}",
                  mh.practiceFix ? "on" : "off", mh.frameExtrapolation ? "on" : "off",
                  qolModFrameExtrapolation() ? "on" : "off");

        g_dirty = true;
        auto const& problems = current();
        log::info("[SETUP-CHECK] {} problem(s)", problems.size());
        for (auto const& p : problems)
            log::info("[SETUP-CHECK]   {}", p.what);
        if (problems.empty())
            return;

        std::string body = "These can break GucciBot's recording and playback:";
        for (auto const& p : problems)
            body += fmt::format("\n\n<cy>{}</c>. {}", p.what, p.fix);
        auto* popup = createQuickPopup("Heads Up", body, "Got It", nullptr, 420.f, [](auto, bool) {}, false);
        popup->m_scene = scene;
        popup->show();
    }

    void toastIfNeeded() {
        auto const& problems = current();
        if (problems.empty())
            return;
        static std::chrono::steady_clock::time_point s_last{};
        static bool s_any = false;
        auto const now = std::chrono::steady_clock::now();
        if (s_any && now - s_last < std::chrono::minutes(1))
            return;
        s_any = true;
        s_last = now;
        // The toast doesn't wrap: one line, the rest is on the Macro page.
        auto const text = problems.size() == 1
                              ? fmt::format("Heads up: {}", problems.front().what)
                              : fmt::format("Heads up: {} (+{} more on the Macro page)", problems.front().what,
                                            problems.size() - 1);
        Notification::create(text, NotificationIcon::Warning, geode::NOTIFICATION_LONG_TIME)->show();
    }

} // namespace gucci::setupcheck
