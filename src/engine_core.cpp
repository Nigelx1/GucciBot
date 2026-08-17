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
#include <cstring>

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
            // m_pathSamples is indexed by frame and only ever grows -- without this,
            // a checkpoint retry leaves stale ground-truth data (from the attempt
            // that just died) sitting at every index past the checkpoint, silently
            // corrupting "Show Macro Path" and Calculate for the new attempt.
            if (m_pathSamples.size() > (size_t)respawnFrame + 1)
                m_pathSamples.resize((size_t)respawnFrame + 1);
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
            m_pathSamples.clear();
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
    // NOT m_replayName -- that field is only ever populated by actually
    // loading an existing GBR6 file (from its own embedded header), so for
    // any macro recorded fresh in this session it silently stays "" for its
    // entire lifetime, making every call site of this function (Calculate's
    // auto-save, the Manual Frame Windows save button, autosave-at-interval,
    // autosave-at-level-end) resolve to the same bare "<dir>/.brrr" for
    // every macro. gb->replayName is the field the UI actually keeps in
    // sync (record start, load, rename, the macro-name text field), so it's
    // the reliable source of "which macro is this really."
    return dir / (gb->replayName + ".brrr");
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
    uint8_t ver = 2; f.write((const char*)&ver, 1);
    uint32_t n = (uint32_t)samples.size(); f.write((const char*)&n, 4);
    for (auto const& s : samples) {
        f.write((const char*)&s.p1x, 4);     f.write((const char*)&s.p1y, 4);
        f.write((const char*)&s.p1XVel, 4);  f.write((const char*)&s.p1YVel, 4);
        f.write((const char*)&s.p1Rot, 4);
        uint8_t p1flags = (s.p1OnGround ? 1 : 0) | (s.p1UpsideDown ? 2 : 0) | (s.p1Dashing ? 4 : 0);
        f.write((const char*)&p1flags, 1);
        f.write(&s.gamemode1, 1);

        f.write((const char*)&s.p2x, 4);     f.write((const char*)&s.p2y, 4);
        f.write((const char*)&s.p2XVel, 4);  f.write((const char*)&s.p2YVel, 4);
        f.write((const char*)&s.p2Rot, 4);
        uint8_t p2flags = (s.p2OnGround ? 1 : 0) | (s.p2UpsideDown ? 2 : 0) | (s.p2Dashing ? 4 : 0)
                        | (s.hasP2 ? 8 : 0);
        f.write((const char*)&p2flags, 1);
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
    if (ver != 2) return; // v1 sidecars (position-only) are silently dropped -- re-record to get force-capture support
    uint32_t n = 0; f.read((char*)&n, 4);
    // n comes straight from an untrusted sidecar file -- reserving it outright
    // lets a corrupted/truncated file (or a garbage value near UINT32_MAX)
    // request a multi-gigabyte allocation, throwing length_error/bad_alloc
    // with nothing upstream to catch it. This function runs from
    // GucciEngine::initialize() at mod startup, so that would crash the whole
    // game. Cap the reserve hint to what the file could actually still hold;
    // the loop below already handles a short/corrupt file via `if (!f) break`.
    {
        constexpr std::streamoff kRecordBytes = 44;
        auto curPos = f.tellg();
        f.seekg(0, std::ios::end);
        auto endPos = f.tellg();
        f.seekg(curPos);
        uint64_t maxRecords = (curPos >= 0 && endPos > curPos)
            ? (uint64_t)(endPos - curPos) / kRecordBytes : 0;
        samples.reserve(std::min<uint64_t>(n, maxRecords));
    }
    for (uint32_t i = 0; i < n; ++i) {
        MacroPathSample s;
        f.read((char*)&s.p1x, 4);    f.read((char*)&s.p1y, 4);
        f.read((char*)&s.p1XVel, 4); f.read((char*)&s.p1YVel, 4);
        f.read((char*)&s.p1Rot, 4);
        uint8_t p1flags = 0; f.read((char*)&p1flags, 1);
        s.p1OnGround = p1flags & 1; s.p1UpsideDown = p1flags & 2; s.p1Dashing = p1flags & 4;
        f.read(&s.gamemode1, 1);

        f.read((char*)&s.p2x, 4);    f.read((char*)&s.p2y, 4);
        f.read((char*)&s.p2XVel, 4); f.read((char*)&s.p2YVel, 4);
        f.read((char*)&s.p2Rot, 4);
        uint8_t p2flags = 0; f.read((char*)&p2flags, 1);
        s.p2OnGround = p2flags & 1; s.p2UpsideDown = p2flags & 2; s.p2Dashing = p2flags & 4;
        s.hasP2 = p2flags & 8;
        f.read(&s.gamemode2, 1);

        if (!f) break;
        samples.push_back(s);
    }
    log::info("[GucciBot] Macro path: loaded {} sample(s) from sidecar", samples.size());
}

// Loads a native BRR macro's click timing + path samples directly into a
// GucciEngine::JupiterMacroData, WITHOUT going through GucciReplaySystem::
// load() -- that method reaches into the global GucciEngine::get() singleton
// unconditionally (sets mode to Playing, sets loadedMacroLevelName, etc.)
// regardless of which GucciReplaySystem instance it's called on, so there's
// no way to use it here without those side effects leaking into the general
// bot-playback state. This duplicates just the parsing logic that's actually
// needed (mirrors GucciReplaySystem::buildClickIntervals and the legacy-BRR
// branch of load()), deliberately kept separate.
static void loadJupiterMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
    out = {};

    // NOT BRRMacro::loadFromDisk(stem) -- that's hardcoded to search
    // getSaveDir()/replays regardless of what path is passed in, which is
    // exactly the general folder this data is deliberately NOT stored in
    // anymore. Read the exact file directly and deserialize it instead,
    // same low-level call convertToBRR itself uses for its BRR-payload branch.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return;
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> bytes(sz);
    if (sz) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
    f.close();
    if (bytes.empty()) return;

    auto* legacy = BRRMacro::deserialize(bytes);
    if (!legacy) return;
    if (legacy->inputs.empty()) { delete legacy; return; }

    double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
    std::unordered_map<int, uint32_t> openPress;
    for (auto& inp : legacy->inputs) {
        int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
        if (inp.isPressed()) {
            openPress[key] = (uint32_t)inp.tick;
        } else {
            auto it = openPress.find(key);
            if (it != openPress.end()) {
                out.clickIntervalsSec.push_back({ it->second / tps, (double)inp.tick / tps });
                openPress.erase(it);
            }
        }
    }
    out.clickBarTps = tps;
    delete legacy;

    loadPathSamples(path, out.pathSamples);
    out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
    log::info("[GucciBot] Jupiter macro data: {} click interval(s), {} path sample(s)",
              out.clickIntervalsSec.size(), out.pathSamples.size());
}

// General-purpose version of loadJupiterMacroData for the Trainer tab's "load
// any macro" picker. Jupiter's bundled macro always ends up in genuine legacy
// BRR format by the time loadJupiterMacroData reads it (it's routed through
// convertToBRR/BRRMacro::persist() first), so that function only ever needs
// the BRRMacro::deserialize() branch below. A macro the user actually
// recorded and saved in-app is GBR6 format instead (GucciReplaySystem::save()
// always writes via GBR6File, regardless of the cosmetic file extension), so
// this version has to sniff the magic and handle both -- same dual-branch
// approach GucciReplaySystem::load() already uses for real playback.
static void loadTrainerMacroData(const fs::path& path, GucciEngine::TrainerMacroData& out) {
    out = {};

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return;
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> bytes(sz);
    if (sz) f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(sz));
    f.close();
    if (bytes.size() < 4) return;

    if (std::memcmp(bytes.data(), "GBR6", 4) == 0) {
        auto result = GBR6File::deserialize(bytes.data(), bytes.size());
        if (!result) return;
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
                        out.clickIntervalsSec.push_back({ it->second / tps, (double)inp.frame / tps });
                        openPress.erase(it);
                    }
                }
            }
        };
        pairUp(result->p1Inputs, false);
        pairUp(result->p2Inputs, true);
        out.clickBarTps = tps;
    } else if (bytes.size() >= 4 && bytes[0]=='B' && bytes[1]=='R' && bytes[2]=='R' && bytes[3]=='\0') {
        auto* legacy = BRRMacro::deserialize(bytes);
        if (!legacy) return;
        if (legacy->inputs.empty()) { delete legacy; return; }

        double tps = legacy->framerate > 0.0 ? legacy->framerate : 240.0;
        out.levelName = legacy->levelName; // usually empty -- see TrainerMacroData's comment
        out.levelId   = legacy->levelId;
        std::unordered_map<int, uint32_t> openPress;
        for (auto& inp : legacy->inputs) {
            int key = (int)inp.actionType * 2 + (inp.isPlayer2() ? 1 : 0);
            if (inp.isPressed()) {
                openPress[key] = (uint32_t)inp.tick;
            } else {
                auto it = openPress.find(key);
                if (it != openPress.end()) {
                    out.clickIntervalsSec.push_back({ it->second / tps, (double)inp.tick / tps });
                    openPress.erase(it);
                }
            }
        }
        out.clickBarTps = tps;
        delete legacy;
    } else {
        return; // unsupported bytes -- the picker should already exclude these via incompatibleMacros
    }

    loadPathSamples(path, out.pathSamples);
    out.loaded = !out.clickIntervalsSec.empty() || !out.pathSamples.empty();
    log::info("[GucciBot] Trainer macro data: {} click interval(s), {} path sample(s), level='{}'",
              out.clickIntervalsSec.size(), out.pathSamples.size(), out.levelName);
}

bool GucciEngine::loadTrainerMacro(const std::string& stem) {
    auto dir = getReplayDir();
    fs::path found;
    for (auto ext : { ".brrr", ".toosii", ".ja", ".giddey", ".bam", ".sexyy" }) {
        std::error_code ec;
        auto candidate = dir / (stem + ext);
        if (fs::exists(candidate, ec)) { found = candidate; break; }
    }
    if (found.empty()) return false;

    loadTrainerMacroData(found, trainerMacro);
    if (!trainerMacro.loaded) return false;
    trainerMacroName = stem;
    Mod::get()->setSavedValue("trainer_macro_name", stem);
    log::info("[GucciBot] Trainer: loaded macro '{}'", stem);
    return true;
}

void GucciReplaySystem::savePathSamplesNow() {
    savePathSamples(getCurrentPath(), m_pathSamples);
    log::info("[GucciBot] Macro path: backfilled {} sample(s) saved for '{}'",
              m_pathSamples.size(), m_replayName);
}

static fs::path trainerSidecarPath(const fs::path& macroPath) {
    return fs::path(macroPath.string() + ".trainer");
}

static void saveTrainerProgress(const fs::path& macroPath, float bestX) {
    std::ofstream f(trainerSidecarPath(macroPath), std::ios::binary);
    if (!f) return;
    f.write("GBTP", 4);
    uint8_t ver = 1; f.write((const char*)&ver, 1);
    f.write((const char*)&bestX, 4);
}

static float loadTrainerProgress(const fs::path& macroPath) {
    auto sc = trainerSidecarPath(macroPath);
    if (!fs::exists(sc)) return 0.f;
    std::ifstream f(sc, std::ios::binary);
    if (!f) return 0.f;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "GBTP", 4) != 0) return 0.f;
    uint8_t ver = 0; f.read((char*)&ver, 1);
    if (ver != 1) return 0.f;
    float bestX = 0.f; f.read((char*)&bestX, 4);
    return f ? bestX : 0.f;
}

void GucciReplaySystem::saveTrainerProgressNow() {
    saveTrainerProgress(getCurrentPath(), m_trainerBestX);
}

static void saveFwMarks(const fs::path& macroPath) {
    auto* gb = GucciEngine::get();
    auto sc  = fwSidecarPath(macroPath);
    std::error_code ec;
    if (gb->fwMarks.empty()) { fs::remove(sc, ec); return; }
    std::ofstream f(sc, std::ios::binary);
    if (!f) return;
    f.write("GBFW", 4);
    uint8_t ver = 3; f.write((const char*)&ver, 1);
    uint32_t n = (uint32_t)gb->fwMarks.size(); f.write((const char*)&n, 4);
    for (auto const& mk : gb->fwMarks) {
        int32_t w  = mk.window;
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
    if (!fs::exists(sc)) return;
    std::ifstream f(sc, std::ios::binary);
    if (!f) return;
    char magic[4] = {};
    f.read(magic, 4);
    if (std::memcmp(magic, "GBFW", 4) != 0) return;
    uint8_t ver = 0; f.read((char*)&ver, 1);
    if (ver < 1 || ver > 3) return;
    uint32_t n = 0; f.read((char*)&n, 4);
    for (uint32_t i = 0; i < n; ++i) {
        float x = 0, y = 0, pct = 0; int32_t w = 0; uint8_t p2 = 0; uint32_t fr = 0; uint8_t man = 0; uint8_t rel = 0;
        f.read((char*)&x, 4); f.read((char*)&y, 4); f.read((char*)&w, 4);
        f.read((char*)&p2, 1); f.read((char*)&fr, 4); f.read((char*)&pct, 4);
        if (ver >= 2) f.read((char*)&man, 1); // v1 sidecars predate manual marks -- default false
        if (ver >= 3) f.read((char*)&rel, 1); // v1/v2 sidecars predate release windows -- default false
        if (!f) break;
        gb->fwMarks.push_back({ x, y, (int)w, p2 != 0, fr, pct, man != 0, rel != 0 });
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
        if (mk.manual && mk.frame == frame && mk.player2 == player2) return true;
    return false;
}

void GucciReplaySystem::save(const fs::path& path, bool noOverwrite) {
    if (noOverwrite && fs::exists(path)) return;

    auto* gb = GucciEngine::get();
    // Keep in sync with the field the UI actually maintains -- see
    // getCurrentPath()'s comment. Without this, a freshly recorded macro's
    // saved file embeds an empty header.name forever, since m_replayName
    // was never set to anything else before this point.
    m_replayName = gb->replayName;

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
    saveTrainerProgress(path, m_trainerBestX);
}

void GucciReplaySystem::buildClickIntervals(double tps) {
    m_clickIntervalsSec.clear();
    m_clickBarTps = tps > 0.0 ? tps : 240.0;
    std::unordered_map<int, uint32_t> openPress; // key = type*2 + player2, value = press frame
    for (auto const& a : m_actionAtom.m_actions) {
        if (!a.isInput()) continue;
        int key = (int)a.m_type * 2 + (a.m_player2 ? 1 : 0);
        if (a.m_holding) {
            openPress[key] = a.m_frame;
        } else {
            auto it = openPress.find(key);
            if (it != openPress.end()) {
                m_clickIntervalsSec.push_back({ it->second / m_clickBarTps, a.m_frame / m_clickBarTps });
                openPress.erase(it);
            }
        }
    }
    log::info("[GucciBot] Click bar: built {} interval(s) at {} tps", m_clickIntervalsSec.size(), m_clickBarTps);
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
        m_trainerBestX = loadTrainerProgress(path);
        buildClickIntervals(gb->updater.m_tps);
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
        // Legacy format has no path-sample/trainer-progress sidecar of its own
        // -- unlike the GBR6 branch above, which always calls loadPathSamples/
        // loadTrainerProgress for the newly loaded file. Without clearing these
        // here, loading a legacy macro right after a GBR6 one leaves the OLD
        // macro's ground-truth path data and trainer best-X attached to this
        // one, so Calculate/"Show Macro Path" force-write the wrong macro's
        // positions and the trainer bar shows the wrong progress marker.
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
    // Same reasoning as GucciReplaySystem::onReset() -- keep path-sample ground
    // truth in sync with the action atom's own clipping so a resumed recording
    // can't leave stale post-resume-point samples lying around.
    if (replay.m_pathSamples.size() > (size_t)now)
        replay.m_pathSamples.resize((size_t)now);

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

// Minimal MessagePack reader -- just enough to walk a binary GDR file's
// structure (nested maps/arrays/strings/ints/bools/floats) and pull out the
// "inputs" array's frame/btn/2p/down fields, mirroring gdrJsonExtract's
// field-matching exactly (confirmed by inspecting a real exported .gdr:
// same key names, just msgpack-encoded instead of JSON-encoded). Not a
// general-purpose msgpack library -- skips anything it doesn't need
// (gameVersion, description, author, bot/level metadata, etc.) generically
// rather than trying to fully decode the file.
namespace gdrmsgpack {

// Depth-limited: skipValue/skipContainer are mutually recursive, and a
// crafted or corrupted .gdr can nest arrays/maps one level per byte (e.g.
// repeated single-element array headers), which without a cap recurses
// deep enough to overflow the stack and crash the whole game -- there's no
// exception to catch here since a stack overflow isn't a C++ exception.
// Legitimate GDR macro data never nests anywhere close to this deep.
static constexpr size_t kMaxDepth = 64;

static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth = 0);

static bool skipN(const std::vector<uint8_t>& b, size_t& i, size_t n) {
    if (i + n > b.size()) return false;
    i += n; return true;
}
static bool skipContainer(const std::vector<uint8_t>& b, size_t& i, size_t count, bool isMap, size_t depth) {
    if (depth > kMaxDepth) return false;
    size_t n = isMap ? count * 2 : count;
    for (size_t k = 0; k < n; k++) if (!skipValue(b, i, depth + 1)) return false;
    return true;
}
static bool skipValue(const std::vector<uint8_t>& b, size_t& i, size_t depth) {
    if (depth > kMaxDepth) return false;
    if (i >= b.size()) return false;
    uint8_t t = b[i++];
    if (t <= 0x7f) return true;
    if (t >= 0xe0) return true;
    if (t >= 0x80 && t <= 0x8f) return skipContainer(b, i, t & 0x0f, true, depth + 1);
    if (t >= 0x90 && t <= 0x9f) return skipContainer(b, i, t & 0x0f, false, depth + 1);
    if (t >= 0xa0 && t <= 0xbf) return skipN(b, i, t & 0x1f);
    switch (t) {
        case 0xc0: case 0xc2: case 0xc3: return true;
        case 0xc4: { if (i>=b.size()) return false; uint8_t n=b[i++]; return skipN(b,i,n); }
        case 0xc5: { if (i+2>b.size()) return false; uint16_t n=(uint16_t)((b[i]<<8)|b[i+1]); i+=2; return skipN(b,i,n); }
        case 0xc6: { if (i+4>b.size()) return false; uint32_t n=((uint32_t)b[i]<<24)|((uint32_t)b[i+1]<<16)|((uint32_t)b[i+2]<<8)|b[i+3]; i+=4; return skipN(b,i,n); }
        case 0xca: return skipN(b, i, 4);
        case 0xcb: return skipN(b, i, 8);
        case 0xcc: return skipN(b, i, 1);
        case 0xcd: return skipN(b, i, 2);
        case 0xce: return skipN(b, i, 4);
        case 0xcf: return skipN(b, i, 8);
        case 0xd0: return skipN(b, i, 1);
        case 0xd1: return skipN(b, i, 2);
        case 0xd2: return skipN(b, i, 4);
        case 0xd3: return skipN(b, i, 8);
        case 0xd9: { if (i>=b.size()) return false; uint8_t n=b[i++]; return skipN(b,i,n); }
        case 0xda: { if (i+2>b.size()) return false; uint16_t n=(uint16_t)((b[i]<<8)|b[i+1]); i+=2; return skipN(b,i,n); }
        case 0xdb: { if (i+4>b.size()) return false; uint32_t n=((uint32_t)b[i]<<24)|((uint32_t)b[i+1]<<16)|((uint32_t)b[i+2]<<8)|b[i+3]; i+=4; return skipN(b,i,n); }
        case 0xdc: { if (i+2>b.size()) return false; uint16_t n=(uint16_t)((b[i]<<8)|b[i+1]); i+=2; return skipContainer(b,i,n,false,depth+1); }
        case 0xdd: { if (i+4>b.size()) return false; uint32_t n=((uint32_t)b[i]<<24)|((uint32_t)b[i+1]<<16)|((uint32_t)b[i+2]<<8)|b[i+3]; i+=4; return skipContainer(b,i,n,false,depth+1); }
        case 0xde: { if (i+2>b.size()) return false; uint16_t n=(uint16_t)((b[i]<<8)|b[i+1]); i+=2; return skipContainer(b,i,n,true,depth+1); }
        case 0xdf: { if (i+4>b.size()) return false; uint32_t n=((uint32_t)b[i]<<24)|((uint32_t)b[i+1]<<16)|((uint32_t)b[i+2]<<8)|b[i+3]; i+=4; return skipContainer(b,i,n,true,depth+1); }
        default: return false;
    }
}

struct Header { size_t count = SIZE_MAX; bool isMap = false; };
static Header readContainerHeader(const std::vector<uint8_t>& b, size_t& i) {
    Header h;
    if (i >= b.size()) return h;
    uint8_t t = b[i];
    if (t >= 0x80 && t <= 0x8f) { h.count = t & 0x0f; h.isMap = true; i++; return h; }
    if (t >= 0x90 && t <= 0x9f) { h.count = t & 0x0f; h.isMap = false; i++; return h; }
    if (t == 0xdc) { if (i+3>b.size()) return h; h.count=(size_t)((b[i+1]<<8)|b[i+2]); h.isMap=false; i+=3; return h; }
    if (t == 0xdd) { if (i+5>b.size()) return h; h.count=(size_t)(((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]); h.isMap=false; i+=5; return h; }
    if (t == 0xde) { if (i+3>b.size()) return h; h.count=(size_t)((b[i+1]<<8)|b[i+2]); h.isMap=true; i+=3; return h; }
    if (t == 0xdf) { if (i+5>b.size()) return h; h.count=(size_t)(((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]); h.isMap=true; i+=5; return h; }
    return h;
}

static bool readString(const std::vector<uint8_t>& b, size_t& i, std::string& out) {
    if (i >= b.size()) return false;
    uint8_t t = b[i];
    size_t len;
    if (t >= 0xa0 && t <= 0xbf) { len = t & 0x1f; i++; }
    else if (t == 0xd9) { if (i+2>b.size()) return false; len=b[i+1]; i+=2; }
    else if (t == 0xda) { if (i+3>b.size()) return false; len=(size_t)((b[i+1]<<8)|b[i+2]); i+=3; }
    else if (t == 0xdb) { if (i+5>b.size()) return false; len=(size_t)(((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]); i+=5; }
    else return false;
    if (i + len > b.size()) return false;
    out.assign((const char*)&b[i], len);
    i += len;
    return true;
}

static bool readNumber(const std::vector<uint8_t>& b, size_t& i, double& out) {
    if (i >= b.size()) return false;
    uint8_t t = b[i];
    if (t <= 0x7f) { out = t; i++; return true; }
    if (t >= 0xe0) { out = (double)(int8_t)t; i++; return true; }
    switch (t) {
        case 0xcc: if(i+2>b.size())return false; out=b[i+1]; i+=2; return true;
        case 0xcd: if(i+3>b.size())return false; out=(double)((b[i+1]<<8)|b[i+2]); i+=3; return true;
        case 0xce: if(i+5>b.size())return false; out=(double)(((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]); i+=5; return true;
        case 0xcf: { if(i+9>b.size())return false; uint64_t v=0; for(int k=0;k<8;k++)v=(v<<8)|b[i+1+k]; out=(double)v; i+=9; return true; }
        case 0xd0: if(i+2>b.size())return false; out=(double)(int8_t)b[i+1]; i+=2; return true;
        case 0xd1: if(i+3>b.size())return false; out=(double)(int16_t)((b[i+1]<<8)|b[i+2]); i+=3; return true;
        case 0xd2: if(i+5>b.size())return false; out=(double)(int32_t)(((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]); i+=5; return true;
        case 0xd3: { if(i+9>b.size())return false; uint64_t v=0; for(int k=0;k<8;k++)v=(v<<8)|b[i+1+k]; out=(double)(int64_t)v; i+=9; return true; }
        case 0xca: { if(i+5>b.size())return false; uint32_t u=((uint32_t)b[i+1]<<24)|((uint32_t)b[i+2]<<16)|((uint32_t)b[i+3]<<8)|b[i+4]; float f; std::memcpy(&f,&u,4); out=f; i+=5; return true; }
        case 0xcb: { if(i+9>b.size())return false; uint64_t u=0; for(int k=0;k<8;k++)u=(u<<8)|b[i+1+k]; double d; std::memcpy(&d,&u,8); out=d; i+=9; return true; }
        default: return false;
    }
}

static bool readBool(const std::vector<uint8_t>& b, size_t& i, bool& out) {
    if (i >= b.size()) return false;
    if (b[i] == 0xc2) { out = false; i++; return true; }
    if (b[i] == 0xc3) { out = true; i++; return true; }
    double n; size_t save = i;
    if (readNumber(b, i, n)) { out = n != 0.0; return true; }
    i = save;
    return false;
}

} // namespace gdrmsgpack

static bool gdrBinaryExtract(const std::vector<uint8_t>& bytes, double& framerate, std::vector<GdrJsonInput>& out) {
    using namespace gdrmsgpack;
    size_t i = 0;
    auto top = readContainerHeader(bytes, i);
    if (top.count == SIZE_MAX || !top.isMap) return false;

    for (size_t k = 0; k < top.count; k++) {
        std::string key;
        if (!readString(bytes, i, key)) return false;

        if (key == "inputs") {
            auto arr = readContainerHeader(bytes, i);
            if (arr.count == SIZE_MAX || arr.isMap) return false;
            for (size_t e = 0; e < arr.count; e++) {
                auto obj = readContainerHeader(bytes, i);
                if (obj.count == SIZE_MAX || !obj.isMap) return false;
                GdrJsonInput in;
                for (size_t f = 0; f < obj.count; f++) {
                    std::string fk;
                    if (!readString(bytes, i, fk)) return false;
                    if (fk == "frame") { double v; if (readNumber(bytes,i,v)) in.frame=(long long)v; else return false; }
                    else if (fk == "btn" || fk == "button") { double v; if (readNumber(bytes,i,v)) in.button=(int)v; else return false; }
                    else if (fk == "2p" || fk == "player2") { bool v; if (readBool(bytes,i,v)) in.player2=v; else return false; }
                    else if (fk == "down" || fk == "hold" || fk == "holding") { bool v; if (readBool(bytes,i,v)) in.down=v; else return false; }
                    else { if (!skipValue(bytes, i)) return false; }
                }
                out.push_back(in);
            }
        } else if (key == "framerate" || key == "fps") {
            double v; size_t save = i;
            if (readNumber(bytes, i, v)) {
                if (v > 0) framerate = v;
            } else {
                i = save;
                if (!skipValue(bytes, i)) return false;
            }
        } else {
            if (!skipValue(bytes, i)) return false;
        }
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
            std::sort(out.inputs.begin(), out.inputs.end(),
                      [](const BRRInput& x, const BRRInput& y) { return x.tick < y.tick; });
            out.persist();
            reloadMacroList();
            log::info("[GucciBot] Converted '{}' (GDR binary/msgpack, {} inputs) to native format",
                      name, out.inputs.size());
            return true;
        }
    }

    log::warn("[GucciBot] convertToBRR: unsupported format: {}", src.string());
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

        // Auto-convert + load the bundled Jupiter My Favourite GDR macro's
    // click/path data, once, into jupiterMacro -- its OWN dedicated folder
    // AND its own dedicated data holder, never touching replay/mode/
    // loadedMacroLevelName or the general replays folder. It used to load
    // via replay.load(), which reaches into all of that -- that's exactly
    // why it was showing up in the general Saved Replays list AND why
    // actually loading/playing a real macro afterward stopped working
    // (mode was already force-set to Playing at startup, replayName was
    // never set to match, etc). Still uses convertToBRR normally (a
    // general-purpose tool that reads/writes the replays folder) by seeding
    // the raw .gdr there just long enough to run, then moves the result
    // into save/jupiter/ and deletes the seed so nothing lingers where the
    // general macro browser can see it.
    {
        auto jupDir = Mod::get()->getSaveDir() / "jupiter";
        fs::create_directories(jupDir);

        auto findIn = [](fs::path const& dir, std::string const& stem) -> fs::path {
            for (auto ext : { ".brrr", ".toosii", ".ja", ".giddey", ".bam", ".sexyy" }) {
                std::error_code ec;
                auto candidate = dir / (stem + ext);
                if (fs::exists(candidate, ec)) return candidate;
            }
            return {};
        };

        auto hidden = findIn(jupDir, "jupiter_my_favourite");
        if (!hidden.empty()) loadJupiterMacroData(hidden, jupiterMacro);

        // Self-healing: if nothing was found, OR what was found didn't
        // actually parse into anything (e.g. a broken/empty file left behind
        // by an earlier build's version of this logic), (re)do the seed +
        // convert + move from scratch rather than trusting a stale file's
        // mere existence.
        if (!jupiterMacro.loaded) {
            std::error_code rmEc;
            if (!hidden.empty()) fs::remove(hidden, rmEc);

            auto bundled = Mod::get()->getResourcesDir() / "jupiter_my_favourite.gdr";
            std::error_code ec;
            if (fs::exists(bundled, ec)) {
                // Seeded under a name no real user macro would ever have,
                // NOT "jupiter_my_favourite" -- that used to unconditionally
                // overwrite (copy_options::overwrite_existing) whatever the
                // user already had at that exact path in the shared replays
                // folder, and convertToBRR/persist() match purely by stem, so
                // a same-named user macro could get its OWN file silently
                // renamed aside (_2) and then this seed's converted copy
                // mistaken for it and relocated/deleted in its place. A
                // private stem makes both collisions structurally impossible
                // instead of just unlikely. The dedicated jupDir copy is
                // still named jupiter_my_favourite.<ext> below, so this is
                // invisible to everything downstream (self-heal lookup,
                // loadJupiterMacroData).
                const std::string seedStem = "__guccibot_jupiter_seed";
                auto seedDest = getReplayDir() / (seedStem + ".gdr");
                fs::copy_file(bundled, seedDest, fs::copy_options::overwrite_existing, ec);
                convertToBRR(seedStem);
                auto converted = findIn(getReplayDir(), seedStem);
                if (!converted.empty()) {
                    auto dest = jupDir / ("jupiter_my_favourite" + converted.extension().string());
                    fs::rename(converted, dest, ec);
                    if (!ec) hidden = dest;
                }
                fs::remove(seedDest, ec);
                reloadMacroList();
                if (!hidden.empty()) loadJupiterMacroData(hidden, jupiterMacro);
            }
        }
    }

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
    // Keep manual marks across a fresh run -- only Calculate-computed entries
    // get wiped and recomputed. The probing loop below (see
    // beginOrSkipProbeClick) skips re-measuring any click a manual mark
    // already covers, so this run won't just immediately overwrite them again.
    fwMarks.erase(std::remove_if(fwMarks.begin(), fwMarks.end(),
        [](const FrameWindowMark& mk){ return !mk.manual; }), fwMarks.end());
    fwCapStack.clear();
    fwClickSamples.clear();

    // A release's timing only matters for Wave/Ship/Robot -- everywhere else
    // (Cube/UFO/Ball/Spider/Swing) releasing early or late doesn't change
    // anything, so testing it there was just noise. Gamemode is read from
    // ground truth captured during the original recording (m_pathSamples);
    // if that's not available for this frame (e.g. an older macro with no
    // path-sample data), the release is excluded rather than guessed.
    auto shouldTestRelease = [&](uint32_t frame, bool player2) -> bool {
        if (frame >= replay.m_pathSamples.size()) return false;
        char gm = player2 ? replay.m_pathSamples[frame].gamemode2 : replay.m_pathSamples[frame].gamemode1;
        if (gm == 'H' && !fwTestShipReleases) return false; // Ship, user disabled
        return gm == 'V' || gm == 'H' || gm == 'R'; // Wave, Ship, Robot
    };

                    for (auto const& a : replay.m_actionAtom.m_actions) {
        if (!a.isInput()) continue;
        if (a.m_holding) fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, false });
        else if (shouldTestRelease(a.m_frame, a.m_player2))
            fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, true });
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
        if (!a.isInput()) continue;
        if (a.m_holding) { fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, false }); continue; }
        if (shouldTestRelease(a.m_frame, a.m_player2))
            fwClickSamples.push_back({ a.m_frame, 0.f, 0.f, a.m_player2, true });
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
            beginOrSkipProbeClick();
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
    // fwClickSamples can now hold releases too (see analyzeFrameWindows), so
    // this has to match on holding-state as well -- previously hardcoded to
    // a.m_holding (press-only), which meant probing a release sample found
    // nothing to shift and silently tested the unshifted timing every time.
    bool     targetHolding = !fwClickSamples[fwProbeClick].release;
    if (fwProbeShift > 0) {
        for (auto& a : acts) {
            if (a.m_frame == targetFrame && a.m_player2 == targetP2 && a.m_holding == targetHolding) {
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
    mk.isRelease = fwClickSamples[fwProbeClick].release;
    fwMarks.push_back(mk);

    log::info("[GucciBot] Frame-window: click {} @ frame {} ({:.1f}%) window={} "
              "(shifts {}..{})",
              fwProbeClick, fwClickSamples[fwProbeClick].frame, mk.percent,
              window, fwProbeLow, fwProbeHigh);

    fwProbeClick++;
    beginOrSkipProbeClick();
}

void GucciEngine::beginOrSkipProbeClick() {
    // A manual mark at a click means the user has already decided its window
    // by hand -- don't spend a probe run re-measuring (and potentially
    // overwriting) it. Keep advancing past every manually-covered click
    // before actually starting the next probe.
    while (fwProbeClick < fwClickSamples.size() &&
           fwHasManualMarkAt(fwClickSamples[fwProbeClick].frame, fwClickSamples[fwProbeClick].player2)) {
        log::info("[GucciBot] Frame-window: click {} @ frame {} has a manual mark, skipping probe",
                  fwProbeClick, fwClickSamples[fwProbeClick].frame);
        fwProbeClick++;
    }

    fwAnalyzeCur = (int)fwProbeClick;
    fwAnalyzeProgress = fwClickSamples.empty() ? 1.0f
        : 0.5f + 0.5f * (float)fwProbeClick / (float)fwClickSamples.size();

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

    // Clicks already finished before the cancel (finishProbeClick pushes
    // into fwMarks progressively as each one completes) used to just get
    // thrown away here -- any interruption mid-run (Stop, a death flipping
    // mode, leaving the level) silently lost all completed work. Persist
    // whatever's there instead.
    fwHasData = !fwMarks.empty();
    if (!fwMarks.empty()) saveFwMarksNow();

    log::info("[GucciBot] Frame-window: analysis cancelled — {} result(s) kept",
              fwMarks.size());
}
