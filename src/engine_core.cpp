#include "GucciBot.hpp"
#include <fmt/format.h>
#include "brr_format.hpp"
#include "gbr6_format.hpp"
#include "selfcheck.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace geode::prelude;
namespace fs = std::filesystem;

GucciScheduler::JobId GucciScheduler::schedule(double interval, std::function<void()> fn) {
    JobId id = m_nextId++;
    m_jobs[id] = {interval, 0.0, fn};
    return id;
}
void GucciScheduler::unschedule(JobId id) { m_jobs.erase(id); }
void GucciScheduler::reschedule(JobId id, double interval) {
    auto it = m_jobs.find(id);
    if (it != m_jobs.end()) it->second.interval = interval;
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
    if (!cp) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;

    SavedCheckpointState state;
    state.m_checkpoint  = cp;
    state.m_frameOffset = frameOffset;
    state.m_gameState   = pl->m_gameState;

    auto* p1 = pl->m_player1;
    auto* p2 = pl->m_player2;

    state.m_p1Position      = p1->m_position;
    state.m_p1Rotation      = p1->getRotation();
    state.m_p1XVel          = p1->m_playerSpeed;
    state.m_p1YVel          = p1->m_yVelocity;
    state.m_p1IsUpsideDown  = p1->m_isUpsideDown;
    state.m_p1JumpBuffered  = p1->m_jumpBuffered;
    state.m_p1IsOnGround    = p1->m_isOnGround;
    state.m_p1GameMode      = 0;

    state.m_p2Position      = p2->m_position;
    state.m_p2Rotation      = p2->getRotation();
    state.m_p2XVel          = p2->m_playerSpeed;
    state.m_p2YVel          = p2->m_yVelocity;
    state.m_p2IsUpsideDown  = p2->m_isUpsideDown;
    state.m_p2JumpBuffered  = p2->m_jumpBuffered;
    state.m_p2IsOnGround    = p2->m_isOnGround;
    state.m_p2GameMode      = 0;

    m_savedCheckpoints.push_back(state);

        StoredFrame sf;
    sf.state = state;
    sf.frame = GucciEngine::get()->updater.getFrame();
    m_storedFrames.push_back(sf);
}

void GucciPracticeFix::saveState(CheckpointObject* cp, uint64_t frameOffset) {
    saveCurrent(cp, frameOffset);
}

void GucciPracticeFix::restorePreviousFrame(std::function<void(CheckpointObject*)> loadFn) {
    if (m_storedFrames.size() <= 1) return;
    m_storedFrames.pop_back();
    auto& prev = m_storedFrames.back();
    if (prev.state.m_checkpoint)
        loadFn(prev.state.m_checkpoint);
    applyCheckpoint(prev.state);
}

void GucciPracticeFix::applyLatest() {
    if (m_savedCheckpoints.empty()) return;
    applyCheckpoint(m_savedCheckpoints.back());
}

void GucciPracticeFix::applyCheckpoint(const SavedCheckpointState& state) {
    auto* pl = PlayLayer::get();
    if (!pl) return;

    pl->m_gameState = state.m_gameState;

    auto* p1 = pl->m_player1;
    auto* p2 = pl->m_player2;
    if (p1) {
        p1->setPosition(state.m_p1Position);
        p1->setRotation(state.m_p1Rotation);
        p1->m_isUpsideDown = state.m_p1IsUpsideDown;
                        p1->m_playerSpeed = state.m_p1XVel;
        p1->m_yVelocity   = state.m_p1YVel;
    }
    if (p2) {
        p2->setPosition(state.m_p2Position);
        p2->setRotation(state.m_p2Rotation);
        p2->m_isUpsideDown = state.m_p2IsUpsideDown;
        p2->m_playerSpeed = state.m_p2XVel;
        p2->m_yVelocity   = state.m_p2YVel;
    }
}

void GucciPracticeFix::dropLastStoredFrame() {
    if (!m_storedFrames.empty()) m_storedFrames.pop_back();
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
    if (m_inputIndex >= m_actionAtom.length()) return std::nullopt;
    return m_actionAtom.m_actions[m_inputIndex];
}

std::optional<gb::Action> GucciReplaySystem::getNextInput(uint32_t frame) {
    if (m_inputIndex >= m_actionAtom.length()) return std::nullopt;
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
            size_t after = m_actionAtom.length();
            m_inputIndex = m_actionAtom.length();
            if (before != after)
                log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                          "— deleted {} stale input(s) after checkpoint (kept {})",
                          deathFrame, respawnFrame, before - after, after);
            else
                log::info("[GucciBot] Recording: died@{}, respawn@{} (checkpoint) "
                          "— nothing after checkpoint to delete (kept all {})",
                          deathFrame, respawnFrame, after);
        } else {
                                                                                                                                    m_actionAtom.m_actions.clear();
            m_inputIndex = 0;
            log::info("[GucciBot] Recording: died@{}, full restart (no checkpoint) "
                      "— cleared {} input(s), re-recording from frame 0",
                      deathFrame, before);
        }
    } else {
        if (m_actionAtom.empty()) { m_inputIndex = 0; return; }
        m_inputIndex = static_cast<size_t>(std::distance(
            m_actionAtom.m_actions.begin(),
            std::find_if(m_actionAtom.m_actions.begin(), m_actionAtom.m_actions.end(),
                [respawnFrame](const gb::Action& a){ return a.m_frame >= respawnFrame; })));
    }
}

fs::path GucciReplaySystem::getCurrentPath() const {
    auto* gb = GucciEngine::get();
    auto  dir = gb->getReplayDir();
    return dir / (m_replayName + ".brrr");
}

void GucciReplaySystem::backupExisting(const fs::path& path) {
    if (!fs::exists(path)) return;
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

static void savePathSamples(const fs::path& macroPath, const std::vector<MacroPathSample>& samples) {
    auto sc = pathSamplesSidecarPath(macroPath);
    std::error_code ec;
    if (samples.empty()) { fs::remove(sc, ec); return; }
    std::ofstream f(sc, std::ios::binary);
    if (!f) return;
    f.write("GBPS", 4);
    uint8_t ver = 1; f.write((const char*)&ver, 1);
    uint32_t n = (uint32_t)samples.size(); f.write((const char*)&n, 4);
    for (auto const& s : samples) {
        f.write((const char*)&s.p1x, 4);
        f.write((const char*)&s.p1y, 4);
        f.write((const char*)&s.p2x, 4);
        f.write((const char*)&s.p2y, 4);
        f.write(&s.gamemode1, 1);
        f.write(&s.gamemode2, 1);
    }
}

static void loadPathSamples(const fs::path& macroPath, std::vector<MacroPathSample>& samples) {
    samples.clear();
    auto sc = pathSamplesSidecarPath(macroPath);
    if (!fs::exists(sc)) return;
    std::ifstream f(sc, std::ios::binary);
    if (!f) return;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "GBPS", 4) != 0) return;
    uint8_t ver = 0; f.read((char*)&ver, 1);
    if (ver != 1) return;
    uint32_t n = 0; f.read((char*)&n, 4);
    samples.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        MacroPathSample s;
        f.read((char*)&s.p1x, 4); f.read((char*)&s.p1y, 4);
        f.read((char*)&s.p2x, 4); f.read((char*)&s.p2y, 4);
        f.read(&s.gamemode1, 1); f.read(&s.gamemode2, 1);
        if (!f) break;
        samples.push_back(s);
    }
    log::info("[GucciBot] Macro path: loaded {} sample(s) from sidecar", samples.size());
}

static void saveFwMarks(const fs::path& macroPath) {
    auto* gb = GucciEngine::get();
    auto sc  = fwSidecarPath(macroPath);
    std::error_code ec;
    if (gb->fwMarks.empty()) { fs::remove(sc, ec); return; }
    std::ofstream f(sc, std::ios::binary);
    if (!f) return;
    f.write("GBFW", 4);
    uint8_t ver = 1; f.write((const char*)&ver, 1);
    uint32_t n = (uint32_t)gb->fwMarks.size(); f.write((const char*)&n, 4);
    for (auto const& mk : gb->fwMarks) {
        int32_t w  = mk.window;
        uint8_t p2 = mk.player2 ? 1 : 0;
        f.write((const char*)&mk.x, 4);
        f.write((const char*)&mk.y, 4);
        f.write((const char*)&w, 4);
        f.write((const char*)&p2, 1);
        f.write((const char*)&mk.frame, 4);
        f.write((const char*)&mk.percent, 4);
    }
}

static void loadFwMarks(const fs::path& macroPath) {
    auto* gb = GucciEngine::get();
    gb->fwMarks.clear();
    gb->fwHasData = false;
    auto sc = fwSidecarPath(macroPath);
    if (!fs::exists(sc)) return;
    std::ifstream f(sc, std::ios::binary);
    if (!f) return;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "GBFW", 4) != 0) return;
    uint8_t ver = 0; f.read((char*)&ver, 1);
    if (ver != 1) return;
    uint32_t n = 0; f.read((char*)&n, 4);
    for (uint32_t i = 0; i < n; ++i) {
        float x = 0, y = 0, pct = 0; int32_t w = 0; uint8_t p2 = 0; uint32_t fr = 0;
        f.read((char*)&x, 4); f.read((char*)&y, 4); f.read((char*)&w, 4);
        f.read((char*)&p2, 1); f.read((char*)&fr, 4); f.read((char*)&pct, 4);
        if (!f) break;
        gb->fwMarks.push_back({ x, y, (int)w, p2 != 0, fr, pct });
    }
    gb->fwHasData = !gb->fwMarks.empty();
    log::info("[GucciBot] Frame-window: loaded {} persisted mark(s) from sidecar",
              gb->fwMarks.size());
}

void GucciReplaySystem::save(const fs::path& path, bool noOverwrite) {
    if (noOverwrite && fs::exists(path)) return;

    auto* gb = GucciEngine::get();

        std::vector<GBR6Input> p1, p2;
    for (auto& a : m_actionAtom.m_actions) {
        if (!a.isInput()) continue;
        GBR6Input inp;
        inp.frame   = a.m_frame;
        inp.button  = static_cast<uint8_t>(a.m_type);
        inp.pressed = a.m_holding;
        inp.player2 = a.m_player2;
        if (a.m_player2) p2.push_back(inp);
        else             p1.push_back(inp);
    }

    GBR6Header hdr;
    hdr.tps       = static_cast<float>(gb->updater.m_tps);
    hdr.name      = m_replayName;
    hdr.rngSeed   = m_startingSeed;
    hdr.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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
            f.deaths.push_back({ a.m_frame, static_cast<uint8_t>(t) });
        }
    }
    if (!f.deaths.empty()) f.header.flags |= GBR6_HAS_DEATHS;

    if (!f.saveToPath(path)) {
        log::error("[GucciBot] Failed to save to {}", path.string());
    } else if (!f.deaths.empty()) {
        log::info("[GucciBot] Saved with {} intentional-death marker(s)", f.deaths.size());
    }
    saveFwMarks(path);
    savePathSamples(path, m_pathSamples);
}

void GucciReplaySystem::load(const fs::path& path) {
    if (!fs::exists(path)) {
        log::error("[GucciBot] File not found: {}", path.string());
        return;
    }

    auto* gb = GucciEngine::get();

        char magic[4] = {};
    { std::ifstream f(path, std::ios::binary); f.read(magic, 4); }

    if (std::memcmp(magic, "GBR6", 4) == 0) {
        auto result = GBR6File::loadFromPath(path);
        if (!result) { log::error("[GucciBot] Failed to load GBR6: {}", path.string()); return; }
        auto& f = *result;
        gb->updater.setTps(f.header.tps);
        m_startingSeed = f.header.rngSeed;
        m_replayName   = f.header.name;
        gb->loadedMacroLevelName = f.header.levelName;
        m_actionAtom.clear();
        m_inputIndex = 0;
        for (auto& inp : f.p1Inputs)
            m_actionAtom.addAction(inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, false);
        for (auto& inp : f.p2Inputs)
            m_actionAtom.addAction(inp.frame, static_cast<gb::ActionType>(inp.button), inp.pressed, true);
                                for (auto& d : f.deaths)
            m_actionAtom.addAction(d.frame, static_cast<gb::ActionType>(d.type), false, false);
        std::sort(m_actionAtom.m_actions.begin(), m_actionAtom.m_actions.end());
        gb->setMode(GucciEngine::Mode::Playing);
        log::info("[GucciBot] Loaded GBR6: {} inputs, {} death marker(s)",
                  m_actionAtom.length(), f.deaths.size());
        loadFwMarks(path);
        loadPathSamples(path, m_pathSamples);
        return;
    }

        auto* legacy = BRRMacro::loadFromDisk(path.stem().string());
    if (legacy && !legacy->inputs.empty()) {
        m_actionAtom.clear(); m_inputIndex = 0;
        for (auto& inp : legacy->inputs) {
            gb::ActionType t = gb::ActionType::Jump;
            if (inp.actionType == 2) t = gb::ActionType::Left;
            if (inp.actionType == 3) t = gb::ActionType::Right;
            m_actionAtom.addAction(static_cast<uint32_t>(inp.tick), t, inp.isPressed(), inp.isPlayer2());
        }
        gb->updater.setTps(legacy->framerate);
        gb->setMode(GucciEngine::Mode::Playing);
        log::info("[GucciBot] Loaded legacy BRR: {} inputs", m_actionAtom.length());
        loadFwMarks(path);
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
    if (m == Mode::Playing) {
                                if (prev != Mode::Playing) userTpsSaved = updater.m_tps;
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
    if (!isPlaying() || replay.m_actionAtom.empty()) return false;
    auto* pl = PlayLayer::get();
    if (!pl) return false;

    uint32_t now = updater.getFrame();
    auto& atom = replay.m_actionAtom;
    atom.clipFrom(now);

        bool held[2][4] = {};
    for (auto const& a : atom.m_actions)
        if (a.isInput()) held[a.m_player2 ? 1 : 0][(uint8_t)a.m_type] = a.m_holding;

    setMode(Mode::Recording);

            for (int p = 0; p < 2; ++p)
        for (int b = 1; b <= 3; ++b)
            if (held[p][b]) pl->handleButton(false, b, p == 0);

    log::info("[GucciBot] Resume recording @ frame {} — {} actions kept",
              now, atom.m_actions.size());
    return true;
}

void GucciEngine::reloadMacroList() {
    storedMacros.clear(); incompatibleMacros.clear();
    jaMacros.clear(); giddeyMacros.clear(); toosiiMacros.clear();
    bamMacros.clear(); sexyyMacros.clear();

    auto dir = getReplayDir();
    if (!fs::exists(dir)) { fs::create_directories(dir); return; }

    std::error_code ec;
    for (auto& it : fs::directory_iterator(dir, ec)) {
        if (!it.is_regular_file()) continue;
        auto ext  = it.path().extension().string();
        auto stem = it.path().stem().string();
        if (ext == ".brrr" || ext == ".toosii" || ext == ".ja" ||
            ext == ".giddey" || ext == ".bam" || ext == ".sexyy") {
            storedMacros.push_back(stem);
            if (ext == ".ja")     jaMacros.insert(stem);
            if (ext == ".giddey") giddeyMacros.insert(stem);
            if (ext == ".toosii") toosiiMacros.insert(stem);
            if (ext == ".bam")    bamMacros.insert(stem);
            if (ext == ".sexyy")  sexyyMacros.insert(stem);
        } else if (ext == ".gdr" || ext == ".xd" || ext == ".json" || ext == ".brr") {
                        incompatibleMacros.insert(stem);
        }
    }
    std::sort(storedMacros.begin(), storedMacros.end());
}

void GucciEngine::applyIntervalAutosave() {
    if (autosaveIntervalSec < 1.0) autosaveIntervalSec = 1.0;
    if (replay.m_autosaveJobId == 0) {
        replay.m_autosaveJobId = scheduler.schedule(autosaveIntervalSec, [this]{
            if (autosaveAtInterval && isRecording() && !replay.m_actionAtom.empty()) {
                auto path = replay.getCurrentPath();
                if (replayBackupsEnabled) replay.backupExisting(path);
                replay.save(path);
            }
        });
    } else {
        scheduler.reschedule(replay.m_autosaveJobId, autosaveIntervalSec);
    }
}

bool GucciEngine::trimMacro(const std::string& name, int startTick, int endTick, bool rebase) {
    if (endTick <= startTick) return false;
    BRRMacro* m = BRRMacro::loadFromDisk(name);
    if (!m) return false;
    std::vector<BRRInput> kept;
    kept.reserve(m->inputs.size());
    for (auto const& in : m->inputs) {
        if (in.tick < startTick || in.tick > endTick) continue;
        BRRInput c = in;
        if (rebase) c.tick -= startTick;
        kept.push_back(c);
    }
    if (kept.empty()) { delete m; return false; }
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
        delete ma; delete mb; return false;
    }
    int32_t base = ma->inputs.back().tick + (gapTicks > 0 ? gapTicks : 0);
    for (auto in : mb->inputs) { in.tick += base; ma->inputs.push_back(in); }
    ma->anchors.clear();
    ma->checkpoints.clear();
    ma->deathFrames.clear();
    ma->attemptStartTicks.clear();
    ma->name = a + "_merged";
    ma->persistedName.clear();
    ma->persist();
    delete ma; delete mb;
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

struct GdrJsonInput { long long frame = 0; int button = 1; bool player2 = false; bool down = false; };

static bool gdrJsonExtract(const std::string& text, double& framerate, std::vector<GdrJsonInput>& out) {
    {
        auto p = text.find("\"framerate\"");
        if (p != std::string::npos) {
            p = text.find(':', p);
            if (p != std::string::npos) {
                try { double fr = std::stod(text.substr(p + 1, 32)); if (fr > 0) framerate = fr; }
                catch (...) {}
            }
        }
    }
    auto ip = text.find("\"inputs\"");
    if (ip == std::string::npos) return false;
    auto arrStart = text.find('[', ip);
    if (arrStart == std::string::npos) return false;

    size_t i = arrStart + 1;
    int depth = 1;
    while (i < text.size() && depth > 0) {
        char c = text[i];
        if (c == '[') { depth++; i++; continue; }
        if (c == ']') { depth--; i++; continue; }
        if (c == '{') {
            size_t j = i + 1; int od = 1;
            while (j < text.size() && od > 0) {
                if (text[j] == '{') od++;
                else if (text[j] == '}') od--;
                j++;
            }
            std::string obj = text.substr(i, j - i);
            auto num = [&](const char* k, double& val) -> bool {
                auto p = obj.find(std::string("\"") + k + "\"");
                if (p == std::string::npos) return false;
                p = obj.find(':', p);
                if (p == std::string::npos) return false;
                try { val = std::stod(obj.substr(p + 1, 32)); } catch (...) { return false; }
                return true;
            };
            auto boolean = [&](const char* k, bool& val) -> bool {
                auto p = obj.find(std::string("\"") + k + "\"");
                if (p == std::string::npos) return false;
                p = obj.find(':', p);
                if (p == std::string::npos) return false;
                auto rest = obj.substr(p + 1, 8);
                if (rest.find("true")  != std::string::npos) { val = true;  return true; }
                if (rest.find("false") != std::string::npos) { val = false; return true; }
                try { val = std::stod(rest) != 0.0; } catch (...) { return false; }
                return true;
            };
            GdrJsonInput in;
            double v = 0; bool bv = false;
            if (num("frame", v)) in.frame = static_cast<long long>(v);
            if (num("btn", v) || num("button", v)) in.button = static_cast<int>(v);
            if (boolean("2p", bv) || boolean("player2", bv)) in.player2 = bv;
            if (boolean("down", bv) || boolean("hold", bv) || boolean("holding", bv)) in.down = bv;
            out.push_back(in);
            i = j;
            continue;
        }
        i++;
    }
    return !out.empty();
}

}

bool GucciEngine::convertToBRR(const std::string& name) {
    auto dir = getReplayDir();
    auto isNative = [](const std::string& e) {
        return e == ".brrr" || e == ".toosii" || e == ".ja" ||
               e == ".giddey" || e == ".bam" || e == ".sexyy";
    };

    fs::path src;
    std::error_code ec;
    for (auto& it : fs::directory_iterator(dir, ec)) {
        if (!it.is_regular_file()) continue;
        if (it.path().stem().string() != name) continue;
        if (isNative(it.path().extension().string())) continue;
        src = it.path();
        break;
    }
    if (src.empty()) {
        log::warn("[GucciBot] convertToBRR: no legacy file found for '{}'", name);
        return false;
    }

    std::ifstream f(src, std::ios::binary | std::ios::ate);
    if (!f) { log::warn("[GucciBot] convertToBRR: can't open {}", src.string()); return false; }
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> bytes(sz);
    if (sz) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
    f.close();
    if (bytes.empty()) { log::warn("[GucciBot] convertToBRR: {} is empty", src.string()); return false; }

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
            std::sort(out.inputs.begin(), out.inputs.end(),
                      [](const BRRInput& x, const BRRInput& y) { return x.tick < y.tick; });
            out.persist();
            reloadMacroList();
            log::info("[GucciBot] Converted '{}' (GDR JSON, {} inputs) to native format",
                      name, out.inputs.size());
            return true;
        }
    }

    log::warn("[GucciBot] convertToBRR: unsupported format: {} (binary GDR? export it as JSON and retry)",
              src.string());
    return false;
}

std::vector<GucciEngine::DiffEntry> GucciEngine::diffMacros(const std::string& a, const std::string& b) {
    std::vector<DiffEntry> out;
    constexpr size_t kMaxDiffs = 500;

    BRRMacro* ma = BRRMacro::loadFromDisk(a);
    BRRMacro* mb = BRRMacro::loadFromDisk(b);
    if (!ma || !mb) {
        if (!ma) out.push_back({ -1, "Could not load macro A: " + a });
        if (!mb) out.push_back({ -1, "Could not load macro B: " + b });
        delete ma; delete mb;
        return out;
    }

    if (ma->framerate != mb->framerate)
        out.push_back({ -1, fmt::format("Framerate differs: A={} vs B={}", ma->framerate, mb->framerate) });
    if (ma->inputs.size() != mb->inputs.size())
        out.push_back({ -1, fmt::format("Input count differs: A={} vs B={}",
                                        ma->inputs.size(), mb->inputs.size()) });

    auto describe = [](const BRRInput& in) {
        const char* btn = in.actionType == 2 ? "left" : in.actionType == 3 ? "right" : "jump";
        return fmt::format("f{} {} {} {}", in.tick, btn,
                           in.isPressed() ? "press" : "release",
                           in.isPlayer2() ? "P2" : "P1");
    };

    size_t n = std::min(ma->inputs.size(), mb->inputs.size());
    for (size_t i = 0; i < n && out.size() < kMaxDiffs; ++i) {
        const auto& A = ma->inputs[i];
        const auto& B = mb->inputs[i];
        if (A.tick != B.tick || A.actionType != B.actionType ||
            A.isPressed() != B.isPressed() || A.isPlayer2() != B.isPlayer2()) {
            out.push_back({ static_cast<int>(A.tick),
                fmt::format("#{}: A[{}] vs B[{}]", i, describe(A), describe(B)) });
        }
    }

    if (out.size() >= kMaxDiffs) {
        out.push_back({ -1, "... truncated at 500 differences." });
    } else if (ma->inputs.size() != mb->inputs.size()) {
        const auto& longer = ma->inputs.size() > mb->inputs.size() ? *ma : *mb;
        const char* tag    = ma->inputs.size() > mb->inputs.size() ? "A" : "B";
        size_t extra = longer.inputs.size() - n;
        out.push_back({ static_cast<int>(longer.inputs[n].tick),
            fmt::format("{} has {} extra input(s) starting at [{}]",
                        tag, extra, describe(longer.inputs[n])) });
    }

    delete ma; delete mb;
    return out;
}

void GucciEngine::saveBotSettingsPreset(const std::string& name) {
    BotSettingsPreset p;
    p.name = name;
    p.tps = updater.m_tps; p.speedhack = updater.m_speedhack;
    p.lockDelta = updater.m_lockDelta;
    p.lockDeltaMode = (int)updater.m_lockDeltaMode;
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

    auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(),
        [&](auto& x){ return x.name == name; });
    if (it != settingsPresets.end()) *it = p;
    else settingsPresets.push_back(p);

        auto dir = getPresetsDir();
    fs::create_directories(dir);
    std::ofstream f(dir / (name + ".json"));
    f << "{\"name\":\"" << p.name << "\""
      << ",\"tps\":" << p.tps
      << ",\"speedhack\":" << p.speedhack
      << ",\"lockDelta\":" << (p.lockDelta?"true":"false")
      << ",\"lockDeltaMode\":" << p.lockDeltaMode
      << ",\"ssbFix\":" << (p.ssbFix?"true":"false")
      << ",\"preventDeath\":" << (p.preventDeath?"true":"false")
      << ",\"noclip\":" << (p.noclip?"true":"false")
      << "}";
}

bool GucciEngine::loadBotSettingsPreset(const std::string& name) {
    auto it = std::find_if(settingsPresets.begin(), settingsPresets.end(),
        [&](auto& x){ return x.name == name; });
    if (it == settingsPresets.end()) return false;
    auto& p = *it;
    updater.setTps(p.tps); updater.m_speedhack = p.speedhack;
    updater.m_lockDelta = p.lockDelta;
    updater.m_lockDeltaMode = (GucciUpdater::LockDeltaMode)p.lockDeltaMode;
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
    settingsPresets.erase(
        std::remove_if(settingsPresets.begin(), settingsPresets.end(),
            [&](auto& p){ return p.name == name; }),
        settingsPresets.end());
    std::error_code ec;
    fs::remove(getPresetsDir() / (name + ".json"), ec);
}

void GucciEngine::initialize() {
        fs::create_directories(getReplayDir());
    fs::create_directories(getPresetsDir());

        auto* mod = Mod::get();
    updater.m_tps              = mod->getSavedValue<double>("updater_tps", 240.0);
    updater.m_speedhack        = mod->getSavedValue<double>("updater_speedhack", 1.0);
    updater.m_lockDelta        = mod->getSavedValue<bool>("updater_lockDelta", true);
    updater.m_lockDeltaMode    = (GucciUpdater::LockDeltaMode)mod->getSavedValue<int>("updater_lockDeltaMode", 0);
    updater.m_ssbFix           = mod->getSavedValue<bool>("updater_ssbFix", true);
    updater.m_backwardsStepping= mod->getSavedValue<bool>("updater_backwardsStepping", false);
    updater.m_extrapolateFrames= mod->getSavedValue<bool>("updater_extrapolateFrames", false);
    updater.m_preventDeath     = mod->getSavedValue<bool>("updater_preventDeath", false);
    updater.m_autoFlipOnDeath  = mod->getSavedValue<bool>("updater_autoFlipOnDeath", false);
    updater.m_speedhackAudio   = mod->getSavedValue<bool>("updater_speedhackAudio", true);
    noclipEnabled              = mod->getSavedValue<bool>("hack_noclip", false);
    noclipThreshold            = mod->getSavedValue<float>("hack_noclipThreshold", 0.f);
    showHitboxes               = mod->getSavedValue<bool>("hack_hitboxes", false);
    pathPreview                = mod->getSavedValue<bool>("hack_trajectory", false);
    pathLength                 = mod->getSavedValue<int>("hack_trajectory_len", 312);
    layoutMode                 = mod->getSavedValue<bool>("hack_layoutMode", false);
    noMirrorEffect             = mod->getSavedValue<bool>("hack_noMirror", false);
    audioPitchEnabled          = mod->getSavedValue<bool>("hack_audioPitch", false);
    practiceRangeEnabled       = mod->getSavedValue<bool>("practice_range", false);
    autosaveAtLevelEnd         = mod->getSavedValue<bool>("autosave_atLevelEnd", false);
    autosaveAtInterval         = mod->getSavedValue<bool>("autosave_atInterval", false);
    autosaveIntervalSec        = mod->getSavedValue<double>("autosave_interval", 60.0);
    replayBackupsEnabled       = mod->getSavedValue<bool>("replay_backups", true);
    replay.m_mirrorInputs      = mod->getSavedValue<bool>("replay_mirrorInputs", false);
    replay.m_maintainGravity   = mod->getSavedValue<bool>("replay_maintainGravity", false);
    hud.enabled                = mod->getSavedValue<bool>("hud_enabled", false);
    hud.showFrame              = mod->getSavedValue<bool>("hud_showFrame", true);
    hud.showTPS                = mod->getSavedValue<bool>("hud_showTPS", false);

        std::error_code ec;
    for (auto& entry : fs::directory_iterator(getPresetsDir(), ec)) {
        if (entry.path().extension() != ".json") continue;
        BotSettingsPreset p;
        p.name = entry.path().stem().string();
                std::ifstream fin(entry.path());
        std::string json((std::istreambuf_iterator<char>(fin)), {});
        auto ext = [&](const std::string& key) -> std::string {
            auto pos = json.find("\"" + key + "\":");
            if (pos == std::string::npos) return "";
            pos += key.size() + 3;
            auto end = json.find_first_of(",}", pos);
            auto val = json.substr(pos, end - pos);
            if (!val.empty() && val.front() == '"') val = val.substr(1, val.size() - 2);
            return val;
        };
        try {
            p.tps = std::stod(ext("tps"));
            p.lockDelta = ext("lockDelta") == "true";
            p.ssbFix = ext("ssbFix") == "true";
            p.preventDeath = ext("preventDeath") == "true";
            p.noclip = ext("noclip") == "true";
            settingsPresets.push_back(p);
        } catch (...) {}
    }

                applyIntervalAutosave();

    reloadMacroList();
    enabled = true;
            log::info("[GucciBot] ========================================");
    log::info("[GucciBot] BUILD: {} | compiled {} {}", GB_BUILD_LABEL, __DATE__, __TIME__);
    log::info("[GucciBot] ========================================");
    log::info("[GucciBot] " MOD_VERSION " initialized — {} macros", storedMacros.size());

        gbcheck::run(5, 4,
                 GBR6_VERSION, BRR_FORMAT_VERSION, MOD_VERSION);
}

static PauseLayer* findOpenPauseLayerRecursive(CCNode* node) {
    if (!node) return nullptr;
    if (auto* p = typeinfo_cast<PauseLayer*>(node)) return p;
    if (auto* kids = node->getChildren()) {
        for (auto* child : CCArrayExt<CCNode*>(kids)) {
            if (!child) continue;
            if (auto* found = findOpenPauseLayerRecursive(child)) return found;
        }
    }
    return nullptr;
}
static PauseLayer* findOpenPauseLayer() {
    return findOpenPauseLayerRecursive(CCDirector::sharedDirector()->getRunningScene());
}

void GucciEngine::analyzeFrameWindows() {
                fwMarks.clear();
    fwCapStack.clear();
    fwClickSamples.clear();

                    for (auto const& a : replay.m_actionAtom.m_actions) {
        if (a.isInput() && a.m_holding)
            fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, false });
    }

    if (fwClickSamples.empty()) {
        fwHasData = false;
        fwAnalyzeRunning = false;
        fwState = FwState::Idle;
        log::info("[GucciBot] Frame-window: no inputs to analyze");
        return;
    }

    std::sort(fwClickSamples.begin(), fwClickSamples.end(),
        [](auto& a, auto& b){ return a.frame < b.frame; });

    fwAnalyzeTotal     = (int)fwClickSamples.size();
    fwAnalyzeCur       = 0;
    fwAnalyzeProgress  = 0.0f;
    fwAnalyzeRunning   = true;
    fwAnalyzeStage  = "starting";

            auto* pl = PlayLayer::get();
    if (!pl) { fwAnalyzeRunning = false; fwState = FwState::Idle; return; }

    fwProbeHorizon = std::max(8, fwSweepRange + 4);
    fwCapIndex     = 0;
    fwAnalyzing    = true;
                fwSavedAtom = replay.m_actionAtom;

                                                        if (auto* pause = findOpenPauseLayer()) {
        pause->onResume(nullptr);
        log::info("[GucciBot] Frame-window: dismissed open pause menu before analysis");
    }
    pl->m_isPaused = false;
    pl->m_isPracticeMode = false;
    practiceFix.m_loadCheckpoint = false;
    practiceFix.m_isBackstep     = false;
    practiceFix.m_savedCheckpoints.clear();
    updater.m_fullReset = true;
    pl->resetLevel();
                                                                                                                            updater.m_fullReset = false;
    updater.resetFrame();
    replay.m_inputIndex = 0;
    setMode(Mode::Playing);

                            fwClickSamples.clear();
    for (auto const& a : fwSavedAtom.m_actions) {
        if (a.isInput() && a.m_holding)
            fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, false });
    }
    std::sort(fwClickSamples.begin(), fwClickSamples.end(),
        [](auto& a, auto& b){ return a.frame < b.frame; });
    fwSampling = false;

    fwState = FwState::Capturing;
    log::info("[GucciBot] Frame-window: async analysis started — {} inputs, "
              "sweep +/-{}, horizon {} (paused was {})",
              fwClickSamples.size(), fwSweepRange, fwProbeHorizon,
              pl->m_isPaused ? "yes" : "no");
}

void GucciEngine::fwTick() {
    if (!fwAnalyzing) return;
    auto* pl = PlayLayer::get();
    if (!pl) { cancelAnalysis(); return; }

    auto player1 = pl->m_player1;
    uint32_t frame = updater.getFrame();

    switch (fwState) {

        case FwState::Capturing: {
        fwAnalyzeStage = "capturing";
                                        if (player1 && !fwProbeDied) {
            log::info("[CAP-F] f={} steps={} x={:.1f} y={:.1f} yvel={:.1f} ground={} respawn={} tps={:.0f}",
                      frame, updater.estimatedStepCount,
                      player1->m_position.x, player1->m_position.y, player1->m_yVelocity,
                      player1->m_isOnGround ? 1 : 0, updater.m_respawnTimer, updater.m_tps);
        }
                while (fwCapIndex < fwClickSamples.size() &&
               fwClickSamples[fwCapIndex].frame <= frame) {
                        if (auto* sp = fwClickSamples[fwCapIndex].player2 ? pl->m_player2 : pl->m_player1) {
                fwClickSamples[fwCapIndex].x = sp->m_position.x;
                fwClickSamples[fwCapIndex].y = sp->m_position.y;
            }
            CheckpointObject* cp = pl->createCheckpoint();
            fwCkptCreatedThisFrame = true;
            log::info("[CAP] createCheckpoint @ f={} click={}", frame, fwCapIndex);
            StoredFrame sf; sf.frame = fwClickSamples[fwCapIndex].frame;
            if (cp) {
                cp->retain();
                practiceFix.saveState(cp, fwClickSamples[fwCapIndex].frame);
                if (!practiceFix.m_storedFrames.empty()) {
                    sf.state = practiceFix.m_storedFrames.back().state;
                    practiceFix.m_storedFrames.pop_back();
                }
            }
            fwCapStack.push_back(sf);
            fwCapIndex++;
            fwAnalyzeProgress = 0.5f * (float)fwCapIndex / (float)fwClickSamples.size();
        }

                if (fwCapIndex >= fwClickSamples.size()) {
            log::info("[GucciBot] Frame-window: capture done — {} checkpoints",
                      fwCapStack.size());
                                                                                                mode = Mode::Idle;
            fwSampling = false;
                        fwProbeClick = 0; fwProbeShift = -1; fwProbePhase = 0;
            fwProbeLow = 0; fwProbeHigh = 0;
            fwProbeFrame = 0; fwProbeInjected = false;
            fwState = FwState::Probing;
            muteAnalysisMusic();
            computeProbeHorizon();
            beginProbeRun();
        }
        break;
    }

        case FwState::Probing: {
        fwAnalyzeStage = "probing";
                                                fwProbeFrame++;

                if (fwProbeDied || fwProbeFrame >= fwProbeHorizon) {
            bool survived = !fwProbeDied;
            log::info("[GucciBot]   probe click {} shift {:+d}: died={} "
                      "(after {} frames, horizon {})",
                      fwProbeClick, fwProbeShift, fwProbeDied ? "YES" : "no",
                      fwProbeFrame, fwProbeHorizon);

                        if (fwProbePhase == 0) {
                if (survived) { fwProbeLow = fwProbeShift; fwProbeShift--; }
                else          { fwProbePhase = 1; fwProbeShift = 1; }
                if (fwProbeShift < -fwSweepRange) { fwProbePhase = 1; fwProbeShift = 1; }
            } else {
                if (survived) { fwProbeHigh = fwProbeShift; fwProbeShift++; }
                else          { finishProbeClick(); break; }
                if (fwProbeShift > fwSweepRange) { finishProbeClick(); break; }
            }
            beginProbeRun();
        }
        break;
    }

    case FwState::Finishing:
    case FwState::Idle:
    default:
        break;
    }
}

void GucciEngine::computeProbeHorizon() {
    const int kMaxHorizon = std::max(16, fwMaxFramesMeasured);
    const int kMinHorizon = 12;
    const int depth = std::max(1, fwLookaheadDepth);
                    size_t target = fwProbeClick + (size_t)depth;
    if (target < fwClickSamples.size()) {
        long gap = (long)fwClickSamples[target].frame
                 - (long)fwClickSamples[fwProbeClick].frame;
        gap += fwSweepRange + 4;
        fwProbeHorizon = (int)std::clamp(gap, (long)kMinHorizon, (long)kMaxHorizon);
    } else {
                        fwProbeHorizon = kMaxHorizon;
    }
}

void GucciEngine::beginProbeRun() {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    if (fwProbeClick >= fwCapStack.size() ||
        fwCapStack[fwProbeClick].frame != fwClickSamples[fwProbeClick].frame) {
                finishProbeClick();
        return;
    }

                    replay.m_actionAtom = fwSavedAtom;
    auto& acts = replay.m_actionAtom.m_actions;
    acts.erase(std::remove_if(acts.begin(), acts.end(),
                   [](const gb::Action& a){ return !a.isInput(); }),
               acts.end());

                    uint32_t targetFrame = fwClickSamples[fwProbeClick].frame;
    bool     targetP2    = fwClickSamples[fwProbeClick].player2;
    if (fwProbeShift > 0) {
        for (auto& a : acts) {
            if (a.m_frame == targetFrame && a.m_player2 == targetP2 && a.m_holding) {
                a.m_frame = targetFrame + (uint32_t)fwProbeShift;
                break;
            }
        }
                std::stable_sort(acts.begin(), acts.end(),
                         [](const gb::Action& a, const gb::Action& b){
                             return a.m_frame < b.m_frame; });
    }

                                        practiceFix.m_savedCheckpoints.clear();
    practiceFix.m_storedFrames.clear();
    practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);
    practiceFix.m_storedFrames.push_back(fwCapStack[fwProbeClick]);

    fwProbeDied     = false;
    fwProbeFrame    = 0;
    fwProbeInjected = false;

                mode = Mode::Playing;

    if (practiceFix.canRestoreState()) {
        practiceFix.m_loadCheckpoint = true;
        practiceFix.m_isBackstep     = true;
        pl->resetLevel();
        practiceFix.m_loadCheckpoint = false;
        practiceFix.m_isBackstep     = false;
    }
}

void GucciEngine::finishProbeClick() {
    int window = (fwProbeHigh - fwProbeLow) + 1;
    const float levelLen = m_levelLength > 0.f ? m_levelLength : 1.f;

    FrameWindowMark mk;
    mk.x       = fwClickSamples[fwProbeClick].x;
    mk.y       = fwClickSamples[fwProbeClick].y;
    mk.window  = window;
    mk.player2 = fwClickSamples[fwProbeClick].player2;
    mk.frame   = fwClickSamples[fwProbeClick].frame;
    mk.percent = std::clamp(fwClickSamples[fwProbeClick].x / levelLen * 100.f, 0.f, 100.f);
    fwMarks.push_back(mk);

    log::info("[GucciBot] Frame-window: click {} @ frame {} ({:.1f}%) window={} "
              "(shifts {}..{})",
              fwProbeClick, fwClickSamples[fwProbeClick].frame, mk.percent,
              window, fwProbeLow, fwProbeHigh);

    fwProbeClick++;
    fwAnalyzeCur = (int)fwProbeClick;
    fwAnalyzeProgress = 0.5f + 0.5f * (float)fwProbeClick / (float)fwClickSamples.size();

    if (fwProbeClick >= fwClickSamples.size()) {
        fwFinishAnalysis();
        return;
    }
        fwProbeShift = -1; fwProbePhase = 0; fwProbeLow = 0; fwProbeHigh = 0;
    computeProbeHorizon();
    beginProbeRun();
}

void GucciEngine::fwFinishAnalysis() {
    fwState = FwState::Finishing;
    auto* pl = PlayLayer::get();

        practiceFix.m_storedFrames.clear();
    practiceFix.m_loadCheckpoint = false;
    practiceFix.m_isBackstep     = false;
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

    fwAnalyzing      = false;
    fwProbeDied      = false;
    fwAnalyzeRunning = false;
    fwAnalyzeProgress= 1.0f;
    fwAnalyzeStage= "done";
    fwHasData        = !fwMarks.empty();
    saveFwMarks(replay.getCurrentPath());
    fwState          = FwState::Idle;

    log::info("[GucciBot] Frame-window: analysis complete — {} marks, macro restored",
              fwMarks.size());
}

void GucciEngine::muteAnalysisMusic() {
                }
void GucciEngine::unmuteAnalysisMusic() {
    }

void GucciEngine::cancelAnalysis() {
    if (!fwAnalyzing) return;
    fwState = FwState::Finishing;

    practiceFix.m_storedFrames.clear();
    practiceFix.m_loadCheckpoint = false;
    practiceFix.m_isBackstep     = false;
    if (auto* pl = PlayLayer::get()) {
        updater.m_fullReset = true;
        pl->resetLevel();
        updater.m_fullReset = false;
        updater.resetFrame();
    }

        replay.m_actionAtom = fwSavedAtom;
    replay.m_inputIndex = 0;
    unmuteAnalysisMusic();

    fwAnalyzing      = false;
    fwProbeDied      = false;
    fwAnalyzeRunning = false;
    fwAnalyzeStage   = "cancelled";
    fwState          = FwState::Idle;
    mode             = Mode::Idle;
    if (userTpsSaved > 0.0) { updater.setTps(userTpsSaved); userTpsSaved = 0.0; }

    log::info("[GucciBot] Frame-window: analysis cancelled — macro restored");
}
