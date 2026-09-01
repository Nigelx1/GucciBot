#include "core/GucciBot.hpp"
#include "gui/gui.hpp"
#include <fmt/format.h>
#include "core/brr_format.hpp"
#include "core/gbr6_format.hpp"
#include "tools/selfcheck.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

using namespace geode::prelude;

namespace gucci {

    namespace fs = std::filesystem;

    GucciScheduler::JobId GucciScheduler::schedule(double interval, std::function<void()> fn) {
        JobId id = m_nextId++;
        m_jobs[id] = {interval, 0.0, fn};
        return id;
    }
    void GucciScheduler::unschedule(JobId id) {
        m_jobs.erase(id);
    }
    void GucciScheduler::reschedule(JobId id, double interval) {
        auto it = m_jobs.find(id);
        if (it != m_jobs.end())
            it->second.interval = interval;
    }
    void GucciScheduler::update(float dt) {
        for (auto& [id, job] : m_jobs) {
            job.elapsed += dt;
            if (job.elapsed >= job.interval) {
                job.elapsed = 0.0;
                job.fn();
            }
        }
    }

    void GucciPracticeFix::saveCurrent(CheckpointObject* cp, uint64_t frameOffset) {
        if (!cp)
            return;
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        SavedCheckpointState state;
        state.m_checkpoint = cp;
        state.m_frameOffset = frameOffset;
        state.m_gameState = pl->m_gameState;

        auto* p1 = pl->m_player1;
        auto* p2 = pl->m_player2;

        state.m_player1 = SavedPlayerCheckpoint::create(p1);
        state.m_player2 = SavedPlayerCheckpoint::create(p2);

        state.m_brokenObjects = m_brokenObjects;

        m_savedCheckpoints.push_back(state);

        StoredFrame sf;
        sf.state = state;
        sf.frame = frameOffset;
        m_storedFrames.push_back(sf);
    }

    void GucciPracticeFix::saveState(CheckpointObject* cp, uint64_t frameOffset) {
        saveCurrent(cp, frameOffset);
    }

    void GucciPracticeFix::restorePreviousFrame(std::function<void(CheckpointObject*)> loadFn) {
        if (m_storedFrames.size() <= 1)
            return;
        m_storedFrames.pop_back();
        auto& prev = m_storedFrames.back();
        if (prev.state.m_checkpoint)
            loadFn(prev.state.m_checkpoint);
        applyCheckpoint(prev.state);
    }

    void GucciPracticeFix::applyLatest() {
        if (m_savedCheckpoints.empty())
            return;
        applyCheckpoint(m_savedCheckpoints.back());
    }

    void GucciPracticeFix::applyCheckpoint(SavedCheckpointState& state) {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        pl->m_gameState = state.m_gameState;

        auto* p1 = pl->m_player1;
        auto* p2 = pl->m_player2;
        if (p1)
            state.m_player1.apply(p1);
        if (p2)
            state.m_player2.apply(p2);
        if (GucciEngine::get()->updater.m_logFrameIncrements) {
            logFrameIncrement(
                "applyCheckpoint(restored, label)", (uint32_t)state.m_frameOffset, p1);
        }

        m_brokenObjects = state.m_brokenObjects;
        for (auto* obj : m_brokenObjects) {
            if (!obj)
                continue;
            obj->m_isDisabled = true;
            obj->m_isDisabled2 = true;
            obj->setOpacity(0.f);
        }
    }

    void GucciPracticeFix::dropLastStoredFrame() {
        if (!m_storedFrames.empty())
            m_storedFrames.pop_back();
    }

    void GucciPracticeFix::clearPlatformer(bool full) {
        m_platformerCheckpoints.clear();
        m_shouldLoadPlatformer = false;
        if (full) {
            m_savedCheckpoints.clear();
            m_storedFrames.clear();
        }
    }

    std::optional<gb::Action> GucciReplaySystem::getCurrentQueuedInput() const {
        if (m_inputIndex >= m_actionAtom.length())
            return std::nullopt;
        return m_actionAtom.m_actions[m_inputIndex];
    }

    std::optional<gb::Action> GucciReplaySystem::getNextInput(uint32_t frame) {
        if (m_inputIndex >= m_actionAtom.length())
            return std::nullopt;
        auto& input = m_actionAtom.m_actions[m_inputIndex];
        if (input.m_frame == frame) {
            m_inputIndex++;
            return input;
        }
        return std::nullopt;
    }

    void GucciReplaySystem::onReset(uint32_t respawnFrame, uint32_t deathFrame) {
        auto* gb = GucciEngine::get();
        if (gb->isRecording() && !gb->fwAnalyzing) {
            size_t before = m_actionAtom.length();
            if (respawnFrame > 0) {
                m_actionAtom.clipFrom(respawnFrame + 1);
                if (!m_actionAtom.m_actions.empty()) {
                    auto& last = m_actionAtom.m_actions.back();
                    if (last.isInput() && last.m_holding) {
                        int p = last.m_player2 ? 1 : 0;
                        log::info("[GucciBot] Recording: removed dangling press @ frame {} "
                                  "(died mid-click) -- suppressing next release for player{}",
                                  last.m_frame,
                                  p + 1);
                        m_actionAtom.m_actions.pop_back();
                        m_suppressNextRelease[p] = true;
                    }
                }
                size_t after = m_actionAtom.length();
                m_inputIndex = m_actionAtom.length();
                if (m_pathSamples.size() > (size_t)respawnFrame + 1)
                    m_pathSamples.resize((size_t)respawnFrame + 1);
                if (before != after)
                    log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                              "— deleted {} stale input(s) after checkpoint (kept {})",
                              deathFrame,
                              respawnFrame,
                              before - after,
                              after);
                else
                    log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                              "— nothing after checkpoint to delete (kept all {})",
                              deathFrame,
                              respawnFrame,
                              after);
            } else {
                m_actionAtom.m_actions.clear();
                m_pathSamples.clear();
                m_inputIndex = 0;
                m_suppressNextRelease[0] = false;
                m_suppressNextRelease[1] = false;
                log::info("[GucciBot] Recording: died@{}, full restart (no checkpoint) "
                          "— cleared {} input(s), re-recording from frame 0",
                          deathFrame,
                          before);
            }
        } else {
            if (m_actionAtom.empty()) {
                m_inputIndex = 0;
                return;
            }
            m_inputIndex =
                static_cast<size_t>(std::distance(m_actionAtom.m_actions.begin(),
                                                  std::find_if(m_actionAtom.m_actions.begin(),
                                                               m_actionAtom.m_actions.end(),
                                                               [respawnFrame](const gb::Action& a) {
                                                                   return a.m_frame >= respawnFrame;
                                                               })));
        }
    }

    fs::path GucciReplaySystem::getCurrentPath() const {
        auto* gb = GucciEngine::get();
        auto dir = gb->getReplayDir();
        for (auto& ext : allKnownMacroExtensions()) {
            std::error_code ec;
            auto candidate = dir / (gb->replayName + ext);
            if (fs::exists(candidate, ec))
                return candidate;
        }
        return dir / (gb->replayName + ".brrr");
    }

    void GucciReplaySystem::backupExisting(const fs::path& path) {
        if (!fs::exists(path))
            return;
        auto backup = path;
        backup.replace_extension(".brrr.bak");
        std::error_code ec;
        fs::copy_file(path, backup, fs::copy_options::overwrite_existing, ec);
    }

    void GucciReplaySystem::createBackup() {
        backupExisting(getCurrentPath());
    }

    static fs::path fwSidecarPath(const fs::path& macroPath) {
        return fs::path(macroPath.string() + ".fw");
    }

    static fs::path pathSamplesSidecarPath(const fs::path& macroPath) {
        return fs::path(macroPath.string() + ".path");
    }

    static void savePathSamples(const fs::path& macroPath,
                                const std::vector<MacroPathSample>& samples) {
        auto sc = pathSamplesSidecarPath(macroPath);
        std::error_code ec;
        if (samples.empty()) {
            fs::remove(sc, ec);
            return;
        }
        std::ofstream f(sc, std::ios::binary);
        if (!f)
            return;
        f.write("GBPS", 4);
        uint8_t ver = 3;
        f.write((const char*)&ver, 1);
        uint32_t n = (uint32_t)samples.size();
        f.write((const char*)&n, 4);
        for (auto const& s : samples) {
            f.write((const char*)&s.p1x, 4);
            f.write((const char*)&s.p1y, 4);
            f.write((const char*)&s.p1XVel, 4);
            f.write((const char*)&s.p1YVel, 4);
            f.write((const char*)&s.p1Rot, 4);
            uint8_t p1flags = (s.p1OnGround ? 1 : 0) | (s.p1UpsideDown ? 2 : 0) |
                              (s.p1Dashing ? 4 : 0) | (s.p1OrbDash ? 8 : 0) |
                              (s.p1OrbNonDash ? 16 : 0);
            f.write((const char*)&p1flags, 1);
            f.write(&s.gamemode1, 1);

            f.write((const char*)&s.p2x, 4);
            f.write((const char*)&s.p2y, 4);
            f.write((const char*)&s.p2XVel, 4);
            f.write((const char*)&s.p2YVel, 4);
            f.write((const char*)&s.p2Rot, 4);
            uint8_t p2flags = (s.p2OnGround ? 1 : 0) | (s.p2UpsideDown ? 2 : 0) |
                              (s.p2Dashing ? 4 : 0) | (s.hasP2 ? 8 : 0) | (s.p2OrbDash ? 16 : 0) |
                              (s.p2OrbNonDash ? 32 : 0);
            f.write((const char*)&p2flags, 1);
            f.write(&s.gamemode2, 1);
        }
    }

    static void loadPathSamples(const fs::path& macroPath, std::vector<MacroPathSample>& samples) {
        samples.clear();
        auto sc = pathSamplesSidecarPath(macroPath);
        if (!fs::exists(sc))
            return;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBPS", 4) != 0)
            return;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver != 3)
            return;
        uint32_t n = 0;
        f.read((char*)&n, 4);
        {
            constexpr std::streamoff kRecordBytes = 44;
            auto curPos = f.tellg();
            f.seekg(0, std::ios::end);
            auto endPos = f.tellg();
            f.seekg(curPos);
            uint64_t maxRecords =
                (curPos >= 0 && endPos > curPos) ? (uint64_t)(endPos - curPos) / kRecordBytes : 0;
            samples.reserve(std::min<uint64_t>(n, maxRecords));
        }
        for (uint32_t i = 0; i < n; ++i) {
            MacroPathSample s;
            f.read((char*)&s.p1x, 4);
            f.read((char*)&s.p1y, 4);
            f.read((char*)&s.p1XVel, 4);
            f.read((char*)&s.p1YVel, 4);
            f.read((char*)&s.p1Rot, 4);
            uint8_t p1flags = 0;
            f.read((char*)&p1flags, 1);
            s.p1OnGround = p1flags & 1;
            s.p1UpsideDown = p1flags & 2;
            s.p1Dashing = p1flags & 4;
            s.p1OrbDash = p1flags & 8;
            s.p1OrbNonDash = p1flags & 16;
            f.read(&s.gamemode1, 1);

            f.read((char*)&s.p2x, 4);
            f.read((char*)&s.p2y, 4);
            f.read((char*)&s.p2XVel, 4);
            f.read((char*)&s.p2YVel, 4);
            f.read((char*)&s.p2Rot, 4);
            uint8_t p2flags = 0;
            f.read((char*)&p2flags, 1);
            s.p2OnGround = p2flags & 1;
            s.p2UpsideDown = p2flags & 2;
            s.p2Dashing = p2flags & 4;
            s.hasP2 = p2flags & 8;
            s.p2OrbDash = p2flags & 16;
            s.p2OrbNonDash = p2flags & 32;
            f.read(&s.gamemode2, 1);

            if (!f)
                break;
            samples.push_back(s);
        }
        log::info("[GucciBot] Macro path: loaded {} sample(s) from sidecar", samples.size());
    }

    static void loadJupiterMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
        out = {};

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return;
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.empty())
            return;

        auto* legacy = BRRMacro::deserialize(bytes);
        if (!legacy)
            return;
        if (legacy->inputs.empty()) {
            delete legacy;
            return;
        }

        double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
        std::unordered_map<int, uint32_t> openPress;
        for (auto& inp : legacy->inputs) {
            int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
            if (inp.isPressed()) {
                openPress[key] = (uint32_t)inp.tick;
            } else {
                auto it = openPress.find(key);
                if (it != openPress.end()) {
                    out.clickIntervalsSec.push_back({it->second / tps, (double)inp.tick / tps});
                    openPress.erase(it);
                }
            }
        }
        out.clickBarTps = tps;
        delete legacy;

        loadPathSamples(path, out.pathSamples);
        out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
        log::info("[GucciBot] Jupiter macro data: {} click interval(s), {} path sample(s)",
                  out.clickIntervalsSec.size(),
                  out.pathSamples.size());
    }

    static void loadTrainerMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
        out = {};

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            return;
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.size() < 4)
            return;

        if (std::memcmp(bytes.data(), "GBR6", 4) == 0) {
            auto result = GBR6File::deserialize(bytes.data(), bytes.size());
            if (!result)
                return;
            double tps = result->header.tps > 0.f ? (double)result->header.tps : 240.0;
            out.levelName = result->header.levelName;

            std::unordered_map<int, uint32_t> openPress;
            auto pairUp = [&](std::vector<GBR6Input> const& inputs, bool p2) {
                for (auto const& inp : inputs) {
                    int key = (int)inp.button * 2 + (p2 ? 1 : 0);
                    if (inp.pressed) {
                        openPress[key] = inp.frame;
                    } else {
                        auto it = openPress.find(key);
                        if (it != openPress.end()) {
                            out.clickIntervalsSec.push_back(
                                {it->second / tps, (double)inp.frame / tps});
                            openPress.erase(it);
                        }
                    }
                }
            };
            pairUp(result->p1Inputs, false);
            pairUp(result->p2Inputs, true);
            out.clickBarTps = tps;
        } else if (bytes.size() >= 4 && bytes[0] == 'B' && bytes[1] == 'R' && bytes[2] == 'R' &&
                   bytes[3] == '\0') {
            auto* legacy = BRRMacro::deserialize(bytes);
            if (!legacy)
                return;
            if (legacy->inputs.empty()) {
                delete legacy;
                return;
            }

            double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
            out.levelName = legacy->levelName;
            out.levelId = legacy->levelId;
            std::unordered_map<int, uint32_t> openPress;
            for (auto& inp : legacy->inputs) {
                int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
                if (inp.isPressed()) {
                    openPress[key] = (uint32_t)inp.tick;
                } else {
                    auto it = openPress.find(key);
                    if (it != openPress.end()) {
                        out.clickIntervalsSec.push_back({it->second / tps, (double)inp.tick / tps});
                        openPress.erase(it);
                    }
                }
            }
            out.clickBarTps = tps;
            delete legacy;
        } else {
            return;
        }

        loadPathSamples(path, out.pathSamples);
        out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
        log::info(
            "[GucciBot] Trainer macro data: {} click interval(s), {} path sample(s), level='{}'",
            out.clickIntervalsSec.size(),
            out.pathSamples.size(),
            out.levelName);
    }

    std::optional<int> GucciEngine::scoreRealClick(const std::vector<std::pair<double, double>>& intervals,
                                                   ClickIndicatorScore& score,
                                                   double clickTimeSec,
                                                   bool isPress,
                                                   double tps) {
        if (score.answeredPress.size() != intervals.size())
            score.reset(intervals.size());

        auto& answered = isPress ? score.answeredPress : score.answeredRelease;
        double best = 0.0;
        int bestIdx = -1;
        for (size_t i = 0; i < intervals.size(); i++) {
            if (answered[i])
                continue;
            double target = isPress ? intervals[i].first : intervals[i].second;
            double d = clickTimeSec - target;
            if (bestIdx < 0 || std::fabs(d) < std::fabs(best)) {
                best = d;
                bestIdx = (int)i;
            }
        }
        if (bestIdx < 0) {
            score.miss++;
            return std::nullopt;
        }
        answered[bestIdx] = true;

        double deltaMs = best * 1000.0;
        if (std::fabs(deltaMs) <= clickIndicatorPerfectMs)
            score.perfect++;
        else if (std::fabs(deltaMs) <= clickIndicatorOkMs)
            score.ok++;
        else
            score.miss++;

        int deltaFrames = (int)std::lround(best * tps);
        score.lastDeltaFrames = deltaFrames;
        score.hasLastReading = true;
        return deltaFrames;
    }

    bool GucciEngine::loadTrainerMacro(const std::string& stem) {
        auto dir = getReplayDir();
        fs::path found;
        for (auto& ext : allKnownMacroExtensions()) {
            std::error_code ec;
            auto candidate = dir / (stem + ext);
            if (fs::exists(candidate, ec)) {
                found = candidate;
                break;
            }
        }
        if (found.empty())
            return false;

        loadTrainerMacroData(found, trainerMacro);
        if (!trainerMacro.loaded)
            return false;
        trainerMacroName = stem;
        Mod::get()->setSavedValue("trainer_macro_name", stem);
        log::info("[GucciBot] Trainer: loaded macro '{}'", stem);
        return true;
    }

    void GucciReplaySystem::savePathSamplesNow() {
        savePathSamples(getCurrentPath(), m_pathSamples);
        log::info("[GucciBot] Macro path: backfilled {} sample(s) saved for '{}'",
                  m_pathSamples.size(),
                  m_replayName);
    }

    static fs::path trainerSidecarPath(const fs::path& macroPath) {
        return fs::path(macroPath.string() + ".trainer");
    }

    static void saveTrainerProgress(const fs::path& macroPath, float bestX) {
        std::ofstream f(trainerSidecarPath(macroPath), std::ios::binary);
        if (!f)
            return;
        f.write("GBTP", 4);
        uint8_t ver = 1;
        f.write((const char*)&ver, 1);
        f.write((const char*)&bestX, 4);
    }

    static float loadTrainerProgress(const fs::path& macroPath) {
        auto sc = trainerSidecarPath(macroPath);
        if (!fs::exists(sc))
            return 0.f;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return 0.f;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBTP", 4) != 0)
            return 0.f;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver != 1)
            return 0.f;
        float bestX = 0.f;
        f.read((char*)&bestX, 4);
        return f ? bestX : 0.f;
    }

    void GucciReplaySystem::saveTrainerProgressNow() {
        saveTrainerProgress(getCurrentPath(), m_trainerBestX);
    }

    static void saveFwMarks(const fs::path& macroPath) {
        auto* gb = GucciEngine::get();
        auto sc = fwSidecarPath(macroPath);
        std::error_code ec;
        if (gb->fwMarks.empty()) {
            fs::remove(sc, ec);
            return;
        }
        std::ofstream f(sc, std::ios::binary);
        if (!f)
            return;
        f.write("GBFW", 4);
        uint8_t ver = 3;
        f.write((const char*)&ver, 1);
        uint32_t n = (uint32_t)gb->fwMarks.size();
        f.write((const char*)&n, 4);
        for (auto const& mk : gb->fwMarks) {
            int32_t w = mk.window;
            uint8_t p2 = mk.player2 ? 1 : 0;
            uint8_t man = mk.manual ? 1 : 0;
            uint8_t rel = mk.isRelease ? 1 : 0;
            f.write((const char*)&mk.x, 4);
            f.write((const char*)&mk.y, 4);
            f.write((const char*)&w, 4);
            f.write((const char*)&p2, 1);
            f.write((const char*)&mk.frame, 4);
            f.write((const char*)&mk.percent, 4);
            f.write((const char*)&man, 1);
            f.write((const char*)&rel, 1);
        }
    }

    static void loadFwMarks(const fs::path& macroPath) {
        auto* gb = GucciEngine::get();
        gb->fwMarks.clear();
        gb->fwHasData = false;
        auto sc = fwSidecarPath(macroPath);
        if (!fs::exists(sc))
            return;
        std::ifstream f(sc, std::ios::binary);
        if (!f)
            return;
        char magic[4] = {};
        f.read(magic, 4);
        if (std::memcmp(magic, "GBFW", 4) != 0)
            return;
        uint8_t ver = 0;
        f.read((char*)&ver, 1);
        if (ver < 1 || ver > 3)
            return;
        uint32_t n = 0;
        f.read((char*)&n, 4);
        for (uint32_t i = 0; i < n; ++i) {
            float x = 0, y = 0, pct = 0;
            int32_t w = 0;
            uint8_t p2 = 0;
            uint32_t fr = 0;
            uint8_t man = 0;
            uint8_t rel = 0;
            f.read((char*)&x, 4);
            f.read((char*)&y, 4);
            f.read((char*)&w, 4);
            f.read((char*)&p2, 1);
            f.read((char*)&fr, 4);
            f.read((char*)&pct, 4);
            if (ver >= 2)
                f.read((char*)&man, 1);
            if (ver >= 3)
                f.read((char*)&rel, 1);
            if (!f)
                break;
            gb->fwMarks.push_back({x, y, (int)w, p2 != 0, fr, pct, man != 0, rel != 0});
        }
        gb->fwHasData = !gb->fwMarks.empty();
        log::info("[GucciBot] Frame-window: loaded {} persisted mark(s) from sidecar",
                  gb->fwMarks.size());
    }

    void GucciEngine::saveFwMarksNow() {
        saveFwMarks(replay.getCurrentPath());
    }

    bool GucciEngine::fwHasManualMarkAt(uint32_t frame, bool player2) const {
        for (auto const& mk : fwMarks)
            if (mk.manual && mk.frame == frame && mk.player2 == player2)
                return true;
        return false;
    }

    void GucciReplaySystem::save(const fs::path& path, bool noOverwrite) {
        if (noOverwrite && fs::exists(path))
            return;

        auto* gb = GucciEngine::get();
        m_replayName = gb->replayName;

        std::vector<GBR6Input> p1, p2;
        for (auto& a : m_actionAtom.m_actions) {
            if (!a.isInput())
                continue;
            GBR6Input inp;
            inp.frame = a.m_frame;
            inp.button = static_cast<uint8_t>(a.m_type);
            inp.pressed = a.m_holding;
            inp.player2 = a.m_player2;
            if (a.m_player2)
                p2.push_back(inp);
            else
                p1.push_back(inp);
        }

        GBR6Header hdr;
        hdr.tps = static_cast<float>(gb->updater.m_tps);
        hdr.name = m_replayName;
        hdr.rngSeed = m_startingSeed;
        hdr.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        if (auto* pl = PlayLayer::get()) {
            if (pl->m_level) {
                hdr.levelName = pl->m_level->m_levelName;
                hdr.flags |= GBR6_HAS_LEVELNAME;
            }
        }

        auto f = GBR6File::fromInputs(hdr, p1, p2);

        for (auto& a : m_actionAtom.m_actions) {
            auto t = a.m_type;
            if (t == gb::ActionType::Death || t == gb::ActionType::Restart ||
                t == gb::ActionType::RestartFull) {
                f.deaths.push_back({a.m_frame, static_cast<uint8_t>(t)});
            }
        }
        if (!f.deaths.empty())
            f.header.flags |= GBR6_HAS_DEATHS;

        if (!f.saveToPath(path)) {
            log::error("[GucciBot] Failed to save to {}", path.string());
        } else if (!f.deaths.empty()) {
            log::info("[GucciBot] Saved with {} intentional-death marker(s)", f.deaths.size());
        }
        saveFwMarks(path);
        savePathSamples(path, m_pathSamples);
        saveTrainerProgress(path, m_trainerBestX);
    }

    void GucciReplaySystem::buildClickIntervals(double tps) {
        m_clickIntervalsSec.clear();
        m_clickBarTps = tps > 0.0 ? tps : 240.0;
        std::unordered_map<int, uint32_t> openPress;
        for (auto const& a : m_actionAtom.m_actions) {
            if (!a.isInput())
                continue;
            int key = (int)a.m_type * 2 + (a.m_player2 ? 1 : 0);
            if (a.m_holding) {
                openPress[key] = a.m_frame;
            } else {
                auto it = openPress.find(key);
                if (it != openPress.end()) {
                    m_clickIntervalsSec.push_back(
                        {it->second / m_clickBarTps, a.m_frame / m_clickBarTps});
                    openPress.erase(it);
                }
            }
        }
        log::info("[GucciBot] Click bar: built {} interval(s) at {} tps",
                  m_clickIntervalsSec.size(),
                  m_clickBarTps);
    }

    void GucciReplaySystem::load(const fs::path& path) {
        if (!fs::exists(path)) {
            log::error("[GucciBot] File not found: {}", path.string());
            return;
        }

        auto* gb = GucciEngine::get();

        char magic[4] = {};
        {
            std::ifstream f(path, std::ios::binary);
            f.read(magic, 4);
        }

        if (std::memcmp(magic, "GBR6", 4) == 0) {
            auto result = GBR6File::loadFromPath(path);
            if (!result) {
                log::error("[GucciBot] Failed to load GBR6: {}", path.string());
                return;
            }
            auto& f = *result;
            gb->updater.setTps(f.header.tps);
            m_startingSeed = f.header.rngSeed;
            m_replayName = f.header.name;
            gb->loadedMacroLevelName = f.header.levelName;
            m_actionAtom.clear();
            m_inputIndex = 0;
            for (auto& inp : f.p1Inputs)
                m_actionAtom.addAction(
                    inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, false);
            for (auto& inp : f.p2Inputs)
                m_actionAtom.addAction(
                    inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, true);
            for (auto& d : f.deaths)
                m_actionAtom.addAction(d.frame, static_cast<gb::ActionType>(d.type), false, false);
            std::sort(m_actionAtom.m_actions.begin(), m_actionAtom.m_actions.end());
            gb->setMode(GucciEngine::Mode::Playing);
            log::info("[GucciBot] Loaded GBR6: {} inputs, {} death marker(s)",
                      m_actionAtom.length(),
                      f.deaths.size());
            loadFwMarks(path);
            loadPathSamples(path, m_pathSamples);
            m_trainerBestX = loadTrainerProgress(path);
            buildClickIntervals(gb->updater.m_tps);
            return;
        }

        auto* legacy = BRRMacro::loadFromDisk(path.stem().string());
        if (legacy && !legacy->inputs.empty()) {
            m_actionAtom.clear();
            m_inputIndex = 0;
            for (auto& inp : legacy->inputs) {
                gb::ActionType t = gb::ActionType::Jump;
                if (inp.actionType == 2)
                    t = gb::ActionType::Left;
                if (inp.actionType == 3)
                    t = gb::ActionType::Right;
                m_actionAtom.addAction(
                    static_cast<uint32_t>(inp.tick), t, inp.isPressed(), inp.isPlayer2());
            }
            gb->updater.setTps(legacy->framerate);
            gb->setMode(GucciEngine::Mode::Playing);
            log::info("[GucciBot] Loaded legacy BRR: {} inputs", m_actionAtom.length());
            loadFwMarks(path);
            m_pathSamples.clear();
            m_trainerBestX = 0.f;
            buildClickIntervals(gb->updater.m_tps);
            delete legacy;
            return;
        }
        delete legacy;
        log::error("[GucciBot] Unknown format: {}", path.string());
    }

    void GucciEngine::setMode(Mode m) {
        if (fwAnalyzing && m != Mode::Playing && fwState != FwState::Finishing) {
            cancelAnalysis();
        }
        Mode prev = mode;
        mode = m;
        if (m == Mode::Recording) {
            fwEnabledLive = false;
            fwLegendEnabled = false;
            Mod::get()->setSavedValue("fw_live", false);
            Mod::get()->setSavedValue("fw_legend", false);
        }
        if (m == Mode::Playing) {
            if (prev != Mode::Playing)
                userTpsSaved = updater.m_tps;
            replay.m_inputIndex = 0;
            fwClickSamples.clear();
            fwSampling = true;
        } else {
            if (prev == Mode::Playing && userTpsSaved > 0.0) {
                updater.setTps(userTpsSaved);
                userTpsSaved = 0.0;
            }
            fwSampling = false;
        }
    }

    bool GucciEngine::beginResumeRecording() {
        if (!isPlaying() || replay.m_actionAtom.empty())
            return false;
        auto* pl = PlayLayer::get();
        if (!pl)
            return false;

        uint32_t now = updater.getFrame();
        auto& atom = replay.m_actionAtom;
        atom.clipFrom(now);
        if (replay.m_pathSamples.size() > (size_t)now)
            replay.m_pathSamples.resize((size_t)now);

        bool held[2][4] = {};
        for (auto const& a : atom.m_actions)
            if (a.isInput())
                held[a.m_player2 ? 1 : 0][(uint8_t)a.m_type] = a.m_holding;

        setMode(Mode::Recording);

        for (int p = 0; p < 2; ++p)
            for (int b = 1; b <= 3; ++b)
                if (held[p][b])
                    pl->handleButton(false, b, p == 0);

        log::info(
            "[GucciBot] Resume recording @ frame {} — {} actions kept", now, atom.m_actions.size());
        return true;
    }

    void GucciEngine::reloadMacroList() {
        storedMacros.clear();
        incompatibleMacros.clear();
        jaMacros.clear();
        giddeyMacros.clear();
        toosiiMacros.clear();
        bamMacros.clear();
        sexyyMacros.clear();
        juiceMacros.clear();
        butlerMacros.clear();
        saweetieMacros.clear();
        maybachMacros.clear();
        romoMacros.clear();
        grizzleyMacros.clear();
        redKingdomMacros.clear();
        lemonadeMacros.clear();
        brrrMacros.clear();
        customThemeMacrosByExt.clear();

        auto dir = getReplayDir();
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
            return;
        }

        auto known = allKnownMacroExtensions();
        std::error_code ec;
        for (auto& it : fs::directory_iterator(dir, ec)) {
            if (!it.is_regular_file())
                continue;
            auto ext = it.path().extension().string();
            auto stem = it.path().stem().string();
            if (std::find(known.begin(), known.end(), ext) != known.end()) {
                storedMacros.push_back(stem);
                if (ext == ".ja")
                    jaMacros.insert(stem);
                if (ext == ".giddey")
                    giddeyMacros.insert(stem);
                if (ext == ".toosii")
                    toosiiMacros.insert(stem);
                if (ext == ".bam")
                    bamMacros.insert(stem);
                if (ext == ".sexyy")
                    sexyyMacros.insert(stem);
                if (ext == ".juice")
                    juiceMacros.insert(stem);
                if (ext == ".butler")
                    butlerMacros.insert(stem);
                if (ext == ".saweetie")
                    saweetieMacros.insert(stem);
                if (ext == ".maybach")
                    maybachMacros.insert(stem);
                if (ext == ".romo")
                    romoMacros.insert(stem);
                if (ext == ".grizzley")
                    grizzleyMacros.insert(stem);
                if (ext == ".redkingdom")
                    redKingdomMacros.insert(stem);
                if (ext == ".lemonade")
                    lemonadeMacros.insert(stem);
                if (ext == ".icebrrr")
                    brrrMacros.insert(stem);
                if (!ext.empty()) {
                    std::string bare = ext.substr(1);
                    bool isBuiltin = ext == ".brrr" || ext == ".toosii" || ext == ".ja" ||
                                     ext == ".giddey" || ext == ".bam" || ext == ".sexyy" ||
                                     ext == ".juice" || ext == ".butler" || ext == ".saweetie" ||
                                     ext == ".maybach" || ext == ".romo" || ext == ".grizzley" ||
                                     ext == ".redkingdom" || ext == ".lemonade" || ext == ".icebrrr";
                    if (!isBuiltin)
                        customThemeMacrosByExt[bare].insert(stem);
                }
            } else if (ext == ".gdr" || ext == ".xd" || ext == ".json" || ext == ".brr") {
                incompatibleMacros.insert(stem);
            }
        }
        std::sort(storedMacros.begin(), storedMacros.end());
    }

    void GucciEngine::applyIntervalAutosave() {
        if (autosaveIntervalSec < 1.0)
            autosaveIntervalSec = 1.0;
        if (replay.m_autosaveJobId == 0) {
            replay.m_autosaveJobId = scheduler.schedule(autosaveIntervalSec, [this] {
                if (autosaveAtInterval && isRecording() && !replay.m_actionAtom.empty()) {
                    auto path = replay.getCurrentPath();
                    if (replayBackupsEnabled)
                        replay.backupExisting(path);
                    replay.save(path);
                }
            });
        } else {
            scheduler.reschedule(replay.m_autosaveJobId, autosaveIntervalSec);
        }
    }

    bool GucciEngine::trimMacro(const std::string& name, int startTick, int endTick, bool rebase) {
        if (endTick <= startTick)
            return false;
        BRRMacro* m = BRRMacro::loadFromDisk(name);
        if (!m)
            return false;
        std::vector<BRRInput> kept;
        kept.reserve(m->inputs.size());
        for (auto const& in : m->inputs) {
            if (in.tick < startTick || in.tick > endTick)
                continue;
            BRRInput c = in;
            if (rebase)
                c.tick -= startTick;
            kept.push_back(c);
        }
        if (kept.empty()) {
            delete m;
            return false;
        }
        m->inputs = std::move(kept);
        m->anchors.clear();
        m->checkpoints.clear();
        m->deathFrames.clear();
        m->attemptStartTicks.clear();
        m->name = name + "_trim";
        m->persistedName.clear();
        m->persist();
        delete m;
        reloadMacroList();
        log::info("[GucciBot] Trimmed '{}' [{}..{}] -> '{}_trim'", name, startTick, endTick, name);
        return true;
    }

    bool GucciEngine::mergeMacros(const std::string& a, const std::string& b, int gapTicks) {
        BRRMacro* ma = BRRMacro::loadFromDisk(a);
        BRRMacro* mb = BRRMacro::loadFromDisk(b);
        if (!ma || !mb || ma->inputs.empty() || mb->inputs.empty()) {
            delete ma;
            delete mb;
            return false;
        }
        int32_t base = ma->inputs.back().tick + (gapTicks > 0 ? gapTicks : 0);
        for (auto in : mb->inputs) {
            in.tick += base;
            ma->inputs.push_back(in);
        }
        ma->anchors.clear();
        ma->checkpoints.clear();
        ma->deathFrames.clear();
        ma->attemptStartTicks.clear();
        ma->name = a + "_merged";
        ma->persistedName.clear();
        ma->persist();
        delete ma;
        delete mb;
        reloadMacroList();
        log::info("[GucciBot] Merged '{}' + '{}' (B offset {}) -> '{}_merged'", a, b, base, a);
        return true;
    }

    void GucciEngine::recordTpsChange(double tps) {
        if (!isRecording()) {
            log::warn("[GucciBot] recordTpsChange: not recording, ignored");
            return;
        }
        replay.m_actionAtom.addTpsChange(updater.m_frame, tps);
        log::info("[GucciBot] Recorded TPS change to {} at frame {}", tps, updater.m_frame);
    }

    namespace {

        struct GdrJsonInput {
            long long frame = 0;
            int button = 1;
            bool player2 = false;
            bool down = false;
        };

        static bool
        gdrJsonExtract(const std::string& text, double& framerate, std::vector<GdrJsonInput>& out) {
            {
                auto p = text.find("\"framerate\"");
                if (p != std::string::npos) {
                    p = text.find(':', p);
                    if (p != std::string::npos) {
                        try {
                            double fr = std::stod(text.substr(p + 1, 32));
                            if (fr > 0)
                                framerate = fr;
                        } catch (...) {
                        }
                    }
                }
            }
            auto ip = text.find("\"inputs\"");
            if (ip == std::string::npos)
                return false;
            auto arrStart = text.find('[', ip);
            if (arrStart == std::string::npos)
                return false;

            size_t i = arrStart + 1;
            int depth = 1;
            while (i < text.size() && depth > 0) {
                char c = text[i];
                if (c == '[') {
                    depth++;
                    i++;
                    continue;
                }
                if (c == ']') {
                    depth--;
                    i++;
                    continue;
                }
                if (c == '{') {
                    size_t j = i + 1;
                    int od = 1;
                    while (j < text.size() && od > 0) {
                        if (text[j] == '{')
                            od++;
                        else if (text[j] == '}')
                            od--;
                        j++;
                    }
                    std::string obj = text.substr(i, j - i);
                    auto num = [&](const char* k, double& val) -> bool {
                        auto p = obj.find(std::string("\"") + k + "\"");
                        if (p == std::string::npos)
                            return false;
                        p = obj.find(':', p);
                        if (p == std::string::npos)
                            return false;
                        try {
                            val = std::stod(obj.substr(p + 1, 32));
                        } catch (...) {
                            return false;
                        }
                        return true;
                    };
                    auto boolean = [&](const char* k, bool& val) -> bool {
                        auto p = obj.find(std::string("\"") + k + "\"");
                        if (p == std::string::npos)
                            return false;
                        p = obj.find(':', p);
                        if (p == std::string::npos)
                            return false;
                        auto rest = obj.substr(p + 1, 8);
                        if (rest.find("true") != std::string::npos) {
                            val = true;
                            return true;
                        }
                        if (rest.find("false") != std::string::npos) {
                            val = false;
                            return true;
                        }
                        try {
                            val = std::stod(rest) != 0.0;
                        } catch (...) {
                            return false;
                        }
                        return true;
                    };
                    GdrJsonInput in;
                    double v = 0;
                    bool bv = false;
                    if (num("frame", v))
                        in.frame = static_cast<long long>(v);
                    if (num("btn", v) || num("button", v))
                        in.button = static_cast<int>(v);
                    if (boolean("2p", bv) || boolean("player2", bv))
                        in.player2 = bv;
                    if (boolean("down", bv) || boolean("hold", bv) || boolean("holding", bv))
                        in.down = bv;
                    out.push_back(in);
                    i = j;
                    continue;
                }
                i++;
            }
            return !out.empty();
        }

        namespace gdrmsgpack {

            static constexpr size_t kMaxDepth = 64;

            static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth = 0);

            static bool skipN(const std::vector<uint8_t>& b, size_t& i, size_t n) {
                if (i + n > b.size())
                    return false;
                i += n;
                return true;
            }
            static bool skipContainer(
                const std::vector<uint8_t>& b, size_t& i, size_t count, bool isMap, size_t depth) {
                if (depth > kMaxDepth)
                    return false;
                size_t n = isMap ? count * 2 : count;
                for (size_t k = 0; k < n; k++)
                    if (!skipValue(b, i, depth + 1))
                        return false;
                return true;
            }
            static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth) {
                if (depth > kMaxDepth)
                    return false;
                if (i >= b.size())
                    return false;
                uint8_t t = b[i++];
                if (t <= 0x7f)
                    return true;
                if (t >= 0xe0)
                    return true;
                if (t >= 0x80 && t <= 0x8f)
                    return skipContainer(b, i, t & 0x0f, true, depth + 1);
                if (t >= 0x90 && t <= 0x9f)
                    return skipContainer(b, i, t & 0x0f, false, depth + 1);
                if (t >= 0xa0 && t <= 0xbf)
                    return skipN(b, i, t & 0x1f);
                switch (t) {
                case 0xc0:
                case 0xc2:
                case 0xc3:
                    return true;
                case 0xc4: {
                    if (i >= b.size())
                        return false;
                    uint8_t n = b[i++];
                    return skipN(b, i, n);
                }
                case 0xc5: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipN(b, i, n);
                }
                case 0xc6: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipN(b, i, n);
                }
                case 0xca:
                    return skipN(b, i, 4);
                case 0xcb:
                    return skipN(b, i, 8);
                case 0xcc:
                    return skipN(b, i, 1);
                case 0xcd:
                    return skipN(b, i, 2);
                case 0xce:
                    return skipN(b, i, 4);
                case 0xcf:
                    return skipN(b, i, 8);
                case 0xd0:
                    return skipN(b, i, 1);
                case 0xd1:
                    return skipN(b, i, 2);
                case 0xd2:
                    return skipN(b, i, 4);
                case 0xd3:
                    return skipN(b, i, 8);
                case 0xd9: {
                    if (i >= b.size())
                        return false;
                    uint8_t n = b[i++];
                    return skipN(b, i, n);
                }
                case 0xda: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipN(b, i, n);
                }
                case 0xdb: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipN(b, i, n);
                }
                case 0xdc: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipContainer(b, i, n, false, depth + 1);
                }
                case 0xdd: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipContainer(b, i, n, false, depth + 1);
                }
                case 0xde: {
                    if (i + 2 > b.size())
                        return false;
                    uint16_t n = (uint16_t)((b[i] << 8) | b[i + 1]);
                    i += 2;
                    return skipContainer(b, i, n, true, depth + 1);
                }
                case 0xdf: {
                    if (i + 4 > b.size())
                        return false;
                    uint32_t n = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                                 ((uint32_t)b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    return skipContainer(b, i, n, true, depth + 1);
                }
                default:
                    return false;
                }
            }

            struct Header {
                size_t count = SIZE_MAX;
                bool isMap = false;
            };
            static Header readContainerHeader(const std::vector<uint8_t>& b, size_t& i) {
                Header h;
                if (i >= b.size())
                    return h;
                uint8_t t = b[i];
                if (t >= 0x80 && t <= 0x8f) {
                    h.count = t & 0x0f;
                    h.isMap = true;
                    i++;
                    return h;
                }
                if (t >= 0x90 && t <= 0x9f) {
                    h.count = t & 0x0f;
                    h.isMap = false;
                    i++;
                    return h;
                }
                if (t == 0xdc) {
                    if (i + 3 > b.size())
                        return h;
                    h.count = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    h.isMap = false;
                    i += 3;
                    return h;
                }
                if (t == 0xdd) {
                    if (i + 5 > b.size())
                        return h;
                    h.count = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                       ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    h.isMap = false;
                    i += 5;
                    return h;
                }
                if (t == 0xde) {
                    if (i + 3 > b.size())
                        return h;
                    h.count = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    h.isMap = true;
                    i += 3;
                    return h;
                }
                if (t == 0xdf) {
                    if (i + 5 > b.size())
                        return h;
                    h.count = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                       ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    h.isMap = true;
                    i += 5;
                    return h;
                }
                return h;
            }

            static bool readString(const std::vector<uint8_t>& b, size_t& i, std::string& out) {
                if (i >= b.size())
                    return false;
                uint8_t t = b[i];
                size_t len;
                if (t >= 0xa0 && t <= 0xbf) {
                    len = t & 0x1f;
                    i++;
                } else if (t == 0xd9) {
                    if (i + 2 > b.size())
                        return false;
                    len = b[i + 1];
                    i += 2;
                } else if (t == 0xda) {
                    if (i + 3 > b.size())
                        return false;
                    len = (size_t)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                } else if (t == 0xdb) {
                    if (i + 5 > b.size())
                        return false;
                    len = (size_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                   ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                } else
                    return false;
                if (i + len > b.size())
                    return false;
                out.assign((const char*)&b[i], len);
                i += len;
                return true;
            }

            static bool readNumber(const std::vector<uint8_t>& b, size_t& i, double& out) {
                if (i >= b.size())
                    return false;
                uint8_t t = b[i];
                if (t <= 0x7f) {
                    out = t;
                    i++;
                    return true;
                }
                if (t >= 0xe0) {
                    out = (double)(int8_t)t;
                    i++;
                    return true;
                }
                switch (t) {
                case 0xcc:
                    if (i + 2 > b.size())
                        return false;
                    out = b[i + 1];
                    i += 2;
                    return true;
                case 0xcd:
                    if (i + 3 > b.size())
                        return false;
                    out = (double)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                    return true;
                case 0xce:
                    if (i + 5 > b.size())
                        return false;
                    out = (double)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                   ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                    return true;
                case 0xcf: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t v = 0;
                    for (int k = 0; k < 8; k++)
                        v = (v << 8) | b[i + 1 + k];
                    out = (double)v;
                    i += 9;
                    return true;
                }
                case 0xd0:
                    if (i + 2 > b.size())
                        return false;
                    out = (double)(int8_t)b[i + 1];
                    i += 2;
                    return true;
                case 0xd1:
                    if (i + 3 > b.size())
                        return false;
                    out = (double)(int16_t)((b[i + 1] << 8) | b[i + 2]);
                    i += 3;
                    return true;
                case 0xd2:
                    if (i + 5 > b.size())
                        return false;
                    out =
                        (double)(int32_t)(((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                          ((uint32_t)b[i + 3] << 8) | b[i + 4]);
                    i += 5;
                    return true;
                case 0xd3: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t v = 0;
                    for (int k = 0; k < 8; k++)
                        v = (v << 8) | b[i + 1 + k];
                    out = (double)(int64_t)v;
                    i += 9;
                    return true;
                }
                case 0xca: {
                    if (i + 5 > b.size())
                        return false;
                    uint32_t u = ((uint32_t)b[i + 1] << 24) | ((uint32_t)b[i + 2] << 16) |
                                 ((uint32_t)b[i + 3] << 8) | b[i + 4];
                    float f;
                    std::memcpy(&f, &u, 4);
                    out = f;
                    i += 5;
                    return true;
                }
                case 0xcb: {
                    if (i + 9 > b.size())
                        return false;
                    uint64_t u = 0;
                    for (int k = 0; k < 8; k++)
                        u = (u << 8) | b[i + 1 + k];
                    double d;
                    std::memcpy(&d, &u, 8);
                    out = d;
                    i += 9;
                    return true;
                }
                default:
                    return false;
                }
            }

            static bool readBool(const std::vector<uint8_t>& b, size_t& i, bool& out) {
                if (i >= b.size())
                    return false;
                if (b[i] == 0xc2) {
                    out = false;
                    i++;
                    return true;
                }
                if (b[i] == 0xc3) {
                    out = true;
                    i++;
                    return true;
                }
                double n;
                size_t save = i;
                if (readNumber(b, i, n)) {
                    out = n != 0.0;
                    return true;
                }
                i = save;
                return false;
            }

        } // namespace gdrmsgpack

        static bool gdrBinaryExtract(const std::vector<uint8_t>& bytes,
                                     double& framerate,
                                     std::vector<GdrJsonInput>& out) {
            using namespace gdrmsgpack;
            size_t i = 0;
            auto top = readContainerHeader(bytes, i);
            if (top.count == SIZE_MAX || !top.isMap)
                return false;

            for (size_t k = 0; k < top.count; k++) {
                std::string key;
                if (!readString(bytes, i, key))
                    return false;

                if (key == "inputs") {
                    auto arr = readContainerHeader(bytes, i);
                    if (arr.count == SIZE_MAX || arr.isMap)
                        return false;
                    for (size_t e = 0; e < arr.count; e++) {
                        auto obj = readContainerHeader(bytes, i);
                        if (obj.count == SIZE_MAX || !obj.isMap)
                            return false;
                        GdrJsonInput in;
                        for (size_t f = 0; f < obj.count; f++) {
                            std::string fk;
                            if (!readString(bytes, i, fk))
                                return false;
                            if (fk == "frame") {
                                double v;
                                if (readNumber(bytes, i, v))
                                    in.frame = (long long)v;
                                else
                                    return false;
                            } else if (fk == "btn" || fk == "button") {
                                double v;
                                if (readNumber(bytes, i, v))
                                    in.button = (int)v;
                                else
                                    return false;
                            } else if (fk == "2p" || fk == "player2") {
                                bool v;
                                if (readBool(bytes, i, v))
                                    in.player2 = v;
                                else
                                    return false;
                            } else if (fk == "down" || fk == "hold" || fk == "holding") {
                                bool v;
                                if (readBool(bytes, i, v))
                                    in.down = v;
                                else
                                    return false;
                            } else {
                                if (!skipValue(bytes, i))
                                    return false;
                            }
                        }
                        out.push_back(in);
                    }
                } else if (key == "framerate" || key == "fps") {
                    double v;
                    size_t save = i;
                    if (readNumber(bytes, i, v)) {
                        if (v > 0)
                            framerate = v;
                    } else {
                        i = save;
                        if (!skipValue(bytes, i))
                            return false;
                    }
                } else {
                    if (!skipValue(bytes, i))
                        return false;
                }
            }
            return !out.empty();
        }

    } // namespace

    bool GucciEngine::convertToBRR(const std::string& name) {
        auto dir = getReplayDir();
        auto isNative = [](const std::string& e) {
            auto known = allKnownMacroExtensions();
            return std::find(known.begin(), known.end(), e) != known.end();
        };

        fs::path src;
        std::error_code ec;
        for (auto& it : fs::directory_iterator(dir, ec)) {
            if (!it.is_regular_file())
                continue;
            if (it.path().stem().string() != name)
                continue;
            if (isNative(it.path().extension().string()))
                continue;
            src = it.path();
            break;
        }
        if (src.empty()) {
            log::warn("[GucciBot] convertToBRR: no legacy file found for '{}'", name);
            return false;
        }

        std::ifstream f(src, std::ios::binary | std::ios::ate);
        if (!f) {
            log::warn("[GucciBot] convertToBRR: can't open {}", src.string());
            return false;
        }
        auto sz = static_cast<size_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> bytes(sz);
        if (sz)
            f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
        f.close();
        if (bytes.empty()) {
            log::warn("[GucciBot] convertToBRR: {} is empty", src.string());
            return false;
        }

        if (auto* parsed = BRRMacro::deserialize(bytes)) {
            parsed->name = name;
            parsed->persistedName.clear();
            parsed->persist();
            delete parsed;
            reloadMacroList();
            log::info("[GucciBot] Converted '{}' (BRR payload) to native format", name);
            return true;
        }

        std::string text(bytes.begin(), bytes.end());
        if (text.find('{') != std::string::npos && text.find("\"inputs\"") != std::string::npos) {
            double framerate = 240.0;
            std::vector<GdrJsonInput> gdrInputs;
            if (gdrJsonExtract(text, framerate, gdrInputs)) {
                BRRMacro out;
                out.name = name;
                out.framerate = framerate;
                for (auto& gi : gdrInputs) {
                    BRRInput bi;
                    bi.tick = static_cast<int32_t>(std::max(0LL, gi.frame));
                    bi.actionType = static_cast<uint8_t>(std::clamp(gi.button, 1, 3));
                    bi.setPlayer2(gi.player2);
                    bi.setPressed(gi.down);
                    out.inputs.push_back(bi);
                }
                std::sort(
                    out.inputs.begin(), out.inputs.end(), [](const BRRInput& x, const BRRInput& y) {
                        return x.tick < y.tick;
                    });
                out.persist();
                reloadMacroList();
                log::info("[GucciBot] Converted '{}' (GDR JSON, {} inputs) to native format",
                          name,
                          out.inputs.size());
                return true;
            }
        }

        {
            double framerate = 240.0;
            std::vector<GdrJsonInput> gdrInputs;
            if (gdrBinaryExtract(bytes, framerate, gdrInputs)) {
                BRRMacro out;
                out.name = name;
                out.framerate = framerate;
                for (auto& gi : gdrInputs) {
                    BRRInput bi;
                    bi.tick = static_cast<int32_t>(std::max(0LL, gi.frame));
                    bi.actionType = static_cast<uint8_t>(std::clamp(gi.button, 1, 3));
                    bi.setPlayer2(gi.player2);
                    bi.setPressed(gi.down);
                    out.inputs.push_back(bi);
                }
                std::sort(
                    out.inputs.begin(), out.inputs.end(), [](const BRRInput& x, const BRRInput& y) {
                        return x.tick < y.tick;
                    });
                out.persist();
                reloadMacroList();
                log::info(
                    "[GucciBot] Converted '{}' (GDR binary/msgpack, {} inputs) to native format",
                    name,
                    out.inputs.size());
                return true;
            }
        }

        log::warn("[GucciBot] convertToBRR: unsupported format: {}", src.string());
        return false;
    }

    std::vector<GucciEngine::DiffEntry> GucciEngine::diffMacros(const std::string& a,
                                                                const std::string& b) {
        std::vector<DiffEntry> out;
        constexpr size_t kMaxDiffs = 500;

        BRRMacro* ma = BRRMacro::loadFromDisk(a);
        BRRMacro* mb = BRRMacro::loadFromDisk(b);
        if (!ma || !mb) {
            if (!ma)
                out.push_back({-1, "Could not load macro A: " + a});
            if (!mb)
                out.push_back({-1, "Could not load macro B: " + b});
            delete ma;
            delete mb;
            return out;
        }

        if (ma->framerate != mb->framerate)
            out.push_back(
                {-1, fmt::format("Framerate differs: A={} vs B={}", ma->framerate, mb->framerate)});
        if (ma->inputs.size() != mb->inputs.size())
            out.push_back({-1,
                           fmt::format("Input count differs: A={} vs B={}",
                                       ma->inputs.size(),
                                       mb->inputs.size())});

        auto describe = [](const BRRInput& in) {
            const char* btn = in.actionType == 2 ? "left" : in.actionType == 3 ? "right" : "jump";
            return fmt::format("f{} {} {} {}",
                               in.tick,
                               btn,
                               in.isPressed() ? "press" : "release",
                               in.isPlayer2() ? "P2" : "P1");
        };

        size_t n = std::min(ma->inputs.size(), mb->inputs.size());
        for (size_t i = 0; i < n && out.size() < kMaxDiffs; ++i) {
            const auto& A = ma->inputs[i];
            const auto& B = mb->inputs[i];
            if (A.tick != B.tick || A.actionType != B.actionType ||
                A.isPressed() != B.isPressed() || A.isPlayer2() != B.isPlayer2()) {
                out.push_back({static_cast<int>(A.tick),
                               fmt::format("#{}: A[{}] vs B[{}]", i, describe(A), describe(B))});
            }
        }

        if (out.size() >= kMaxDiffs) {
            out.push_back({-1, "... truncated at 500 differences."});
        } else if (ma->inputs.size() != mb->inputs.size()) {
            const auto& longer = ma->inputs.size() > mb->inputs.size() ? *ma : *mb;
            const char* tag = ma->inputs.size() > mb->inputs.size() ? "A" : "B";
            size_t extra = longer.inputs.size() - n;
            out.push_back({static_cast<int>(longer.inputs[n].tick),
                           fmt::format("{} has {} extra input(s) starting at [{}]",
                                       tag,
                                       extra,
                                       describe(longer.inputs[n]))});
        }

        delete ma;
        delete mb;
        return out;
    }

    void GucciEngine::saveBotSettingsPreset(const std::string& name) {
        BotSettingsPreset p;
        p.name = name;
        p.tps = updater.m_tps;
        p.speedhack = updater.m_speedhack;
        p.lockDelta = updater.m_lockDelta;
        p.backwardsStepping = updater.m_backwardsStepping;
        p.ssbFix = updater.m_ssbFix;
        p.extrapolateFrames = updater.m_extrapolateFrames;
        p.preventDeath = updater.m_preventDeath;
        p.autoFlipOnDeath = updater.m_autoFlipOnDeath;
        p.maintainGravity = replay.m_maintainGravity;
        p.mirrorInputs = replay.m_mirrorInputs;
        p.noclip = noclipEnabled;
        p.autosaveInterval = autosaveIntervalSec;
        p.autosaveAtInterval = autosaveAtInterval;
        p.autosaveAtLevelEnd = autosaveAtLevelEnd;

        auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(), [&](auto& x) {
            return x.name == name;
        });
        if (it != settingsPresets.end())
            *it = p;
        else
            settingsPresets.push_back(p);

        auto dir = getPresetsDir();
        fs::create_directories(dir);
        std::ofstream f(dir / (name + ".json"));
        f << "{\"name\":\"" << p.name << "\""
          << ",\"tps\":" << p.tps << ",\"speedhack\":" << p.speedhack
          << ",\"lockDelta\":" << (p.lockDelta ? "true" : "false")
          << ",\"lockDeltaMode\":" << p.lockDeltaMode
          << ",\"ssbFix\":" << (p.ssbFix ? "true" : "false")
          << ",\"preventDeath\":" << (p.preventDeath ? "true" : "false")
          << ",\"noclip\":" << (p.noclip ? "true" : "false") << "}";
    }

    bool GucciEngine::loadBotSettingsPreset(const std::string& name) {
        auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(), [&](auto& x) {
            return x.name == name;
        });
        if (it == settingsPresets.end())
            return false;
        auto& p = *it;
        updater.setTps(p.tps);
        updater.m_speedhack = p.speedhack;
        updater.m_lockDelta = p.lockDelta;
        updater.m_ssbFix = p.ssbFix;
        updater.m_preventDeath = p.preventDeath;
        replay.m_maintainGravity = false;
        noclipEnabled = p.noclip;
        autosaveIntervalSec = p.autosaveInterval;
        autosaveAtInterval = p.autosaveAtInterval;
        autosaveAtLevelEnd = p.autosaveAtLevelEnd;
        return true;
    }

    void GucciEngine::deleteBotSettingsPreset(const std::string& name) {
        settingsPresets.erase(std::remove_if(settingsPresets.begin(),
                                             settingsPresets.end(),
                                             [&](auto& p) {
                                                 return p.name == name;
                                             }),
                              settingsPresets.end());
        std::error_code ec;
        fs::remove(getPresetsDir() / (name + ".json"), ec);
    }

    void GucciEngine::initialize() {
        fs::create_directories(getReplayDir());
        fs::create_directories(getPresetsDir());

        {
            auto jupDir = Mod::get()->getSaveDir() / "jupiter";
            fs::create_directories(jupDir);

            auto findIn = [](fs::path const& dir, std::string const& stem) -> fs::path {
                for (auto& ext : allKnownMacroExtensions()) {
                    std::error_code ec;
                    auto candidate = dir / (stem + ext);
                    if (fs::exists(candidate, ec))
                        return candidate;
                }
                return {};
            };

            auto hidden = findIn(jupDir, "jupiter_my_favourite");
            if (!hidden.empty())
                loadJupiterMacroData(hidden, jupiterMacro);

            if (!jupiterMacro.loaded) {
                std::error_code rmEc;
                if (!hidden.empty())
                    fs::remove(hidden, rmEc);

                auto bundled = Mod::get()->getResourcesDir() / "jupiter_my_favourite.gdr";
                std::error_code ec;
                if (fs::exists(bundled, ec)) {
                    const std::string seedStem = "__guccibot_jupiter_seed";
                    auto seedDest = getReplayDir() / (seedStem + ".gdr");
                    fs::copy_file(bundled, seedDest, fs::copy_options::overwrite_existing, ec);
                    convertToBRR(seedStem);
                    auto converted = findIn(getReplayDir(), seedStem);
                    if (!converted.empty()) {
                        auto dest =
                            jupDir / ("jupiter_my_favourite" + converted.extension().string());
                        fs::rename(converted, dest, ec);
                        if (!ec)
                            hidden = dest;
                    }
                    fs::remove(seedDest, ec);
                    reloadMacroList();
                    if (!hidden.empty())
                        loadJupiterMacroData(hidden, jupiterMacro);
                }
            }

            // Diagnostic added 2026-08-31: Nigel reported the Click Trainer's
            // Resume button doing nothing, traced to drawJupiterClickBar's
            // "No click data yet." early-out when jupiterMacro.clickIntervalsSec
            // is empty -- but a standalone byte-for-byte replica of
            // loadJupiterMacroData's own parsing logic, run against the exact
            // .brrr file sitting in his save folder, decoded it cleanly (467
            // real inputs, 233 matched click pairs, zero unmatched). So the
            // default macro file itself isn't the problem -- this records
            // what the LIVE game actually ends up with in jupiterMacro after
            // this whole block runs, dedicated file since Geode's own
            // console log isn't persisted anywhere reachable on this machine.
            std::ofstream jmLog(Mod::get()->getSaveDir() / "guccibot_jupitermacro.log",
                                std::ios::trunc);
            if (jmLog) {
                jmLog << "hidden_path_found=" << (!hidden.empty() ? hidden.string() : "<none>")
                      << "\n";
                jmLog << "jupiterMacro.loaded=" << (jupiterMacro.loaded ? 1 : 0) << "\n";
                jmLog << "jupiterMacro.clickIntervalsSec.size()="
                      << jupiterMacro.clickIntervalsSec.size() << "\n";
                jmLog << "jupiterMacro.pathSamples.size()=" << jupiterMacro.pathSamples.size()
                      << "\n";
            }
        }

        auto* mod = Mod::get();
        updater.m_tps = mod->getSavedValue<double>("updater_tps", 240.0);
        updater.m_speedhack = mod->getSavedValue<double>("updater_speedhack", 1.0);
        updater.m_lockDelta = mod->getSavedValue<bool>("updater_lockDelta", true);
        updater.m_ssbFix = mod->getSavedValue<bool>("updater_ssbFix", true);
        updater.m_backwardsStepping = mod->getSavedValue<bool>("updater_backwardsStepping", false);
        updater.m_extrapolateFrames = mod->getSavedValue<bool>("updater_extrapolateFrames", false);
        updater.m_preventDeath = mod->getSavedValue<bool>("updater_preventDeath", false);
        updater.m_autoFlipOnDeath = mod->getSavedValue<bool>("updater_autoFlipOnDeath", false);
        updater.m_speedhackAudio = mod->getSavedValue<bool>("updater_speedhackAudio", true);
        noclipEnabled = mod->getSavedValue<bool>("hack_noclip", false);
        noclipThreshold = mod->getSavedValue<float>("hack_noclipThreshold", 0.f);
        showHitboxes = mod->getSavedValue<bool>("hack_hitboxes", false);
        pathPreview = mod->getSavedValue<bool>("hack_trajectory", false);
        pathLength = mod->getSavedValue<int>("hack_trajectory_len", 312);
        layoutMode = mod->getSavedValue<bool>("hack_layoutMode", false);
        noMirrorEffect = mod->getSavedValue<bool>("hack_noMirror", false);
        audioPitchEnabled = mod->getSavedValue<bool>("hack_audioPitch", false);
        practiceRangeEnabled = mod->getSavedValue<bool>("practice_range", false);
        autosaveAtLevelEnd = mod->getSavedValue<bool>("autosave_atLevelEnd", false);
        autosaveAtInterval = mod->getSavedValue<bool>("autosave_atInterval", false);
        autosaveIntervalSec = mod->getSavedValue<double>("autosave_interval", 60.0);
        replayBackupsEnabled = mod->getSavedValue<bool>("replay_backups", true);
        replay.m_mirrorInputs = mod->getSavedValue<bool>("replay_mirrorInputs", false);
        replay.m_maintainGravity = mod->getSavedValue<bool>("replay_maintainGravity", false);
        hud.enabled = mod->getSavedValue<bool>("hud_enabled", false);
        hud.showFrame = mod->getSavedValue<bool>("hud_showFrame", true);
        hud.showTPS = mod->getSavedValue<bool>("hud_showTPS", false);

        std::error_code ec;
        for (auto& entry : fs::directory_iterator(getPresetsDir(), ec)) {
            if (entry.path().extension() != ".json")
                continue;
            BotSettingsPreset p;
            p.name = entry.path().stem().string();
            std::ifstream fin(entry.path());
            std::string json((std::istreambuf_iterator<char>(fin)), {});
            auto ext = [&](const std::string& key) -> std::string {
                auto pos = json.find("\"" + key + "\":");
                if (pos == std::string::npos)
                    return "";
                pos += key.size() + 3;
                auto end = json.find_first_of(",}", pos);
                auto val = json.substr(pos, end - pos);
                if (!val.empty() && val.front() == '"')
                    val = val.substr(1, val.size() - 2);
                return val;
            };
            try {
                p.tps = std::stod(ext("tps"));
                p.lockDelta = ext("lockDelta") == "true";
                p.ssbFix = ext("ssbFix") == "true";
                p.preventDeath = ext("preventDeath") == "true";
                p.noclip = ext("noclip") == "true";
                settingsPresets.push_back(p);
            } catch (...) {
            }
        }

        applyIntervalAutosave();

        reloadMacroList();
        enabled = true;
        log::info("[GucciBot] ========================================");
        log::info("[GucciBot] BUILD: {} | compiled {} {}", GB_BUILD_LABEL, __DATE__, __TIME__);
        log::info("[GucciBot] ========================================");
        log::info("[GucciBot] " MOD_VERSION " initialized — {} macros", storedMacros.size());

        gbcheck::run(5, 4, GBR6_VERSION, BRR_FORMAT_VERSION, MOD_VERSION);
    }

    static PauseLayer* findOpenPauseLayerRecursive(CCNode* node) {
        if (!node)
            return nullptr;
        if (auto* p = typeinfo_cast<PauseLayer*>(node))
            return p;
        if (auto* kids = node->getChildren()) {
            for (auto* child : CCArrayExt<CCNode*>(kids)) {
                if (!child)
                    continue;
                if (auto* found = findOpenPauseLayerRecursive(child))
                    return found;
            }
        }
        return nullptr;
    }
    static PauseLayer* findOpenPauseLayer() {
        return findOpenPauseLayerRecursive(CCDirector::sharedDirector()->getRunningScene());
    }

    static int
    fwComputeInputNumber(const gb::ActionAtom& atom, uint32_t frame, bool player2, bool holding) {
        int n = 0;
        for (auto const& a : atom.m_actions) {
            if (!a.isInput())
                continue;
            n++;
            if (a.m_frame == frame && a.m_player2 == player2 && a.m_holding == holding)
                return n;
        }
        return n;
    }

    static char fwGamemodeAt(GucciEngine* gb, uint32_t frame, bool player2) {
        if (frame >= gb->replay.m_pathSamples.size())
            return '?';
        return player2 ? gb->replay.m_pathSamples[frame].gamemode2
                       : gb->replay.m_pathSamples[frame].gamemode1;
    }

    static bool fwIsDashOrbType(GameObjectType type) {
        return type == GameObjectType::DashRing || type == GameObjectType::GravityDashRing;
    }

    static bool fwIsNonDashOrbType(GameObjectType type) {
        switch (type) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::CustomRing:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
        }
    }

    static void fwClassifyOrbTouch(PlayerObject* player, bool& outDash, bool& outNonDash) {
        outDash = false;
        outNonDash = false;
        if (!player || !player->m_touchingRings)
            return;
        for (auto* obj : CCArrayExt<GameObject*>(player->m_touchingRings)) {
            if (!obj)
                continue;
            if (fwIsDashOrbType(obj->m_objectType))
                outDash = true;
            else if (fwIsNonDashOrbType(obj->m_objectType))
                outNonDash = true;
        }
    }

    void GucciEngine::analyzeFrameWindows() {
        fwMarks.erase(std::remove_if(fwMarks.begin(),
                                     fwMarks.end(),
                                     [](const FrameWindowMark& mk) {
                                         return !mk.manual;
                                     }),
                      fwMarks.end());
        fwCapStack.clear();
        fwClickSamples.clear();
        fwDebugMarks.clear();

        auto shouldTestRelease = [&](uint32_t frame, bool player2) -> bool {
            char gm = fwGamemodeAt(this, frame, player2);
            if (gm == 'H' && !fwTestShipReleases)
                return false;
            return gm == 'V' || gm == 'H' || gm == 'R';
        };

        for (auto const& a : replay.m_actionAtom.m_actions) {
            if (!a.isInput())
                continue;
            if (a.m_holding)
                fwClickSamples.push_back({a.m_frame, 0.f, 0.f, a.m_player2, false});
            else if (shouldTestRelease(a.m_frame, a.m_player2))
                fwClickSamples.push_back({a.m_frame, 0.f, 0.f, a.m_player2, true});
        }

        if (fwClickSamples.empty()) {
            fwHasData = false;
            fwAnalyzeRunning = false;
            fwState = FwState::Idle;
            log::info("[GucciBot] Frame-window: no inputs to analyze");
            return;
        }

        std::sort(fwClickSamples.begin(), fwClickSamples.end(), [](auto& a, auto& b) {
            return a.frame < b.frame;
        });

        fwAnalyzeTotal = (int)fwClickSamples.size();
        fwAnalyzeCur = 0;
        fwAnalyzeProgress = 0.0f;
        fwAnalyzeRunning = true;
        fwAnalyzeStage = "starting";

        auto* pl = PlayLayer::get();
        if (!pl) {
            fwAnalyzeRunning = false;
            fwState = FwState::Idle;
            return;
        }

        fwProbeHorizon = std::max(8, fwSweepRange + 4);
        fwCapIndex = 0;
        fwXYIndex = 0;
        fwAnalyzing = true;
        fwSavedAtom = replay.m_actionAtom;

        if (auto* pause = findOpenPauseLayer()) {
            pause->onResume(nullptr);
            log::info("[GucciBot] Frame-window: dismissed open pause menu before analysis");
        }
        pl->m_isPaused = false;
        pl->m_isPracticeMode = false;
        practiceFix.m_loadCheckpoint = false;
        practiceFix.m_isBackstep = false;
        practiceFix.m_savedCheckpoints.clear();
        practiceFix.m_brokenObjects.clear();
        updater.m_fullReset = true;
        pl->resetLevel();
        updater.m_fullReset = false;
        updater.resetFrame();
        replay.m_inputIndex = 0;
        setMode(Mode::Playing);

        fwClickSamples.clear();
        for (auto const& a : fwSavedAtom.m_actions) {
            if (!a.isInput())
                continue;
            if (a.m_holding) {
                fwClickSamples.push_back({a.m_frame, 0.f, 0.f, a.m_player2, false});
                continue;
            }
            if (shouldTestRelease(a.m_frame, a.m_player2))
                fwClickSamples.push_back({a.m_frame, 0.f, 0.f, a.m_player2, true});
        }
        std::sort(fwClickSamples.begin(), fwClickSamples.end(), [](auto& a, auto& b) {
            return a.frame < b.frame;
        });
        fwSampling = false;

        fwState = FwState::Capturing;
        log::info("[GucciBot] Frame-window: async analysis started — {} inputs, "
                  "sweep +/-{}, horizon {} (paused was {})",
                  fwClickSamples.size(),
                  fwSweepRange,
                  fwProbeHorizon,
                  pl->m_isPaused ? "yes" : "no");
    }

    void GucciEngine::fwTick() {
        if (!fwAnalyzing)
            return;
        auto* pl = PlayLayer::get();
        if (!pl) {
            cancelAnalysis();
            return;
        }

        auto player1 = pl->m_player1;
        uint32_t frame = updater.getFrame();

        switch (fwState) {

        case FwState::Capturing: {
            fwAnalyzeStage = "capturing";
            if (player1 && !fwProbeDied) {
                log::info(
                    "[CAP-F] f={} steps={} x={:.1f} y={:.1f} yvel={:.1f} ground={} respawn={} "
                    "tps={:.0f}",
                    frame,
                    updater.estimatedStepCount,
                    player1->m_position.x,
                    player1->m_position.y,
                    player1->m_yVelocity,
                    player1->m_isOnGround ? 1 : 0,
                    updater.m_respawnTimer,
                    updater.m_tps);
            }
            uint32_t markerCaptureDelay = fwDelayMarkerCapture ? 1u : 0u;
            while (fwXYIndex < fwClickSamples.size() &&
                   fwClickSamples[fwXYIndex].frame + markerCaptureDelay <= frame) {
                if (auto* sp = fwClickSamples[fwXYIndex].player2 ? pl->m_player2 : pl->m_player1) {
                    fwClickSamples[fwXYIndex].x = sp->m_position.x;
                    fwClickSamples[fwXYIndex].y = sp->m_position.y;
                    if (!fwClickSamples[fwXYIndex].release) {
                        auto& samples = replay.m_pathSamples;
                        if (frame < samples.size()) {
                            auto const& gt = samples[frame];
                            fwClickSamples[fwXYIndex].orbDash =
                                fwClickSamples[fwXYIndex].player2 ? gt.p2OrbDash : gt.p1OrbDash;
                            fwClickSamples[fwXYIndex].orbNonDash = fwClickSamples[fwXYIndex].player2
                                                                       ? gt.p2OrbNonDash
                                                                       : gt.p1OrbNonDash;
                        } else {
                            fwClassifyOrbTouch(sp,
                                               fwClickSamples[fwXYIndex].orbDash,
                                               fwClickSamples[fwXYIndex].orbNonDash);
                        }
                    }
                }
                fwXYIndex++;
            }

            while (fwCapIndex < fwClickSamples.size()) {
                uint32_t clickFrame = fwClickSamples[fwCapIndex].frame;
                uint32_t margin = (uint32_t)std::max(0, fwSweepRange);
                uint32_t captureFrame = clickFrame > margin ? clickFrame - margin : 0;
                if (captureFrame > frame)
                    break;

                CheckpointObject* cp = pl->createCheckpoint();
                fwCkptCreatedThisFrame = true;
                log::info("[CAP] createCheckpoint @ f={} (click {} @ f={})",
                          frame,
                          fwCapIndex,
                          clickFrame);
                StoredFrame sf;
                sf.frame = frame;
                if (cp) {
                    cp->retain();
                    practiceFix.saveState(cp, frame);
                    if (!practiceFix.m_storedFrames.empty()) {
                        sf.state = practiceFix.m_storedFrames.back().state;
                        practiceFix.m_storedFrames.pop_back();
                    }
                }
                fwCapStack.push_back(sf);
                fwCapIndex++;
                fwAnalyzeProgress = 0.5f * (float)fwCapIndex / (float)fwClickSamples.size();
            }

            if (fwXYIndex >= fwClickSamples.size() && fwCapIndex >= fwClickSamples.size()) {
                log::info("[GucciBot] Frame-window: capture done — {} checkpoints",
                          fwCapStack.size());

                if (fwOrbAwareReleaseSkip) {
                    bool lastOrbNonDash[2] = {false, false};
                    std::vector<FwClickSample> keptSamples;
                    std::vector<StoredFrame> keptCap;
                    keptSamples.reserve(fwClickSamples.size());
                    keptCap.reserve(fwCapStack.size());
                    for (size_t i = 0; i < fwClickSamples.size(); ++i) {
                        auto const& s = fwClickSamples[i];
                        int p = s.player2 ? 1 : 0;
                        if (!s.release) {
                            lastOrbNonDash[p] = s.orbNonDash && !s.orbDash;
                            log::info("[GucciBot] Frame-window: click @ frame {} "
                                      "player{} orbDash={} orbNonDash={}",
                                      s.frame,
                                      p + 1,
                                      s.orbDash,
                                      s.orbNonDash);
                            keptSamples.push_back(s);
                            keptCap.push_back(fwCapStack[i]);
                            continue;
                        }
                        char gm = fwGamemodeAt(this, s.frame, s.player2);
                        log::info("[GucciBot] Frame-window: release @ frame {} "
                                  "player{} gm={} lastOrbNonDash={}",
                                  s.frame,
                                  p + 1,
                                  gm,
                                  lastOrbNonDash[p]);
                        if (gm == 'R' && lastOrbNonDash[p]) {
                            log::info("[GucciBot] Frame-window: release @ frame {} "
                                      "skipped -- preceding click touched a non-dash "
                                      "orb (Juice's 1a rule)",
                                      s.frame);
                            continue;
                        }
                        keptSamples.push_back(s);
                        keptCap.push_back(fwCapStack[i]);
                    }
                    fwClickSamples.swap(keptSamples);
                    fwCapStack.swap(keptCap);
                    fwAnalyzeTotal = (int)fwClickSamples.size();
                }

                {
                    int lastClickCap[2] = {-1, -1};
                    for (size_t i = 0; i < fwClickSamples.size(); ++i) {
                        auto const& s = fwClickSamples[i];
                        int p = s.player2 ? 1 : 0;
                        if (!s.release) {
                            lastClickCap[p] = (int)i;
                            continue;
                        }
                        if (lastClickCap[p] >= 0) {
                            fwCapStack[i] = fwCapStack[(size_t)lastClickCap[p]];
                        }
                    }
                }

                mode = Mode::Idle;
                fwSampling = false;
                fwProbeClick = 0;
                fwProbeShift = -1;
                fwProbePhase = 0;
                fwProbeLow = 0;
                fwProbeHigh = 0;
                fwProbeFrame = 0;
                fwState = FwState::Probing;
                muteAnalysisMusic();
                beginOrSkipProbeClick();
            }
            break;
        }

        case FwState::Probing: {
            fwAnalyzeStage = "probing";
            fwProbeFrame++;

            if (fwProbeDied || fwProbeFrame >= fwProbeHorizon) {
                bool survived = !fwProbeDied;
                if (survived && fwPositionCheckEnabled && fwProbeHasNext) {
                    auto* posPlayer = fwProbeNextPlayer2 ? pl->m_player2 : player1;
                    if (posPlayer) {
                        float dx = std::abs(posPlayer->m_position.x - fwProbeNextX);
                        float dy = std::abs(posPlayer->m_position.y - fwProbeNextY);
                        if (dx > fwPositionSlack || dy > fwPositionSlack)
                            survived = false;
                    }
                }
                log::info("[GucciBot]   probe click {} shift {:+d}: died={} "
                          "(after {} frames, horizon {}, windowHigh {})",
                          fwProbeClick,
                          fwProbeShift,
                          fwProbeDied ? "YES" : "no",
                          fwProbeFrame,
                          fwProbeHorizon,
                          fwProbeWindowHigh);
                logCalcDeathTrace(
                    fmt::format("[CONCLUDE] click={} shift={:+d} died={} frame={} horizon={} "
                                "windowHigh={} survived={}",
                                fwProbeClick,
                                fwProbeShift,
                                fwProbeDied ? 1 : 0,
                                fwProbeFrame,
                                fwProbeHorizon,
                                fwProbeWindowHigh,
                                survived ? 1 : 0));
                if (fwUseRecoveryRangeAlgorithm)
                    advanceRecoverySweep(survived);
                else
                    advanceOffsetSweep(survived);
            }
            break;
        }

        case FwState::DebugPause: {
            fwAnalyzeStage = "debug pause";
            if (fwDebugPauseRemaining > 0) {
                fwDebugPauseRemaining--;
                break;
            }
            fwState = FwState::Probing;
            beginShiftTest();
            break;
        }

        case FwState::Finishing:
        case FwState::Idle:
        default:
            break;
        }
    }

    void GucciEngine::computeProbeHorizon() {
        fwProbeHorizon = std::max(16, fwMaxFramesMeasured);
        long margin = 0;
        if (fwProbeClick < fwCapStack.size())
            margin =
                (long)fwClickSamples[fwProbeClick].frame - (long)fwCapStack[fwProbeClick].frame;
        fwProbeHorizon += (int)std::max(0L, margin);
    }

    void GucciEngine::beginShiftTest() {
        if (fwProbeTestedShifts.count(fwProbeShift)) {
            log::error("[GucciBot] Frame-window: shift {:+d} already tested for click {} -- "
                       "dedup guard caught a repeat, ending this click's sweep early "
                       "instead of risking an inflated window",
                       fwProbeShift,
                       fwProbeClick);
            finishProbeClick();
            return;
        }
        fwProbeTestedShifts.insert(fwProbeShift);

        if (fwUseRecoveryRangeAlgorithm) {
            beginShiftTestRecovery();
            return;
        }

        if (fwProbeHasNext) {
            int64_t originalGap = std::max<int64_t>(
                (int64_t)fwProbeNextFrame - (int64_t)fwClickSamples[fwProbeClick].frame, 0);
            // Slack SHORTENS the required survival distance, it does not extend
            // it (surviving gap-slack frames counts as a pass, not gap+slack) --
            // this was previously the other sign and produced systematically
            // inflated windows. Don't flip this back without a specific report
            // asking for it.
            long high = (long)std::max<int64_t>(originalGap - fwSlackWindow, 0);
            long warmup = 0;
            if (fwProbeClick < fwCapStack.size())
                warmup =
                    (long)fwClickSamples[fwProbeClick].frame - (long)fwCapStack[fwProbeClick].frame;
            warmup = std::max(0L, warmup);
            const int kMaxHorizon = std::max(16, fwMaxFramesMeasured);
            const int kMinHorizon = 12;
            fwProbeWindowHigh = (int)high;
            fwProbeHorizon = (int)std::clamp(high, (long)kMinHorizon, (long)kMaxHorizon) +
                             (int)std::max(0L, warmup + (long)fwProbeShift);
        } else {
            computeProbeHorizon();
        }
        beginProbeRun();
    }

    void GucciEngine::advanceOffsetSweep(bool survived) {
        if (survived)
            fwProbeValidCount++;

        if (fwProbePhase == -1) {
            if (survived) {
                fwProbeLow = 0;
                fwProbeHigh = 0;
            } else {
                fwProbeNegContiguous = false;
                fwProbePosContiguous = false;
            }
            if (fwProbeMaxNegShift > 0) {
                fwProbePhase = 0;
                fwProbeShift = -1;
            } else if (fwProbeMaxPosShift > 0) {
                fwProbePhase = 1;
                fwProbeShift = 1;
            } else {
                finishProbeClick();
                return;
            }
            debugPauseOrContinue(survived);
            return;
        }

        if (fwProbePhase == 0) {
            if (survived) {
                if (fwProbeNegContiguous)
                    fwProbeLow = fwProbeShift;
            } else {
                fwProbeNegContiguous = false;
            }
            bool keepGoing = fwFullRangeSweep || survived;
            if (keepGoing && fwProbeShift - 1 >= -fwProbeMaxNegShift) {
                fwProbeShift--;
            } else {
                fwProbePhase = 1;
                fwProbeShift = 1;
                if (fwProbeMaxPosShift <= 0) {
                    finishProbeClick();
                    return;
                }
            }
        } else {
            if (survived) {
                if (fwProbePosContiguous)
                    fwProbeHigh = fwProbeShift;
            } else {
                fwProbePosContiguous = false;
            }
            bool keepGoing = fwFullRangeSweep || survived;
            if (keepGoing && fwProbeShift + 1 <= fwProbeMaxPosShift) {
                fwProbeShift++;
            } else {
                finishProbeClick();
                return;
            }
        }
        debugPauseOrContinue(survived);
    }

    void GucciEngine::debugPauseOrContinue(bool survived) {
        if (!fwDebugMode) {
            beginShiftTest();
            return;
        }
        auto* pl = PlayLayer::get();
        auto* p = pl ? pl->m_player1 : nullptr;

        auto const& sample = fwClickSamples[fwProbeClick];
        FwDebugMark mk;
        mk.x = p ? p->m_position.x : 0.f;
        mk.y = p ? p->m_position.y : 0.f;
        mk.survived = survived;
        mk.macroFrame = sample.frame;
        mk.isRelease = sample.release;
        mk.player2 = sample.player2;
        mk.clickIndex = fwProbeClick;
        mk.testedFrame = (uint32_t)std::max<int64_t>((int64_t)sample.frame + fwProbeShift, 0);
        mk.inputNumber =
            fwComputeInputNumber(fwSavedAtom, sample.frame, sample.player2, !sample.release);
        fwDebugMarks.push_back(mk);

        fwDebugPauseRemaining = fwDebugSlowdown;
        fwState = FwState::DebugPause;
    }

    void GucciEngine::debugTeleportToMark(size_t markIndex) {
        if (markIndex >= fwDebugMarks.size())
            return;
        auto const& mk = fwDebugMarks[markIndex];
        if (mk.clickIndex >= fwCapStack.size() || mk.clickIndex >= fwClickSamples.size())
            return;
        auto* pl = PlayLayer::get();
        if (!pl)
            return;

        if (fwAnalyzing)
            cancelAnalysis();

        replay.m_actionAtom = fwSavedAtom;
        auto& acts = replay.m_actionAtom.m_actions;
        acts.erase(std::remove_if(acts.begin(),
                                  acts.end(),
                                  [](const gb::Action& a) {
                                      return !a.isInput();
                                  }),
                   acts.end());

        for (auto& a : acts) {
            if (a.m_frame == mk.macroFrame && a.m_player2 == mk.player2 &&
                a.m_holding == !mk.isRelease) {
                a.m_frame = mk.testedFrame;
                break;
            }
        }
        std::stable_sort(acts.begin(), acts.end(), [](const gb::Action& a, const gb::Action& b) {
            return a.m_frame < b.m_frame;
        });

        practiceFix.m_savedCheckpoints.clear();
        practiceFix.m_brokenObjects.clear();
        practiceFix.m_storedFrames.clear();
        practiceFix.m_storedFrames.push_back(fwCapStack[mk.clickIndex]);
        practiceFix.m_storedFrames.push_back(fwCapStack[mk.clickIndex]);

        mode = Mode::Playing;
        if (practiceFix.canRestoreState()) {
            practiceFix.m_loadCheckpoint = true;
            practiceFix.m_isBackstep = true;
            pl->resetLevel();
            practiceFix.m_loadCheckpoint = false;
            practiceFix.m_isBackstep = false;
        }

        log::info("[GucciBot] Frame-window debug: teleported to mark {} (macroFrame={}, "
                  "testedFrame={})",
                  markIndex,
                  mk.macroFrame,
                  mk.testedFrame);
    }

    void GucciEngine::beginProbeRun() {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        if (fwProbeClick >= fwCapStack.size() ||
            fwCapStack[fwProbeClick].frame > fwClickSamples[fwProbeClick].frame) {
            finishProbeClick();
            return;
        }

        replay.m_actionAtom = fwSavedAtom;
        auto& acts = replay.m_actionAtom.m_actions;
        acts.erase(std::remove_if(acts.begin(),
                                  acts.end(),
                                  [](const gb::Action& a) {
                                      return !a.isInput();
                                  }),
                   acts.end());

        uint32_t targetFrame = fwClickSamples[fwProbeClick].frame;
        bool targetP2 = fwClickSamples[fwProbeClick].player2;
        bool targetHolding = !fwClickSamples[fwProbeClick].release;
        for (auto& a : acts) {
            if (a.m_frame == targetFrame && a.m_player2 == targetP2 &&
                a.m_holding == targetHolding) {
                int64_t shifted = (int64_t)targetFrame + fwProbeShift;
                a.m_frame = (uint32_t)std::max<int64_t>(shifted, 0);
                break;
            }
        }

        std::stable_sort(acts.begin(), acts.end(), [](const gb::Action& a, const gb::Action& b) {
            return a.m_frame < b.m_frame;
        });

        practiceFix.m_savedCheckpoints.clear();
        practiceFix.m_brokenObjects.clear();
        practiceFix.m_storedFrames.clear();
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);

        fwProbeDied = false;
        fwProbeFrame = 0;

        mode = Mode::Playing;

        if (practiceFix.canRestoreState()) {
            practiceFix.m_loadCheckpoint = true;
            practiceFix.m_isBackstep = true;
            log::info("[CAP-RESTORE] click={} shift={} targetFrame={} cpFrame={}",
                      fwProbeClick,
                      fwProbeShift,
                      targetFrame,
                      fwCapStack[fwProbeClick].frame);
            pl->resetLevel();
            practiceFix.m_loadCheckpoint = false;
            practiceFix.m_isBackstep = false;
        }
    }

    void GucciEngine::beginShiftTestRecovery() {
        fwProbeSubPhase = FwProbeSubPhase::Reaching;
        if (!fwProbeHasNext) {
            computeProbeHorizon();
            beginProbeRun();
            return;
        }
        int64_t shiftedIFrame =
            std::max<int64_t>((int64_t)fwClickSamples[fwProbeClick].frame + fwProbeShift, 0);
        int64_t horizonToN = std::max<int64_t>((int64_t)fwProbeNextFrame - shiftedIFrame, 0);
        long margin = 0;
        if (fwProbeClick < fwCapStack.size())
            margin =
                (long)fwClickSamples[fwProbeClick].frame - (long)fwCapStack[fwProbeClick].frame;
        margin = std::max(0L, margin);
        fwProbeHorizon = (int)horizonToN + (int)margin;
        beginProbeRunReach();
    }

    void GucciEngine::beginProbeRunReach() {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        if (fwProbeClick >= fwCapStack.size() ||
            fwCapStack[fwProbeClick].frame > fwClickSamples[fwProbeClick].frame) {
            finishProbeClick();
            return;
        }

        replay.m_actionAtom = fwSavedAtom;
        auto& acts = replay.m_actionAtom.m_actions;
        acts.erase(std::remove_if(acts.begin(),
                                  acts.end(),
                                  [](const gb::Action& a) {
                                      return !a.isInput();
                                  }),
                   acts.end());

        uint32_t targetFrame = fwClickSamples[fwProbeClick].frame;
        bool targetP2 = fwClickSamples[fwProbeClick].player2;
        bool targetHolding = !fwClickSamples[fwProbeClick].release;
        for (auto& a : acts) {
            if (a.m_frame == targetFrame && a.m_player2 == targetP2 &&
                a.m_holding == targetHolding) {
                int64_t shifted = (int64_t)targetFrame + fwProbeShift;
                a.m_frame = (uint32_t)std::max<int64_t>(shifted, 0);
                break;
            }
        }

        if (fwProbeHasNext) {
            acts.erase(std::remove_if(acts.begin(),
                                      acts.end(),
                                      [&](const gb::Action& a) {
                                          return a.m_frame == fwProbeNextFrame &&
                                                 a.m_player2 == fwProbeNextPlayer2 &&
                                                 a.m_holding == !fwProbeNextIsRelease;
                                      }),
                       acts.end());
        }

        std::stable_sort(acts.begin(), acts.end(), [](const gb::Action& a, const gb::Action& b) {
            return a.m_frame < b.m_frame;
        });

        practiceFix.m_savedCheckpoints.clear();
        practiceFix.m_brokenObjects.clear();
        practiceFix.m_storedFrames.clear();
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);

        fwProbeDied = false;
        fwProbeFrame = 0;

        mode = Mode::Playing;

        if (practiceFix.canRestoreState()) {
            practiceFix.m_loadCheckpoint = true;
            practiceFix.m_isBackstep = true;
            pl->resetLevel();
            practiceFix.m_loadCheckpoint = false;
            practiceFix.m_isBackstep = false;
        }
    }

    void GucciEngine::beginRecoveryCandidate() {
        auto* pl = PlayLayer::get();
        if (!pl)
            return;
        if (fwProbeClick >= fwCapStack.size() ||
            fwCapStack[fwProbeClick].frame > fwClickSamples[fwProbeClick].frame) {
            finishProbeClick();
            return;
        }

        replay.m_actionAtom = fwSavedAtom;
        auto& acts = replay.m_actionAtom.m_actions;
        acts.erase(std::remove_if(acts.begin(),
                                  acts.end(),
                                  [](const gb::Action& a) {
                                      return !a.isInput();
                                  }),
                   acts.end());

        uint32_t targetFrame = fwClickSamples[fwProbeClick].frame;
        bool targetP2 = fwClickSamples[fwProbeClick].player2;
        bool targetHolding = !fwClickSamples[fwProbeClick].release;
        for (auto& a : acts) {
            if (a.m_frame == targetFrame && a.m_player2 == targetP2 &&
                a.m_holding == targetHolding) {
                int64_t shifted = (int64_t)targetFrame + fwProbeShift;
                a.m_frame = (uint32_t)std::max<int64_t>(shifted, 0);
                break;
            }
        }

        int64_t shiftedNFrame = fwProbeNextFrame;
        if (fwProbeHasNext) {
            for (auto& a : acts) {
                if (a.m_frame == fwProbeNextFrame && a.m_player2 == fwProbeNextPlayer2 &&
                    a.m_holding == !fwProbeNextIsRelease) {
                    shiftedNFrame =
                        std::max<int64_t>((int64_t)fwProbeNextFrame + fwRecoveryOffset, 0);
                    a.m_frame = (uint32_t)shiftedNFrame;
                    break;
                }
            }
        }

        std::stable_sort(acts.begin(), acts.end(), [](const gb::Action& a, const gb::Action& b) {
            return a.m_frame < b.m_frame;
        });

        practiceFix.m_savedCheckpoints.clear();
        practiceFix.m_brokenObjects.clear();
        practiceFix.m_storedFrames.clear();
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);
        practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);

        fwProbeDied = false;
        fwProbeFrame = 0;

        int64_t shiftedIFrame = std::max<int64_t>((int64_t)targetFrame + fwProbeShift, 0);
        int64_t horizon = std::max<int64_t>(shiftedNFrame - shiftedIFrame, 0) + fwSlackWindow;
        long margin = 0;
        if (fwProbeClick < fwCapStack.size())
            margin = (long)targetFrame - (long)fwCapStack[fwProbeClick].frame;
        margin = std::max(0L, margin);
        fwProbeHorizon = (int)horizon + (int)margin;

        mode = Mode::Playing;

        if (practiceFix.canRestoreState()) {
            practiceFix.m_loadCheckpoint = true;
            practiceFix.m_isBackstep = true;
            pl->resetLevel();
            practiceFix.m_loadCheckpoint = false;
            practiceFix.m_isBackstep = false;
        }
    }

    void GucciEngine::advanceRecoverySweep(bool survived) {
        if (fwProbeSubPhase == FwProbeSubPhase::Reaching) {
            if (!fwProbeHasNext) {
                advanceOffsetSweep(survived);
                return;
            }
            if (!survived) {
                advanceOffsetSweep(false);
                return;
            }
            fwProbeSubPhase = FwProbeSubPhase::RecoveryCandidate;
            fwRecoveryOffset = -fwRecoveryRange;
            beginRecoveryCandidate();
            return;
        }

        if (survived) {
            advanceOffsetSweep(true);
            return;
        }
        fwRecoveryOffset++;
        if (fwRecoveryOffset > fwRecoveryRange) {
            advanceOffsetSweep(false);
            return;
        }
        beginRecoveryCandidate();
    }

    void GucciEngine::finishProbeClick() {
        int window = fwProbeValidCount;
        const float levelLen = m_levelLength > 0.f ? m_levelLength : 1.f;

        FrameWindowMark mk;
        mk.x = fwClickSamples[fwProbeClick].x;
        mk.y = fwClickSamples[fwProbeClick].y;
        mk.window = window;
        mk.player2 = fwClickSamples[fwProbeClick].player2;
        mk.frame = fwClickSamples[fwProbeClick].frame;
        mk.percent = std::clamp(fwClickSamples[fwProbeClick].x / levelLen * 100.f, 0.f, 100.f);
        mk.isRelease = fwClickSamples[fwProbeClick].release;
        fwMarks.push_back(mk);

        log::info("[GucciBot] Frame-window: click {} @ frame {} ({:.1f}%) window={} "
                  "(contiguous span {}..{})",
                  fwProbeClick,
                  fwClickSamples[fwProbeClick].frame,
                  mk.percent,
                  window,
                  fwProbeLow,
                  fwProbeHigh);

        fwProbeClick++;
        beginOrSkipProbeClick();
    }

    void GucciEngine::beginOrSkipProbeClick() {
        while (fwProbeClick < fwClickSamples.size() &&
               fwHasManualMarkAt(fwClickSamples[fwProbeClick].frame,
                                 fwClickSamples[fwProbeClick].player2)) {
            log::info(
                "[GucciBot] Frame-window: click {} @ frame {} has a manual mark, skipping probe",
                fwProbeClick,
                fwClickSamples[fwProbeClick].frame);
            fwProbeClick++;
        }

        fwAnalyzeCur = (int)fwProbeClick;
        fwAnalyzeProgress = fwClickSamples.empty()
                                ? 1.0f
                                : 0.5f + 0.5f * (float)fwProbeClick / (float)fwClickSamples.size();

        if (fwProbeClick >= fwClickSamples.size()) {
            fwFinishAnalysis();
            return;
        }

        size_t nextIdx = fwProbeClick + 1;
        fwProbeHasNext = nextIdx < fwClickSamples.size();
        if (fwProbeHasNext) {
            fwProbeNextFrame = fwClickSamples[nextIdx].frame;
            fwProbeNextIsRelease = fwClickSamples[nextIdx].release;
            fwProbeNextPlayer2 = fwClickSamples[nextIdx].player2;
            fwProbeNextX = fwClickSamples[nextIdx].x;
            fwProbeNextY = fwClickSamples[nextIdx].y;
        }

        uint32_t clickFrame = fwClickSamples[fwProbeClick].frame;
        fwProbeMaxPosShift = fwSweepRange;
        if (fwProbeHasNext) {
            long room = (long)fwProbeNextFrame - (long)clickFrame - 1;
            fwProbeMaxPosShift = (int)std::clamp((long)fwSweepRange, 0L, std::max(0L, room));
        }
        fwProbeMaxNegShift = fwSweepRange;
        if (fwProbeClick > 0) {
            long room = (long)clickFrame - (long)fwClickSamples[fwProbeClick - 1].frame - 1;
            fwProbeMaxNegShift = (int)std::clamp((long)fwSweepRange, 0L, std::max(0L, room));
        }

        fwProbeLow = 0;
        fwProbeHigh = 0;
        fwProbeNegContiguous = true;
        fwProbePosContiguous = true;
        fwProbeValidCount = 0;
        fwProbeTestedShifts.clear();

        fwProbeShift = 0;
        fwProbePhase = -1;
        beginShiftTest();
    }

    void GucciEngine::fwFinishAnalysis() {
        fwState = FwState::Finishing;
        auto* pl = PlayLayer::get();

        practiceFix.m_storedFrames.clear();
        practiceFix.m_loadCheckpoint = false;
        practiceFix.m_isBackstep = false;
        if (pl) {
            updater.m_fullReset = true;
            pl->resetLevel();
            updater.m_fullReset = false;
        }
        updater.resetFrame();
        setMode(Mode::Idle);

        replay.m_actionAtom = fwSavedAtom;
        replay.m_inputIndex = 0;
        unmuteAnalysisMusic();

        fwAnalyzing = false;
        fwProbeDied = false;
        fwAnalyzeRunning = false;
        fwAnalyzeProgress = 1.0f;
        fwAnalyzeStage = "done";
        fwHasData = !fwMarks.empty();
        saveFwMarks(replay.getCurrentPath());
        fwState = FwState::Idle;

        log::info("[GucciBot] Frame-window: analysis complete — {} marks, macro restored",
                  fwMarks.size());
    }

    void GucciEngine::muteAnalysisMusic() {
        if (fwMusicMuted)
            return;
        auto* fmod = FMODAudioEngine::sharedEngine();
        if (!fmod)
            return;
        fwSavedMusicVolume = fmod->getBackgroundMusicVolume();
        fwSavedEffectsVolume = fmod->getEffectsVolume();
        fmod->setBackgroundMusicVolume(0.f);
        fmod->setEffectsVolume(0.f);
        fwMusicMuted = true;
    }
    void GucciEngine::unmuteAnalysisMusic() {
        if (!fwMusicMuted)
            return;
        auto* fmod = FMODAudioEngine::sharedEngine();
        if (fmod) {
            fmod->setBackgroundMusicVolume(fwSavedMusicVolume);
            fmod->setEffectsVolume(fwSavedEffectsVolume);
        }
        fwMusicMuted = false;
    }

    void GucciEngine::cancelAnalysis() {
        if (!fwAnalyzing)
            return;
        fwState = FwState::Finishing;

        practiceFix.m_storedFrames.clear();
        practiceFix.m_loadCheckpoint = false;
        practiceFix.m_isBackstep = false;
        if (auto* pl = PlayLayer::get()) {
            updater.m_fullReset = true;
            pl->resetLevel();
            updater.m_fullReset = false;
            updater.resetFrame();
        }

        replay.m_actionAtom = fwSavedAtom;
        replay.m_inputIndex = 0;
        unmuteAnalysisMusic();

        fwAnalyzing = false;
        fwProbeDied = false;
        fwAnalyzeRunning = false;
        fwAnalyzeStage = "cancelled";
        fwState = FwState::Idle;
        mode = Mode::Idle;
        if (userTpsSaved > 0.0) {
            updater.setTps(userTpsSaved);
            userTpsSaved = 0.0;
        }

        fwHasData = !fwMarks.empty();
        saveFwMarksNow();

        log::info("[GucciBot] Frame-window: analysis cancelled — {} result(s) kept",
                  fwMarks.size());
    }

} // namespace gucci
