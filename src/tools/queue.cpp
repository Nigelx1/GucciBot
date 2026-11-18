// The queues (tools/queue.hpp has what they do). One state machine, stepped
// on every scheduler update (frame(), from core/engine_updater.cpp), works through the running queue's
// items; every step it takes is one the menu or Assistant Access already
// takes (leave the level, open one, Start the Pathfinder, Calculate), so this
// file only decides when.

#include "tools/queue.hpp"

#include "core/platform.hpp"
#include "core/GucciBot.hpp"
#include "core/gbr6_format.hpp"
#include "absense/glue.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/lstar.hpp"
#include "analysis/pathfinder.hpp"
#include "ai/job.hpp"
#include "hacks/legit.hpp"
#include "hacks/safemode.hpp"
#include "render/renderer.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

#if __has_include("core/geode_compat.hpp")
#include "core/geode_compat.hpp"
#define GBQ_HAVE_COMPAT 1
#else
#define GBQ_HAVE_COMPAT 0
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace geode::prelude;

namespace gucci::queue {

    namespace fs = std::filesystem;
    using Clock = std::chrono::steady_clock;

    namespace {

        // ---- small helpers

        std::string utf8(fs::path const& p) {
#if GBQ_HAVE_COMPAT
            return gucci::pathToString(p);
#elif defined(GEODE_IS_WINDOWS)
            return geode::utils::string::wideToUtf8(p.native());
#else
            return p.string();
#endif
        }

        fs::path fromUtf8(std::string const& s) {
            return fs::path(std::u8string(s.begin(), s.end()));
        }

        cocos2d::CCScene* sceneFor(GJGameLevel* level) {
#if GBQ_HAVE_COMPAT
            return gucci::playLayerScene<PlayLayer>(level);
#else
            return PlayLayer::scene(level, false, false);
#endif
        }

        double secondsSince(Clock::time_point t) {
            return std::chrono::duration<double>(Clock::now() - t).count();
        }

        fs::path saveFolder() {
            return GucciEngine::get()->getReplayDir().parent_path();
        }

        fs::path fileFor(Kind k) {
            return saveFolder() / (k == Kind::Pathfinder ? "queue_pathfinder.txt" : "queue_frame_windows.txt");
        }

        void log(std::string const& line) {
            geode::log::info("[GucciBot] queue: {}", line);
        }

        // ---- the state

        enum class Step {
            Idle,
            Next,          // pick the next item
            Leave,         // leave the level (or wait for a scene change to finish)
            Open,          // open the target level (download first if need be)
            WaitDownload,  // the servers are sending it
            WaitOpen,      // the level is loading
            Settle,        // the level is up: a moment for it to settle
            PfStart,       // Start the Pathfinder
            PfWaitStart,
            PfRunning,
            CalcStart,     // load the macro, Calculate
            CalcRunning,
        };

        struct Runner {
            State st;
            bool loaded = false;
            bool offer = false;  // "Continue the queue?" not answered yet
            bool running = false;
            Step step = Step::Idle;
            int index = -1;
            Clock::time_point stepAt{};
            Clock::time_point itemAt{};
            double itemBase = 0;  // seconds the item had before this go
            int attempts = 0;     // Pathfinder starts tried for this item
            bool counting = false;  // this go opens the level to count windows
            Clock::time_point lastAsk{};
            std::string activity;
            // The level to open.
            int targetId = 0;          // main or online ID (0: by name)
            std::string targetName;    // a local level, by name
            Clock::time_point runStart{};
            bool stopAsked = false;  // PfRunning: the Pathfinder was told to stop (once)
            bool asked = false;  // this step's one-off action is done (reset by go)
        };

        Runner g_pf, g_fw;

        Runner& R(Kind k) {
            return k == Kind::Pathfinder ? g_pf : g_fw;
        }

        void save(Kind k) {
            auto& r = R(k);
            r.st.kind = k;
            std::error_code ec;
            fs::create_directories(saveFolder(), ec);
            auto const path = fileFor(k);
            auto tmp = path;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f)
                    return;
                auto const text = serialize(r.st);
                f.write(text.data(), (std::streamsize)text.size());
                if (!f)
                    return;
            }
            fs::rename(tmp, path, ec);
            if (ec) {
                fs::remove(path, ec);
                fs::rename(tmp, path, ec);
            }
        }

        void loadOnce(Kind k) {
            auto& r = R(k);
            if (r.loaded)
                return;
            r.loaded = true;
            r.st.kind = k;
            std::ifstream f(fileFor(k), std::ios::binary);
            if (!f)
                return;
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            State st;
            if (!parse(text, st))
                return;
            st.kind = k;
            bool const wasRunning = st.running;
            int const left = afterRestart(st);
            st.running = false;
            r.st = std::move(st);
            r.offer = wasRunning && left > 0;
            if (r.offer)
                log(fmt::format("the last launch left the {} queue with {} to go", k == Kind::Pathfinder ? "Pathfinder" : "frame-window", left));
        }

        Item* current(Runner& r) {
            return r.index >= 0 && r.index < (int)r.st.items.size() ? &r.st.items[(size_t)r.index] : nullptr;
        }

        void go(Runner& r, Step s) {
            r.step = s;
            r.stepAt = Clock::now();
            r.asked = false;
        }

        // Somebody else has the level: a render, Calculate, a Pathfinder, the
        // AI's jobs. "" when nobody does.
        std::string busyWith() {
            if (SLRenderer::get()->isRecording())
                return "a render is going";
            if (GucciEngine::get()->analyzerOwnsRun())
                return "Calculate is running";
            if (Pathfinder::get()->active || absense::isRunning() || absense::startPending())
                return "the Pathfinder is running";
            if (gucci::ai::job::busy())
                return "the AI engine is busy";
            return "";
        }

        void finishItem(Kind k, Status s, std::string const& note) {
            auto& r = R(k);
            if (auto* it = current(r)) {
                it->status = s;
                if (!note.empty())
                    it->note = note;
                it->seconds = r.itemBase + secondsSince(r.itemAt);
                log(fmt::format("{}: {} ({})", it->levelId ? std::to_string(it->levelId) : it->macro, statusKey(s),
                                it->note));
            }
            r.index = -1;
            r.counting = false;
            save(k);
            go(r, Step::Next);
        }

        void endQueue(Kind k, std::string const& why) {
            auto& r = R(k);
            r.running = false;
            r.st.running = false;
            r.index = -1;
            r.activity.clear();
            go(r, Step::Idle);
            if (!g_pf.running && !g_fw.running)
                safemode::setQueueHold(false);
            save(k);
            log(why);
            Notification::create(why, NotificationIcon::Info, 5.f)->show();
        }

        // The level a frame-window item's macro was made on: its name's
        // "(<id>)" (a Pathfinder queue win), else the level name it was
        // recorded on, looked for in RobTop's levels, the saved online ones
        // and your own. False (and `why`) when there's none.
        bool resolveMacroLevel(Runner& r, Item& it, std::string& why) {
            r.targetId = 0;
            r.targetName.clear();
            auto const path = GucciEngine::get()->findMacroFile(it.macro);
            if (path.empty()) {
                why = "Couldn't find the macro's file";
                return false;
            }
            auto file = GBR6File::loadFromPath(path);
            if (!file) {
                why = "Not a GucciBot macro (convert it on the Macro page first)";
                return false;
            }
            std::string const levelName = file->header.levelName;
            auto* glm = GameLevelManager::get();
            if (int const id = idFromMacroName(it.macro); id > 0) {
                if (isMainLevel(id) || (glm && glm->getSavedLevel(id))) {
                    r.targetId = id;
                    return true;
                }
            }
            if (levelName.empty()) {
                why = "The macro doesn't say which level it was recorded on";
                return false;
            }
            it.levelName = levelName;
            if (glm) {
                for (int id = 1; id <= 5004; id = id == 22 ? 5001 : id + 1) {
                    auto* lvl = glm->getMainLevel(id, true);
                    if (lvl && std::string(lvl->m_levelName) == levelName) {
                        r.targetId = id;
                        return true;
                    }
                }
                if (auto* saved = glm->getSavedLevels(false, 0)) {
                    for (auto* lvl : CCArrayExt<GJGameLevel*>(saved)) {
                        if (lvl && std::string(lvl->m_levelName) == levelName &&
                            !std::string(lvl->m_levelString).empty()) {
                            r.targetId = lvl->m_levelID.value();
                            return true;
                        }
                    }
                }
            }
            if (auto* llm = LocalLevelManager::get(); llm && llm->m_localLevels) {
                for (auto* lvl : CCArrayExt<GJGameLevel*>(llm->m_localLevels)) {
                    if (lvl && std::string(lvl->m_levelName) == levelName) {
                        r.targetName = levelName;
                        return true;
                    }
                }
            }
            why = fmt::format("Couldn't find its level \"{}\" on this PC (download it or play it once first)", levelName);
            return false;
        }

        bool inTargetLevel(Runner const& r) {
            auto* pl = PlayLayer::get();
            if (!pl || !pl->m_level)
                return false;
            if (r.targetId > 0)
                return pl->m_level->m_levelID.value() == r.targetId;
            return std::string(pl->m_level->m_levelName) == r.targetName;
        }

        bool sceneChanging() {
            auto* scene = CCDirector::sharedDirector()->getRunningScene();
            return !scene || typeinfo_cast<CCTransitionScene*>(scene) != nullptr;
        }

        // Opens the target, or says why it can't (yet). 1 opening, 0 asked
        // the servers for it (call again), -1 failed.
        int openTarget(Runner& r, std::string& why) {
            auto* glm = GameLevelManager::get();
            GJGameLevel* level = nullptr;
            if (r.targetId > 0 && isMainLevel(r.targetId)) {
                level = glm ? glm->getMainLevel(r.targetId, false) : nullptr;
                if (!level) {
                    why = "GD has no main level with this ID";
                    return -1;
                }
                auto* stats = GameStatsManager::sharedState();
                int const coins = stats ? stats->getStat("8") : 0;
                if (level->m_requiredCoins > coins) {
                    why = fmt::format("Locked: it needs {} secret coins", level->m_requiredCoins);
                    return -1;
                }
            } else if (r.targetId > 0) {
                level = glm ? glm->getSavedLevel(r.targetId) : nullptr;
                if (!level || std::string(level->m_levelString).empty()) {
#if GB_NATIVE_ENGINE
                    // Not here yet: asked for, as the level's page asks (at
                    // most every 15 s).
                    if (glm && (r.lastAsk == Clock::time_point{} || secondsSince(r.lastAsk) > 15.0)) {
                        r.lastAsk = Clock::now();
                        glm->downloadLevel(r.targetId, false, 0);
                    }
                    return 0;
#else
                    why = "It isn't downloaded on this PC";
                    return -1;
#endif
                }
            } else {
                if (auto* llm = LocalLevelManager::get(); llm && llm->m_localLevels)
                    for (auto* lvl : CCArrayExt<GJGameLevel*>(llm->m_localLevels))
                        if (lvl && std::string(lvl->m_levelName) == r.targetName) {
                            level = lvl;
                            break;
                        }
                if (!level) {
                    why = "Couldn't find the level";
                    return -1;
                }
            }
            if (auto* it = current(r); it && it->levelName.empty())
                it->levelName = level->m_levelName;
            CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, sceneFor(level)));
            return 1;
        }

        template <class FW>
        int countShown(FW const& fw) {
            int n = 0;
            for (auto const& mk : fw.results())
                if (!mk.hidden)
                    n++;
            return n;
        }

        // L* of the windows just counted, worked out on its own thread (as
        // Share my windows does) and written to the item that asked.
        void solveLstarFor(Kind k, std::string const& macro) {
            auto& fw = ::Bot::get()->frameWindow();
            auto inputs = fw.precisionInputs();
            if (inputs.empty())
                return;
            auto const& s = SLSettings::get()->frameWindow;
            lstar::Settings ls;
            ls.m_tps = fw.resultsTps();
            ls.m_respawnSeconds = s.lstarRespawn;
            ls.m_targetSeconds = s.lstarTarget > 0.0 ? s.lstarTarget : 86400.0;
            ls.m_nerve = s.lstarNerve;
            ls.m_fatigue = s.lstarFatigue;
            ls.m_cps = s.lstarCps;
            ls.m_useNerve = s.lstarUseNerve;
            ls.m_useFatigue = s.lstarUseFatigue;
            ls.m_useCps = s.lstarUseCps;
            std::thread([k, macro, inputs = std::move(inputs), ls]() mutable {
                double const v = lstar::solveLikeSolver(std::move(inputs), ls);
                geode::queueInMainThread([k, macro, v] {
                    for (auto& it : R(k).st.items)
                        if (it.macro == macro && it.lstar < 0) {
                            it.lstar = v;
                            save(k);
                            break;
                        }
                });
            }).detach();
        }

        std::string itemLabel(Runner const& r, Item const& it) {
            int const n = r.index + 1;
            int const of = (int)r.st.items.size();
            if (it.levelId > 0)
                return fmt::format("level {} of {} ({})", n, of,
                                   it.levelName.empty() ? std::to_string(it.levelId) : it.levelName);
            return fmt::format("macro {} of {} ({})", n, of, it.macro);
        }

        // ---- one frame of the state machine

        void tick(Kind k) {
            auto& r = R(k);
            if (!r.running)
                return;
            auto* gb = GucciEngine::get();
            double const inStep = secondsSince(r.stepAt);

            if (legit::on()) {
                // Turned on mid-queue: a queue is nothing but cheats.
                if (absense::isRunning() || absense::startPending())
                    absense::stopPathfinder();
                if (auto* it = current(r)) {
                    it->status = Status::Pending;
                    it->tries = std::max(0, it->tries - 1);
                }
                endQueue(k, "Queue stopped: Legit only is on.");
                return;
            }

            switch (r.step) {
                case Step::Idle:
                    return;

                case Step::Next: {
                    r.index = -1;
                    for (size_t i = 0; i < r.st.items.size(); ++i)
                        if (r.st.items[i].status == Status::Pending) {
                            r.index = (int)i;
                            break;
                        }
                    if (r.index < 0) {
                        int beaten = 0;
                        for (auto const& it : r.st.items)
                            beaten += it.status == Status::Beaten;
                        endQueue(k, k == Kind::Pathfinder
                                        ? fmt::format("Pathfinder queue done: {} of {} beaten.", beaten, r.st.items.size())
                                        : fmt::format("Frame-window queue done: {} of {} counted.", beaten, r.st.items.size()));
                        return;
                    }
                    auto& it = r.st.items[(size_t)r.index];
                    it.status = Status::Running;
                    it.tries++;
                    it.note.clear();
                    r.itemBase = it.seconds;
                    r.itemAt = Clock::now();
                    r.attempts = 0;
                    r.counting = false;
                    r.lastAsk = {};
                    if (k == Kind::Pathfinder) {
                        r.targetId = it.levelId;
                        r.targetName.clear();
                    } else {
                        std::string why;
                        if (!resolveMacroLevel(r, it, why)) {
                            finishItem(k, Status::Skipped, why);
                            return;
                        }
                    }
                    save(k);  // marked running: a crash from here on is seen on the next launch
                    r.activity = "Opening " + itemLabel(r, it);
                    go(r, Step::Leave);
                    return;
                }

                case Step::Leave: {
                    if (LevelEditorLayer::get()) {
                        r.activity = "Waiting for you to leave the editor";
                        return;
                    }
                    if (auto* pl = PlayLayer::get()) {
                        if (inStep < 0.2)
                            return;
                        // Already in it (the queue's first level, opened by hand).
                        if (inTargetLevel(r) && !r.counting && k == Kind::FrameWindows) {
                            go(r, Step::Settle);
                            return;
                        }
                        if (!r.asked) {
                            r.asked = true;
                            if (absense::isRunning() || absense::startPending())
                                absense::stopPathfinder();
                            if (::Bot::get()->frameWindow().running())
                                gb->cancelAnalysis();
                            // Frame advance off (a Pathfinder stop can leave
                            // it on): it would hold the fade out.
                            gb->updater.setPaused(false);
                            geode::queueInMainThread([pl] {
                                if (PlayLayer::get() == pl)
                                    pl->onQuit();
                            });
                        }
                        // Still there after 5 s: asked again, straight from here.
                        if (inStep > 5.0 && inStep < 5.5 && !pl->m_isPaused) {
                            gb->updater.setPaused(false);
                            pl->onQuit();
                            return;
                        }
                        if (inStep > 30.0)
                            finishItem(k, Status::Failed, "Couldn't leave the level that was open");
                        return;
                    }
                    if (sceneChanging() || inStep < 1.0)
                        return;
                    go(r, Step::Open);
                    return;
                }

                case Step::Open: {
                    if (sceneChanging())
                        return;
                    std::string why;
                    int const res = openTarget(r, why);
                    if (res < 0) {
                        finishItem(k, Status::Failed, why);
                        return;
                    }
                    if (res == 0) {
                        if (auto* it = current(r))
                            r.activity = "Downloading " + itemLabel(r, *it);
                        go(r, Step::WaitDownload);
                        return;
                    }
                    go(r, Step::WaitOpen);
                    return;
                }

                case Step::WaitDownload: {
                    auto* glm = GameLevelManager::get();
                    auto* level = glm ? glm->getSavedLevel(r.targetId) : nullptr;
                    if (level && !std::string(level->m_levelString).empty()) {
                        if (auto* it = current(r); it && it->levelName.empty())
                            it->levelName = level->m_levelName;
                        go(r, Step::Open);
                        return;
                    }
                    if (inStep > 90.0) {
                        finishItem(k, Status::Failed,
                                   "Couldn't download it (no level with this ID, or no connection)");
                        return;
                    }
                    std::string why;
                    (void)openTarget(r, why);  // asks again every 15 s
                    return;
                }

                case Step::WaitOpen: {
                    if (inTargetLevel(r) && !sceneChanging()) {
                        go(r, Step::Settle);
                        return;
                    }
                    if (inStep > 90.0)
                        finishItem(k, Status::Failed, "The level didn't open");
                    return;
                }

                case Step::Settle: {
                    if (!inTargetLevel(r)) {
                        finishItem(k, Status::Failed, "The level closed");
                        return;
                    }
                    if (inStep < 3.0)
                        return;
                    if (auto* it = current(r); it && it->levelName.empty())
                        if (auto* pl = PlayLayer::get(); pl && pl->m_level)
                            it->levelName = pl->m_level->m_levelName;
                    go(r, k == Kind::Pathfinder && !r.counting ? Step::PfStart : Step::CalcStart);
                    return;
                }

                case Step::PfStart: {
                    auto* it = current(r);
                    if (!it)
                        return finishItem(k, Status::Failed, "");
                    if (r.attempts > 0 && inStep < 3.0)
                        return;  // a moment between tries
                    if (auto const who = busyWith(); !who.empty()) {
                        r.activity = "Waiting: " + who;
                        if (inStep > 120.0)
                            finishItem(k, Status::Failed, "Couldn't start: " + who);
                        return;
                    }
                    if (gb->isRecording() || gb->isPlaying())
                        gb->setMode(GucciEngine::Mode::Idle);
                    if (auto* pl = PlayLayer::get())
                        pl->m_isPaused = false;
                    gb->updater.setPaused(false);
                    // The win is saved under this name (the Pathfinder saves
                    // under the macro's name when it has one).
                    gb->replayName = macroNameFor(it->levelName, it->levelId);
                    r.attempts++;
                    if (!absense::requestStart(true)) {
                        if (r.attempts >= 3)
                            return finishItem(k, Status::Failed, "The Pathfinder didn't start: " + absense::status().message);
                        go(r, Step::PfStart);
                        return;
                    }
                    r.activity = "Starting the Pathfinder on " + itemLabel(r, *it);
                    go(r, Step::PfWaitStart);
                    return;
                }

                case Step::PfWaitStart: {
                    auto const s = absense::status();
                    if (s.running) {
                        r.runStart = Clock::now();
                        r.stopAsked = false;
                        go(r, Step::PfRunning);
                        return;
                    }
                    if ((!absense::startPending() && inStep > 5.0) || inStep > 90.0) {
                        absense::stopPathfinder();
                        if (r.attempts >= 3)
                            return finishItem(k, Status::Failed,
                                              "The Pathfinder didn't start" +
                                                  (s.message.empty() ? std::string() : ": " + s.message));
                        go(r, Step::PfStart);
                    }
                    return;
                }

                case Step::PfRunning: {
                    auto* it = current(r);
                    if (!it)
                        return finishItem(k, Status::Failed, "");
                    auto const s = absense::status();
                    it->bestPercent = std::max(it->bestPercent, (double)s.bestProgress * 100.0);
                    double const ran = secondsSince(r.runStart);
                    r.activity = fmt::format("Pathfinder on {}: best {:.1f}%, {} of {} min", itemLabel(r, *it),
                                             it->bestPercent, (int)(ran / 60.0), r.st.limitMinutes);
                    if (s.running) {
                        if (r.stopAsked) {
                            // Told once; a stop that hasn't taken after 30 s
                            // is told again.
                            if (secondsSince(r.stepAt) > 30.0) {
                                r.stepAt = Clock::now();
                                absense::stopPathfinder();
                            }
                        } else if (!PlayLayer::get()) {
                            r.stopAsked = true;
                            r.stepAt = Clock::now();
                            absense::stopPathfinder();
                        } else if (ran > r.st.limitMinutes * 60.0) {
                            log(fmt::format("{}: the time limit, stopping", it->levelId));
                            it->note = fmt::format("Ran out of time ({} min)", r.st.limitMinutes);
                            r.stopAsked = true;
                            r.stepAt = Clock::now();
                            absense::stopPathfinder();
                        }
                        return;
                    }
                    if (absense::startPending())
                        return;
                    // It stopped: a win saves "macro saved as "<name>"", a
                    // stop after going back the furthest path.
                    std::string const msg = s.message;
                    std::string saved;
                    if (auto const at = msg.find("macro saved as \""); at != std::string::npos) {
                        auto const from = at + std::strlen("macro saved as \"");
                        auto const to = msg.find('"', from);
                        saved = msg.substr(from, to == std::string::npos ? std::string::npos : to - from);
                    }
                    bool const won = !saved.empty() || msg.find("level complete") != std::string::npos;
                    if (won) {
                        it->bestPercent = 100.0;
                        if (saved.empty() && PlayLayer::get() && gb->replay.m_actionAtom.length() > 0) {
                            // Saving when done switched off: saved here.
                            saved = macroNameFor(it->levelName, it->levelId);
                            gb->replay.save(gb->getReplayDir() / fromUtf8(saved + ui::macroExtension()), true);
                        }
                        it->macro = saved;
                        it->note.clear();
                        if (r.st.countWindows && !saved.empty()) {
                            r.counting = true;
                            r.activity = "Counting the frame windows of " + itemLabel(r, *it);
                            go(r, Step::Leave);
                            return;
                        }
                        return finishItem(k, Status::Beaten, "");
                    }
                    std::string furthest;
                    if (auto const at = msg.find("furthest path saved as "); at != std::string::npos) {
                        furthest = msg.substr(at + std::strlen("furthest path saved as "));
                        if (auto const cut = furthest.find(" - "); cut != std::string::npos)
                            furthest = furthest.substr(0, cut);
                    }
                    if (furthest.empty() && PlayLayer::get() && gb->replay.m_actionAtom.length() > 0) {
                        // The live run is the furthest it got: kept as that.
                        furthest = macroNameFor(it->levelName, it->levelId) + " furthest";
                        auto const path = gb->getReplayDir() / fromUtf8(furthest + ui::macroExtension());
                        std::error_code ec;
                        fs::create_directories(path.parent_path(), ec);
                        if (!gb->replay.save(path))
                            furthest.clear();
                    }
                    it->macro = furthest;
                    std::string note = it->note.empty() ? msg : it->note;
                    if (note.size() > 160)
                        note = note.substr(0, 157) + "...";
                    return finishItem(k, Status::NotBeaten, note);
                }

                case Step::CalcStart: {
                    auto* it = current(r);
                    if (!it)
                        return finishItem(k, Status::Failed, "");
                    bool const winCount = k == Kind::Pathfinder;  // a Pathfinder win: it stays beaten whatever happens
                    if (auto const who = busyWith(); !who.empty()) {
                        r.activity = "Waiting: " + who;
                        if (inStep > 120.0)
                            return finishItem(k, winCount ? Status::Beaten : Status::Failed,
                                              "Couldn't count the windows: " + who);
                        return;
                    }
                    auto const path = gb->findMacroFile(it->macro);
                    if (path.empty())
                        return finishItem(k, winCount ? Status::Beaten : Status::Failed,
                                          "Couldn't find the macro's file to count it");
                    if (gb->isRecording())
                        gb->setMode(GucciEngine::Mode::Idle);
                    gb->replay.load(path);
                    gb->replayName = utf8(path.stem());
                    gb->setMode(GucciEngine::Mode::Playing);
                    gb->analyzeFrameWindows(false);
                    if (!gb->fwAcOk) {
                        std::string why = gb->fwAcReport.empty() ? "Calculate didn't start" : gb->fwAcReport;
                        return finishItem(k, winCount ? Status::Beaten : Status::Failed,
                                          "Calculate didn't start: " + why);
                    }
                    r.runStart = Clock::now();
                    r.activity = "Calculate on " + itemLabel(r, *it);
                    go(r, Step::CalcRunning);
                    return;
                }

                case Step::CalcRunning: {
                    auto* it = current(r);
                    if (!it)
                        return finishItem(k, Status::Failed, "");
                    auto& fw = ::Bot::get()->frameWindow();
                    if (fw.running() || fw.returning()) {
                        r.activity = fmt::format("Calculate on {}: {:.0f}%", itemLabel(r, *it), fw.progress() * 100.f);
                        return;
                    }
                    // A frame for the end-of-run save (the menu's draw hook
                    // saves the windows file the frame the run ends).
                    if (inStep < 0.5 && secondsSince(r.runStart) < 1.0)
                        return;
                    bool const winCount = k == Kind::Pathfinder;
                    it->windows = countShown(fw);
                    it->lstar = -1;
                    if (fw.lastRunOk()) {
                        if (it->windows > 0)
                            solveLstarFor(k, it->macro);
                        return finishItem(k, Status::Beaten, "");
                    }
                    std::string why = fw.status().empty() ? "Calculate didn't finish" : fw.status();
                    if (why.size() > 160)
                        why = why.substr(0, 157) + "...";
                    return finishItem(k, winCount ? Status::Beaten : Status::Failed, why);
                }
            }
        }

    }  // namespace

    void frame() {
        // Only at the outer update: a step run from inside one (a checkpoint
        // refresh, Calculate's legs) is not a place to change scenes.
        static int depth = 0;
        if (depth > 0)
            return;
        ++depth;
        for (Kind k : {Kind::Pathfinder, Kind::FrameWindows}) {
            loadOnce(k);
            try {
                tick(k);
            } catch (std::exception const& e) {
                geode::log::error("[GucciBot] queue: {}", e.what());
                if (R(k).running && current(R(k)))
                    finishItem(k, Status::Failed, std::string("Went wrong: ") + e.what());
            }
        }
        --depth;
    }

    bool pathfinderAvailable() {
        return GB_NATIVE_ENGINE != 0;
    }

    State const& state(Kind k) {
        loadOnce(k);
        return R(k).st;
    }

    bool running(Kind k) {
        return R(k).running;
    }

    bool anyRunning() {
        return g_pf.running || g_fw.running;
    }

    std::string activity(Kind k) {
        return R(k).running ? R(k).activity : std::string();
    }

    int currentIndex(Kind k) {
        return R(k).running ? R(k).index : -1;
    }

    double currentSeconds(Kind k) {
        auto& r = R(k);
        return r.running && r.index >= 0 ? r.itemBase + secondsSince(r.itemAt) : 0.0;
    }

    int addLevelIds(std::string const& text, std::vector<std::string>* bad) {
        loadOnce(Kind::Pathfinder);
        auto& r = g_pf;
        int added = 0;
        for (int id : parseLevelIds(text, bad)) {
            bool have = false;
            for (auto const& it : r.st.items)
                if (it.levelId == id && (it.status == Status::Pending || it.status == Status::Running))
                    have = true;
            if (have)
                continue;
            Item it;
            it.levelId = id;
            if (isMainLevel(id))
                if (auto* glm = GameLevelManager::get())
                    if (auto* lvl = glm->getMainLevel(id, true))
                        it.levelName = lvl->m_levelName;
            r.st.items.push_back(std::move(it));
            added++;
        }
        if (added)
            save(Kind::Pathfinder);
        return added;
    }

    int addMacros(std::vector<std::string> const& names) {
        loadOnce(Kind::FrameWindows);
        auto& r = g_fw;
        int added = 0;
        for (auto const& n : names) {
            if (n.empty())
                continue;
            bool have = false;
            for (auto const& it : r.st.items)
                if (it.macro == n && (it.status == Status::Pending || it.status == Status::Running))
                    have = true;
            if (have)
                continue;
            Item it;
            it.macro = n;
            r.st.items.push_back(std::move(it));
            added++;
        }
        if (added)
            save(Kind::FrameWindows);
        return added;
    }

    void setLimitMinutes(int minutes) {
        loadOnce(Kind::Pathfinder);
        g_pf.st.limitMinutes = std::clamp(minutes, 1, 24 * 60);
        save(Kind::Pathfinder);
    }

    void setCountWindows(bool on) {
        loadOnce(Kind::Pathfinder);
        g_pf.st.countWindows = on;
        save(Kind::Pathfinder);
    }

    void remove(Kind k, int index) {
        loadOnce(k);
        auto& r = R(k);
        if (index < 0 || index >= (int)r.st.items.size() || (r.running && index == r.index))
            return;
        r.st.items.erase(r.st.items.begin() + index);
        if (r.running && r.index > index)
            r.index--;
        save(k);
    }

    void clear(Kind k) {
        loadOnce(k);
        auto& r = R(k);
        if (r.running)
            return;
        r.st.items.clear();
        r.offer = false;
        save(k);
    }

    void clearFinished(Kind k) {
        loadOnce(k);
        auto& r = R(k);
        int const keepIndex = r.index;
        std::vector<Item> kept;
        int newIndex = -1;
        for (int i = 0; i < (int)r.st.items.size(); ++i) {
            auto const& it = r.st.items[(size_t)i];
            if (it.status == Status::Pending || it.status == Status::Running || i == keepIndex) {
                if (i == keepIndex)
                    newIndex = (int)kept.size();
                kept.push_back(it);
            }
        }
        r.st.items = std::move(kept);
        r.index = newIndex;
        save(k);
    }

    std::string start(Kind k) {
        loadOnce(k);
        auto& r = R(k);
        if (r.running)
            return "";
        if (k == Kind::Pathfinder && !pathfinderAvailable())
            return "The Pathfinder queue isn't available on this platform yet.";
        if (legit::on())
            return "Turn off Legit only first: a queue plays the Pathfinder and Calculate, which count as cheats.";
        if (R(k == Kind::Pathfinder ? Kind::FrameWindows : Kind::Pathfinder).running)
            return "The other queue is running: stop it first.";
        if (auto const who = busyWith(); !who.empty())
            return "Not now: " + who + ".";
        if (LevelEditorLayer::get())
            return "Leave the editor first.";
        bool any = false;
        for (auto const& it : r.st.items)
            any = any || it.status == Status::Pending;
        if (!any)
            return k == Kind::Pathfinder ? "Add some level IDs first." : "Pick some macros first.";
        r.offer = false;
        r.running = true;
        r.st.running = true;
        r.index = -1;
        r.activity = "Starting";
        safemode::setQueueHold(true);
        save(k);
        go(r, Step::Next);
        log(k == Kind::Pathfinder ? "Pathfinder queue started" : "frame-window queue started");
        return "";
    }

    void stop(Kind k) {
        auto& r = R(k);
        if (!r.running)
            return;
        if (k == Kind::Pathfinder && (absense::isRunning() || absense::startPending()))
            absense::stopPathfinder();
        if (r.step == Step::CalcStart || r.step == Step::CalcRunning)
            GucciEngine::get()->cancelAnalysis();
        if (auto* it = current(r)) {
            // Back in line, as if it hadn't been tried.
            it->status = Status::Pending;
            it->tries = std::max(0, it->tries - 1);
            it->seconds = r.itemBase + secondsSince(r.itemAt);
            it->note = "Stopped by you";
        }
        endQueue(k, "Queue stopped.");
    }

    bool continueOffered(Kind k) {
        loadOnce(k);
        return R(k).offer && !R(k).running;
    }

    void answerContinue(Kind k, bool yes) {
        auto& r = R(k);
        r.offer = false;
        if (yes) {
            auto const why = start(k);
            if (!why.empty())
                Notification::create(why, NotificationIcon::Warning, 4.f)->show();
        }
    }

    matjson::Value toJson(Kind k) {
        loadOnce(k);
        auto& r = R(k);
        auto out = matjson::Value::object();
        out["kind"] = k == Kind::Pathfinder ? "pathfinder" : "frame_windows";
        out["running"] = r.running;
        out["continue_offered"] = continueOffered(k);
        out["activity"] = activity(k);
        out["current"] = (int64_t)currentIndex(k);
        out["limit_minutes"] = (int64_t)r.st.limitMinutes;
        out["count_windows"] = r.st.countWindows;
        out["left"] = (int64_t)r.st.left();
        out["safe_mode_hold"] = safemode::queueHold();
        out["file"] = utf8(fileFor(k));
        auto arr = matjson::Value::array();
        for (size_t i = 0; i < r.st.items.size(); ++i) {
            auto const& it = r.st.items[i];
            auto e = matjson::Value::object();
            if (it.levelId)
                e["id"] = (int64_t)it.levelId;
            e["level"] = it.levelName;
            e["status"] = statusKey(it.status);
            e["tries"] = (int64_t)it.tries;
            e["best_percent"] = it.bestPercent;
            e["seconds"] = (r.running && (int)i == r.index) ? currentSeconds(k) : it.seconds;
            e["macro"] = it.macro;
            e["windows"] = (int64_t)it.windows;
            if (it.lstar >= 0)
                e["lstar"] = it.lstar;
            e["note"] = it.note;
            arr.push(e);
        }
        out["items"] = arr;
        return out;
    }

}  // namespace gucci::queue

// The ticker starts once the game is up, so a queue the last launch left
// can be offered (the page asks), and a notice says so once.
$on_mod(Loaded) {
    geode::queueInMainThread([] {
        using namespace gucci::queue;
        (void)state(Kind::Pathfinder);
        (void)state(Kind::FrameWindows);
        for (Kind k : {Kind::Pathfinder, Kind::FrameWindows})
            if (continueOffered(k))
                Notification::create(fmt::format("GucciBot: the {} queue has {} left. Open its card to continue it.",
                                                 k == Kind::Pathfinder ? "Pathfinder" : "frame-window",
                                                 state(k).left()),
                                     NotificationIcon::Info, 6.f)
                    ->show();
    });
}
