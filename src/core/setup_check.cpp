#include "core/setup_check.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/ui/PopupManager.hpp>
#include <Geode/utils/file.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <unordered_map>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

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

        // Mega Hack's switches. On Windows, asked of Mega Hack itself: its DLL
        // exports hackpro::extensions::isHackEnabled for other mods (Globed
        // reads hacks the same way), so a switch flipped this session counts
        // at once. Elsewhere, or without that export, read from its save file:
        // v9/hackpro.json lists each profile's switched-on hacks ("PROFILE" is
        // the one in use, "PROFILES" each profile's list; seen in Nigel's copy
        // 2026-10-11). Mega Hack writes that file when the game closes, so a
        // change made this session only shows there after a restart.
        struct MegaHack {
            bool practiceFix = false;
            bool frameExtrapolation = false;
            bool live = false; // asked of Mega Hack, not read from the file
        };

        constexpr const char* kMegaHackPracticeFix = "LEVEL/HACK_PRACTICE_BUG_FIX";
        constexpr const char* kMegaHackExtrapolation = "DISPLAY/HACK_FRAME_EXTRAPOLATION";

#ifdef GEODE_IS_WINDOWS
        using MegaHackIsEnabled = bool (*)(std::string const&);

        // Looked up the first time it's needed, which is after Mega Hack has
        // loaded (megaHack() checks that first).
        MegaHackIsEnabled megaHackIsEnabled() {
            static MegaHackIsEnabled const s_fn = []() -> MegaHackIsEnabled {
                auto* dll = GetModuleHandleW(L"absolllute.megahack.dll");
                if (!dll)
                    return nullptr;
                return reinterpret_cast<MegaHackIsEnabled>(GetProcAddress(
                    dll, "?isHackEnabled@extensions@hackpro@@YA_NAEBV?$basic_string@DU?$char_traits@D@std@@V?$"
                         "allocator@D@2@@std@@@Z"));
            }();
            return s_fn;
        }
#endif

        MegaHack megaHack() {
            static std::filesystem::file_time_type s_stamp{};
            static MegaHack s_found{};
            auto* mod = Loader::get()->getInstalledMod("absolllute.megahack");
            if (!mod || !mod->isLoaded())
                return {};
#ifdef GEODE_IS_WINDOWS
            if (auto const isEnabled = megaHackIsEnabled())
                return {isEnabled(kMegaHackPracticeFix), isEnabled(kMegaHackExtrapolation), true};
#endif
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
                if (name.unwrap() == kMegaHackPracticeFix)
                    s_found.practiceFix = true;
                else if (name.unwrap() == kMegaHackExtrapolation)
                    s_found.frameExtrapolation = true;
            }
            return s_found;
        }

        // QOLMod keeps its switches as Geode saved values. Read without a
        // default: the overload with one writes it into QOLMod's save data
        // when the key is missing.
        bool qolModFrameExtrapolation() {
            auto* mod = Loader::get()->getInstalledMod("thesillydoggo.qolmod");
            return mod && mod->isLoaded() && mod->getSavedValue<bool>("frame-extrapolation_enabled");
        }

        // Other bots: any of these loaded alongside GucciBot. Loaded, not just
        // switched on in the mod list: one whose dependency is missing never
        // runs (TCBot on Nigel's PC, most launches), and one switched off this
        // session still runs until the restart. Everything here runs after
        // every mod has loaded; core/standdown.hpp asks the mod list instead
        // because it runs while GucciBot itself is loading.
        constexpr const char* kOtherBots[] = {
            "chagh.tcbot",       "toastexgd.toastyreplay-pro", "toastexgd.toastyreplay", "astralteam.astral",
            "zilko.xdbot",       "kepe.ybot",                  "sorkopiko.40kbot",
        };

        bool isOtherBot(std::string_view id) {
            return std::any_of(std::begin(kOtherBots), std::end(kOtherBots),
                               [&](const char* bot) { return id == bot; });
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
            // Read from the file (not Windows): it only changes when the game
            // closes, so say a restart clears it.
            std::string const mhAfter =
                mh.live ? "" : " Mega Hack saves this when the game closes, so restart the game after turning it off.";
            if (mh.practiceFix)
                out.push_back({"Mega Hack's Practice Bug Fix is on",
                               "Turn it off in Mega Hack (Level, Practice Bug Fix). GucciBot has its own practice "
                               "fix, and two of them fight over every checkpoint." +
                                   mhAfter,
                               ""});

            // What the code shows is a hook, not a switch: a mod can keep its
            // hook in for as long as it's loaded and decide inside it whether
            // to act (Astral does). So the warning can't clear when a fix
            // inside the mod is turned off, and its wording says so. A bot
            // gets the bot warning below instead of this one.
            std::vector<std::string> named;
            for (auto const& hook : checkpointHooks()) {
                if (!hook.enabled)
                    continue;
                std::string id(hook.mod->getID());
                if (std::find(named.begin(), named.end(), id) != named.end() || isDismissed(id) || isOtherBot(id))
                    continue;
                if (id == "absolllute.megahack" && mh.practiceFix)
                    continue; // already named above
                named.push_back(id);
                auto const name = hook.mod->getName().view();
                out.push_back({fmt::format("{} hooks the game's checkpoints", name),
                               fmt::format("Practice fixes work this way. If {} has one, turn it off. GucciBot can't "
                                           "see that switch, so once it's off (or if {} has no practice fix), press "
                                           "\"It's fine\" on GucciBot's Macro page.",
                                           name, name),
                               id});
            }

            if (mh.frameExtrapolation)
                out.push_back({"Mega Hack's Frame Extrapolation is on",
                               "Turn it off in Mega Hack (Display, Frame Extrapolation)." + mhAfter, ""});
            if (qolModFrameExtrapolation())
                out.push_back({"QOLMod's Frame Extrapolation is on", "Turn it off in QOLMod's menu.", ""});

            for (auto const* id : kOtherBots) {
                auto* bot = Loader::get()->getInstalledMod(id);
                if (bot && bot->isLoaded())
                    out.push_back({fmt::format("{} is running", bot->getName().view()),
                                   "Two bots at once fight over your inputs. Turn one of them off in the Geode mod "
                                   "list and restart the game.",
                                   ""});
            }
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

    std::string dismissedNames() {
        auto const list = Mod::get()->getSavedValue<std::string>(kDismissedKey, "");
        std::string out;
        size_t start = 0;
        while (start < list.size()) {
            auto end = list.find(',', start);
            if (end == std::string::npos)
                end = list.size();
            auto const id = list.substr(start, end - start);
            auto* mod = Loader::get()->getInstalledMod(id);
            if (!out.empty())
                out += ", ";
            out += mod ? std::string(mod->getName()) : id;
            start = end + 1;
        }
        return out;
    }

    void undismissAll() {
        Mod::get()->setSavedValue<std::string>(kDismissedKey, "");
        g_dirty = true;
        log::info("[SETUP-CHECK] Warning about every mod's checkpoint hooks again");
    }

    void showLaunchPopupOnce() {
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
        log::info("[SETUP-CHECK] Mega Hack ({}): Practice Bug Fix {}, Frame Extrapolation {}; QOLMod Frame "
                  "Extrapolation {}",
                  mh.live ? "asked live" : "from its save file", mh.practiceFix ? "on" : "off",
                  mh.frameExtrapolation ? "on" : "off", qolModFrameExtrapolation() ? "on" : "off");

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
        // Queued with Geode's popup manager, not shown here: this runs inside
        // MenuLayer::init, before the menu is on screen, and a popup added
        // then doesn't get the back key (Geode moved its own MenuLayer popups
        // for that reason). The queue shows it once the menu is up.
        PopupManager::get().alert("Heads Up", body, "Got It", nullptr, 420.f).showQueue();
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
