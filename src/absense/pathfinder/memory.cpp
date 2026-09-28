#include "memory.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "absense/compat/devlog.hpp"

using namespace geode::prelude;

namespace absense::memory {

namespace {

// --- JSON (GucciBot: Geode's matjson in place of Absense's glaze) -----------
// Same key names as Absense's glz::meta, so a memory folder written by either
// reads in the other. The level key is a 64-bit hash, so it goes as a string:
// a JSON number cannot hold every value of one.

matjson::Value toJson(Solution const& s) {
    std::vector<matjson::Value> events;
    for (uint32_t e : s.events) events.push_back((int64_t)e);
    return matjson::makeObject({
        {"x", (double)s.x},
        {"y", (double)s.y},
        {"mode", (int64_t)s.mode},
        {"upside_down", s.upsideDown},
        {"mini", s.mini},
        {"speed", (double)s.speed},
        {"held", s.held},
        {"events", matjson::Value(events)},
        {"lasted", (int64_t)s.lasted},
        {"uses", (int64_t)s.uses},
        {"wins", (int64_t)s.wins},
        {"hard", s.hard},
    });
}

matjson::Value toJson(Level const& l) {
    std::vector<matjson::Value> solutions, spots;
    for (auto const& s : l.solutions) solutions.push_back(toJson(s));
    for (auto const& s : l.spots)
        spots.push_back(matjson::makeObject({{"x", (double)s.x}, {"fails", (int64_t)s.fails}}));
    return matjson::makeObject({
        {"name", l.name},
        {"key", std::to_string(l.key)},
        {"runs", (int64_t)l.runs},
        {"best_progress", l.bestProgress},
        {"solutions", matjson::Value(solutions)},
        {"spots", matjson::Value(spots)},
    });
}

matjson::Value toJson(Priors const& p) {
    auto wins = matjson::Value::object();
    for (auto const& [situation, kinds] : p.wins) {
        auto inner = matjson::Value::object();
        for (auto const& [kind, n] : kinds) inner[kind] = (int64_t)n;
        wins[situation] = inner;
    }
    std::vector<matjson::Value> levels;
    for (uint64_t k : p.levels) levels.push_back(std::to_string(k));
    return matjson::makeObject({{"wins", wins}, {"levels", matjson::Value(levels)}});
}

template <typename T>
T get(matjson::Value const& v, char const* key, T def) {
    auto r = v[key].as<T>();
    return r.isOk() ? r.unwrap() : def;
}

uint64_t getKey(matjson::Value const& v) {
    if (auto s = v.as<std::string>(); s.isOk()) return std::strtoull(s.unwrap().c_str(), nullptr, 10);
    if (auto n = v.as<int64_t>(); n.isOk()) return (uint64_t)n.unwrap();
    return 0;
}

bool fromJson(matjson::Value const& v, Level& out) {
    if (!v.isObject()) return false;
    Level l;
    l.name = get<std::string>(v, "name", "");
    l.key = getKey(v["key"]);
    l.runs = (uint64_t)get<int64_t>(v, "runs", 0);
    l.bestProgress = get<double>(v, "best_progress", 0.0);
    auto const& sols = v["solutions"];
    for (size_t i = 0; sols.isArray() && i < sols.size(); i++) {
        auto const& j = sols[i];
        Solution s;
        s.x = (float)get<double>(j, "x", 0.0);
        s.y = (float)get<double>(j, "y", 0.0);
        s.mode = (int)get<int64_t>(j, "mode", 0);
        s.upsideDown = get<bool>(j, "upside_down", false);
        s.mini = get<bool>(j, "mini", false);
        s.speed = (float)get<double>(j, "speed", 1.0);
        s.held = get<bool>(j, "held", false);
        auto const& ev = j["events"];
        for (size_t k = 0; ev.isArray() && k < ev.size(); k++)
            if (auto e = ev[k].as<int64_t>(); e.isOk()) s.events.push_back((uint32_t)e.unwrap());
        s.lasted = (int)get<int64_t>(j, "lasted", 0);
        s.uses = (int)get<int64_t>(j, "uses", 0);
        s.wins = (int)get<int64_t>(j, "wins", 0);
        s.hard = get<bool>(j, "hard", false);
        l.solutions.push_back(std::move(s));
    }
    auto const& spots = v["spots"];
    for (size_t i = 0; spots.isArray() && i < spots.size(); i++)
        l.spots.push_back(Spot{(float)get<double>(spots[i], "x", 0.0), (int)get<int64_t>(spots[i], "fails", 0)});
    out = std::move(l);
    return true;
}

bool fromJson(matjson::Value const& v, Priors& out) {
    if (!v.isObject()) return false;
    Priors p;
    auto const& wins = v["wins"];
    if (wins.isObject()) {
        for (auto const& [situation, kinds] : wins) {
            if (!kinds.isObject()) continue;
            for (auto const& [kind, n] : kinds)
                if (auto c = n.as<int64_t>(); c.isOk()) p.wins[situation][kind] = (uint32_t)c.unwrap();
        }
    }
    auto const& levels = v["levels"];
    for (size_t i = 0; levels.isArray() && i < levels.size(); i++) p.levels.push_back(getKey(levels[i]));
    out = std::move(p);
    return true;
}

template <typename T>
bool readJsonFile(std::filesystem::path const& path, T& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto parsed = matjson::parse(text);
    return parsed.isOk() && fromJson(parsed.unwrap(), out);
}

bool writeTextFile(std::filesystem::path const& path, std::string const& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    return (bool)out;
}

constexpr float kNearX = 24.0f;      // a solution counts for a decision this close
constexpr float kSpotSlack = 100.0f;
constexpr size_t kMaxSolutions = 5000;  // the newest; the oldest go first
constexpr size_t kMaxSpots = 2000;
constexpr size_t kMaxEvents = 200;   // longer scripts (pure spam) are not worth keeping
constexpr size_t kMaxNear = 6;
constexpr double kSaveEvery = 30.0;  // seconds, while remembering

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t levelKey(PlayLayer* pl) {
    if (!pl || !pl->m_level) return 0;
    const int id = pl->m_level->m_levelID.value();
    if (id > 0) return (uint64_t)id;
    // A local level: its name and size.
    uint64_t h = 1469598103934665603ull;
    const std::string name = pl->m_level->m_levelName;
    for (const char c : name) h = (h ^ (uint8_t)c) * 1099511628211ull;
    const uint32_t objects = pl->m_objects ? pl->m_objects->count() : 0;
    for (int i = 0; i < 4; i++) h = (h ^ ((objects >> (i * 8)) & 0xff)) * 1099511628211ull;
    return h | (1ull << 63);
}

const char* modeName(int mode) {
    switch (mode) {
        case Trajectory::kModeShip: return "ship";
        case Trajectory::kModeBall: return "ball";
        case Trajectory::kModeBird: return "ufo";
        case Trajectory::kModeDart: return "wave";
        case Trajectory::kModeRobot: return "robot";
        case Trajectory::kModeSpider: return "spider";
        case Trajectory::kModeSwing: return "swing";
        default: return "cube";
    }
}

}  // namespace

Store& Store::get() {
    static Store instance;
    return instance;
}

std::filesystem::path Store::folder() const { return Mod::get()->getSaveDir() / "learned"; }
std::filesystem::path Store::levelPath() const { return folder() / (std::to_string(m_level.key) + ".json"); }

void Store::loadPriors() {
    if (m_priorsLoaded) return;
    m_priorsLoaded = true;
    const auto path = folder() / "priors.json";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    Priors loaded;
    if (readJsonFile(path, loaded)) m_priors = std::move(loaded);
}

void Store::open(PlayLayer* pl) {
    loadPriors();
    const uint64_t key = levelKey(pl);
    if (m_open && m_level.key == key) return;
    if (m_open) save();
    m_level = Level{};
    m_level.key = key;
    m_level.name = pl && pl->m_level ? std::string(pl->m_level->m_levelName) : "";
    std::error_code ec;
    const auto path = levelPath();
    if (std::filesystem::exists(path, ec)) {
        Level loaded;
        if (readJsonFile(path, loaded)) {
            m_level = std::move(loaded);
            m_level.key = key;
        }
    }
    m_level.runs++;
    m_dirty = true;
    m_open = true;
    m_lastSave = now();  // the periodic write is due 30 s into the run, not on its first lesson
    if (std::find(m_priors.levels.begin(), m_priors.levels.end(), key) == m_priors.levels.end()) {
        m_priors.levels.push_back(key);
        m_priorsDirty = true;
    }
    devlog::logf(devlog::Cat::AbsensePathfinder, "memory: %s - run %llu, %zu spots known, %zu that bit before",
                 m_level.name.c_str(), (unsigned long long)m_level.runs, m_level.solutions.size(), m_level.spots.size());
}

void Store::close(double progress) {
    if (!m_open) return;
    if (progress > m_level.bestProgress) {
        m_level.bestProgress = progress;
        m_dirty = true;
    }
    // The lists are in the order they were learned: past the cap the oldest
    // go, so what it knows is always the most recent five thousand. Only
    // here, between runs: while a search runs, its candidates and lessons
    // hold indices into the solutions.
    if (m_level.solutions.size() > kMaxSolutions) {
        m_level.solutions.erase(m_level.solutions.begin(), m_level.solutions.end() - (std::ptrdiff_t)kMaxSolutions);
        m_dirty = true;
    }
    if (m_level.spots.size() > kMaxSpots) {
        m_level.spots.erase(m_level.spots.begin(), m_level.spots.end() - (std::ptrdiff_t)kMaxSpots);
        m_dirty = true;
    }
    save();
    m_open = false;
}

void Store::save() {
    std::error_code ec;
    std::filesystem::create_directories(folder(), ec);
    if (m_dirty && m_level.key != 0) {
        if (!writeTextFile(levelPath(), toJson(m_level).dump(matjson::NO_INDENTATION))) {
            log::error("could not save the level's memory: {}", levelPath().string());
        } else {
            m_dirty = false;
        }
    }
    if (m_priorsDirty) {
        if (writeTextFile(folder() / "priors.json", toJson(m_priors).dump(matjson::NO_INDENTATION)))
            m_priorsDirty = false;
    }
    m_lastSave = now();
}

void Store::forgetLevel() {
    m_level.solutions.clear();
    m_level.spots.clear();
    m_level.runs = 0;
    m_level.bestProgress = 0.0;
    m_dirty = true;
    save();
}

void Store::nearby(const Trajectory::StartState& st, std::vector<size_t>& out) const {
    out.clear();
    const bool mini = st.vehicleSize < 0.9f;
    for (size_t i = 0; i < m_level.solutions.size(); i++) {
        const Solution& s = m_level.solutions[i];
        if (std::abs(s.x - st.x) > kNearX) continue;
        if (s.mode != st.mode || s.upsideDown != st.upsideDown || s.mini != mini || s.held != st.held) continue;
        if (std::abs(s.speed - st.playerSpeed) > 0.05f) continue;
        out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [&](size_t a, size_t b) {
        const Solution& A = m_level.solutions[a];
        const Solution& B = m_level.solutions[b];
        if (A.wins != B.wins) return A.wins > B.wins;
        if (A.hard != B.hard) return A.hard;
        return std::abs(A.x - st.x) < std::abs(B.x - st.x);
    });
    if (out.size() > kMaxNear) out.resize(kMaxNear);
}

void Store::remember(const Trajectory::StartState& st, const std::vector<TickInput>& inputs, int lasted, bool hard) {
    if (!m_open) return;
    std::vector<uint32_t> events = compress(inputs, st.held);
    if (events.empty() || events.size() > kMaxEvents) return;
    const bool mini = st.vehicleSize < 0.9f;
    for (Solution& s : m_level.solutions) {
        // A win only counts for an old solution nearby() would offer from this state, so a route past a speed,
        // gravity or size portal keeps a solution of its own instead of crediting one it can never be offered.
        if (std::abs(s.x - st.x) <= 4.0f && s.mode == st.mode && s.held == st.held && s.upsideDown == st.upsideDown &&
            s.mini == mini && std::abs(s.speed - st.playerSpeed) <= 0.05f && s.events == events) {
            s.wins++;
            s.lasted = std::max(s.lasted, lasted);
            m_dirty = true;
            return;
        }
    }
    Solution s;
    s.x = st.x;
    s.y = st.y;
    s.mode = st.mode;
    s.upsideDown = st.upsideDown;
    s.mini = mini;
    s.speed = st.playerSpeed;
    s.held = st.held;
    s.events = std::move(events);
    s.lasted = lasted;
    s.hard = hard;
    m_level.solutions.push_back(std::move(s));
    m_dirty = true;
}

// The periodic write must not land inside a physics tick: remember() is
// reached from confirmLessons, which in realtime runs from the game's own
// input hook. The pathfinder calls this from a slice instead.
void Store::saveIfDue() {
    if (!m_open) return;
    if (!m_dirty && !m_priorsDirty) return;
    if (now() - m_lastSave <= kSaveEvery) return;
    // This was a synchronous write of the whole level's memory - megabytes of
    // it, once it has learned a lot - on the drawing thread every thirty
    // seconds. Nothing reads these files during a run (open() is the only
    // reader and it runs at the start), so the bytes are laid out here, where
    // the data belongs to this thread, and a writer that owns the file puts
    // them on disk. close() and forgetLevel() still write the blocking way:
    // they happen once, outside the search.
    std::error_code ec;
    std::filesystem::create_directories(folder(), ec);
    std::string levelText, priorsText, levelFile, priorsFile;
    if (m_dirty && m_level.key != 0) {
        levelText = toJson(m_level).dump(matjson::NO_INDENTATION);
        {
            levelFile = levelPath().string();
            m_dirty = false;
        }
    }
    if (m_priorsDirty) {
        priorsText = toJson(m_priors).dump(matjson::NO_INDENTATION);
        {
            priorsFile = (folder() / "priors.json").string();
            m_priorsDirty = false;
        }
    }
    m_lastSave = now();
    if (levelFile.empty() && priorsFile.empty()) return;
    std::thread([levelFile, levelText, priorsFile, priorsText] {
        // One writer at a time, so two of these can never interleave in the
        // same file (and close()'s blocking save is ordered after them).
        static std::mutex files;
        std::lock_guard<std::mutex> lock(files);
        if (!levelFile.empty()) {
            std::ofstream out(levelFile, std::ios::binary | std::ios::trunc);
            out.write(levelText.data(), (std::streamsize)levelText.size());
        }
        if (!priorsFile.empty()) {
            std::ofstream out(priorsFile, std::ios::binary | std::ios::trunc);
            out.write(priorsText.data(), (std::streamsize)priorsText.size());
        }
    }).detach();
}

void Store::used(size_t index) {
    if (index < m_level.solutions.size()) {
        m_level.solutions[index].uses++;
        m_dirty = true;
    }
}

void Store::won(size_t index) {
    if (index < m_level.solutions.size()) {
        m_level.solutions[index].wins++;
        m_dirty = true;
    }
}

int Store::spotFails(float x) const {
    int most = 0;
    for (const Spot& s : m_level.spots) {
        if (std::abs(s.x - x) <= kSpotSlack) most = std::max(most, s.fails);
    }
    return most;
}

void Store::spotFailed(float x, int fails) {
    if (!m_open) return;
    for (Spot& s : m_level.spots) {
        if (std::abs(s.x - x) <= kSpotSlack) {
            s.fails = std::max(s.fails, fails);
            m_dirty = true;
            return;
        }
    }
    m_level.spots.push_back(Spot{x, fails});
    m_dirty = true;
}

std::string Store::situation(const Trajectory::StartState& st, bool repairing) const {
    std::string key = modeName(st.mode);
    if (st.vehicleSize < 0.9f) key += "-mini";
    if (repairing) key += "-repair";
    return key;
}

std::string Store::kindOf(const std::string& ideaName) {
    std::string kind;
    for (const char c : ideaName) {
        if (c == ' ' || c == '#' || c == '(') break;
        kind += (char)std::tolower((unsigned char)c);
    }
    return kind;
}

void Store::win(const std::string& situation, const std::string& ideaName) {
    if (situation.empty()) return;
    m_priors.wins[situation][kindOf(ideaName)]++;
    m_priorsDirty = true;
}

uint32_t Store::score(const std::string& situation, const std::string& ideaName) const {
    const auto it = m_priors.wins.find(situation);
    if (it == m_priors.wins.end()) return 0;
    const auto kt = it->second.find(kindOf(ideaName));
    return kt == it->second.end() ? 0 : kt->second;
}

// Every tick with a press, or where the button changes, is one event.
std::vector<uint32_t> Store::compress(const std::vector<TickInput>& inputs, bool startHeld) {
    std::vector<uint32_t> out;
    bool held = startHeld;
    for (size_t t = 0; t < inputs.size(); t++) {
        const TickInput& in = inputs[t];
        if (in.presses == 0 && in.held == held) continue;
        out.push_back(((uint32_t)t << 3) | (uint32_t)std::min<int>(in.presses, 3) | (in.held ? 4u : 0u));
        held = in.held;
        if (out.size() > kMaxEvents) break;
    }
    return out;
}

std::vector<TickInput> Store::expand(const std::vector<uint32_t>& events, int ticks, bool startHeld) {
    std::vector<TickInput> out((size_t)std::max(0, ticks), TickInput{0, startHeld});
    bool held = startHeld;
    size_t e = 0;
    for (int t = 0; t < ticks; t++) {
        uint8_t presses = 0;
        while (e < events.size() && (int)(events[e] >> 3) == t) {
            presses = (uint8_t)std::min<uint32_t>(events[e] & 3u, 2u);
            held = (events[e] & 4u) != 0;
            e++;
        }
        out[(size_t)t] = TickInput{presses, held};
    }
    return out;
}

}  // namespace absense::memory
