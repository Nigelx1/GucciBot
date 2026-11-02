#include "core/setup_check.hpp"

#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/ui/PopupManager.hpp>
#include <Geode/utils/file.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <variant>

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

        // GitHub issue #31: a second GucciBot file in Geode's mods folder.
        // Geode queues every .geode there and loads one file per mod id. A
        // second file with the same id becomes an invalid entry in its mod
        // list (a "geode_invalid-..." id, the file's name for a name), and as
        // that entry has no Geode version of its own (v1.0.0), its load
        // problem says it was made for an older Geode. A file Geode can't
        // open is an invalid entry too (Geode 5.10.1: LoaderImpl.cpp
        // queueMods, ModMetadataImpl.cpp createInvalidMetadata). Looked for
        // both ways: the file of every load problem whose entry Geode
        // couldn't read, and every nigelx1.guccibot*.geode beside the file
        // GucciBot runs from (iOS saves a second download as
        // "nigelx1.guccibot 2.geode", a browser as "nigelx1.guccibot
        // (1).geode", Geode's own install from a file as
        // "nigelx1.guccibot-0.geode"). A file counts when its own mod.json
        // says it's GucciBot, or, named like it, when it can't be opened at
        // all (a download that didn't finish). Found once a launch, since
        // Geode only reads the folder then; a file deleted since drops out.
        struct ExtraCopy {
            std::filesystem::path path;
            std::optional<VersionInfo> version; // unset: it can't be opened
        };

        bool samePath(std::filesystem::path const& a, std::filesystem::path const& b) {
            std::error_code ec;
            bool const same = std::filesystem::equivalent(a, b, ec);
            return ec ? a.lexically_normal() == b.lexically_normal() : same;
        }

        bool namedLikeGucciBot(std::filesystem::path const& path) {
            auto name = utils::string::pathToString(path.filename());
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return name.rfind(std::string(Mod::get()->getID()), 0) == 0;
        }

        std::vector<ExtraCopy> const& extraCopies() {
            static std::vector<ExtraCopy> s_found;
            static bool s_done = false;
            // Geode's load problems are all in once it has finished loading.
            if (s_done || Loader::get()->getLoadingState() != Loader::LoadingState::Done)
                return s_found;
            s_done = true;

            auto* self = Mod::get();
            auto const running = self->getPackagePath();
            std::vector<std::filesystem::path> seen;
            auto const look = [&](std::filesystem::path const& path) {
                std::error_code ec;
                if (path.empty() || path.extension() != ".geode" || !std::filesystem::is_regular_file(path, ec) ||
                    samePath(path, running))
                    return;
                if (std::any_of(seen.begin(), seen.end(), [&](auto const& p) { return samePath(p, path); }))
                    return;
                seen.push_back(path);
                auto const meta = ModMetadata::createFromGeodeFile(path);
                if (!meta.hasErrors() && meta.getID() == self->getID())
                    s_found.push_back({path, meta.getVersion()});
                else if (meta.hasErrors() && namedLikeGucciBot(path))
                    s_found.push_back({path, std::nullopt});
            };
            for (auto const& problem : Loader::get()->getLoadProblems()) {
                if (auto const* path = std::get_if<std::filesystem::path>(&problem.cause))
                    look(*path);
                else if (auto const* meta = std::get_if<ModMetadata>(&problem.cause); meta && meta->hasErrors())
                    look(meta->getPath());
                else if (auto const* mod = std::get_if<Mod*>(&problem.cause);
                         mod && *mod && (*mod)->getMetadata().hasErrors())
                    look((*mod)->getPackagePath());
            }
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dirs::getModsDir(), ec), end; !ec && it != end;
                 it.increment(ec))
                if (namedLikeGucciBot(it->path()))
                    look(it->path());
            return s_found;
        }

        // What to do about them, in words: which file to delete, by version
        // where it can tell (the iOS reporter's extra file was the newer one).
        std::string extraCopiesFix(std::vector<ExtraCopy> const& extras) {
            auto const quoted = [](std::filesystem::path const& path) {
                return "\"" + utils::string::pathToString(path.filename()) + "\"";
            };
            auto* self = Mod::get();
            std::string const ours = quoted(self->getPackagePath());
            std::string const ourVersion = self->getVersion().toVString();
            if (extras.size() == 1) {
                auto const& extra = extras.front();
                if (!extra.version)
                    return fmt::format("Geode can't open {} (a download that didn't finish, maybe), so it lists it as "
                                       "invalid. Delete it from Geode's mods folder and restart the game.",
                                       quoted(extra.path));
                if (*extra.version > self->getVersion())
                    return fmt::format("Geode loads one copy of a mod and lists any other as invalid, and it loaded the "
                                       "older one: {}, from {}. {} is {}. Delete {} from Geode's mods folder and "
                                       "restart the game to get the newer one.",
                                       ourVersion, ours, quoted(extra.path), extra.version->toVString(), ours);
                return fmt::format("Geode loads one copy of a mod and lists any other as invalid. The one running is "
                                   "{}, from {}. Delete the other, {} ({}), from Geode's mods folder and restart the "
                                   "game.",
                                   ourVersion, ours, quoted(extra.path), extra.version->toVString());
            }
            std::string others;
            for (auto const& extra : extras) {
                if (!others.empty())
                    others += ", ";
                others += quoted(extra.path) + " (" +
                          (extra.version ? extra.version->toVString() : std::string("Geode can't open it")) + ")";
            }
            return fmt::format("Geode loads one copy of a mod and lists the others as invalid. The one running is {}, "
                               "from {}; the others are {}. Keep the newest, delete the rest from Geode's mods "
                               "folder, and restart the game.",
                               ourVersion, ours, others);
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

            std::vector<ExtraCopy> extras;
            for (auto const& extra : extraCopies()) {
                std::error_code ec;
                if (std::filesystem::exists(extra.path, ec))
                    extras.push_back(extra);
            }
            if (!extras.empty())
                out.push_back({"A second GucciBot file is in the mods folder", extraCopiesFix(extras), ""});
            // Said as GBot while disguised (ui::branded). current() works the
            // list out again every second, so a new theme shows within one.
            for (auto& p : out) {
                p.what = ui::branded(std::move(p.what));
                p.fix = ui::branded(std::move(p.fix));
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
        std::string copies;
        for (auto const& extra : extraCopies())
            copies += fmt::format("{}{} ({})", copies.empty() ? "" : ", ", utils::string::pathToString(extra.path),
                                  extra.version ? extra.version->toVString() : std::string("can't be opened"));
        log::info("[SETUP-CHECK] GucciBot runs from {}; other GucciBot files in the mods folder: {}",
                  utils::string::pathToString(Mod::get()->getPackagePath()), copies.empty() ? "none" : copies);

        g_dirty = true;
        auto const& problems = current();
        log::info("[SETUP-CHECK] {} problem(s)", problems.size());
        for (auto const& p : problems)
            log::info("[SETUP-CHECK]   {}", p.what);
        if (problems.empty())
            return;

        std::string body = ui::branded("These can break GucciBot's recording and playback:");
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
