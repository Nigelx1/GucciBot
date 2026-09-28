#ifndef PATHFINDER_MEMORY_HPP
#define PATHFINDER_MEMORY_HPP

// Absense - what the pathfinder remembers between runs.
//
// Nothing here learns physics: the exact simulation already answers "does
// this work" perfectly, so there is nothing to learn about that. What is
// worth keeping is what got it past a hard spot on a level (the next run
// there tries it first and is through in one idea), how often each spot
// has bitten (a repair starts at the right distance at once), and which
// kinds of idea win in which situations across every level (the ones that
// usually work are tried first). Plain bookkeeping in JSON under the mod's
// save folder: one file per level and one shared. The game's own physics
// still judges every remembered idea like any other.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>


#include "absense/trajectory/trajectory.hpp"

class PlayLayer;

namespace absense::memory {

// What got the search past one spot: where it was, what the player was,
// and the inputs as timed events from the decision tick.
struct Solution {
    float x = 0.0f, y = 0.0f;
    int mode = 0;
    bool upsideDown = false;
    bool mini = false;
    float speed = 1.0f;
    bool held = false;
    std::vector<uint32_t> events;  // (tick << 3) | presses | (held ? 4 : 0)
    int lasted = 0;
    int uses = 0;
    int wins = 0;
    bool hard = false;
};

// A spot that bit, and how often.
struct Spot {
    float x = 0.0f;
    int fails = 0;
};

struct Level {
    std::string name;
    uint64_t key = 0;
    uint64_t runs = 0;
    double bestProgress = 0.0;
    std::vector<Solution> solutions;
    std::vector<Spot> spots;
};

struct Priors {
    // situation -> kind of idea -> how often it was the one played
    std::map<std::string, std::map<std::string, uint32_t>> wins;
    std::vector<uint64_t> levels;  // every level learned
};

class Store {
   public:
    static Store& get();

    // The level's memory is loaded when the pathfinder starts there and
    // saved when it stops.
    void open(PlayLayer* pl);
    void close(double progress);
    void save();
    // The periodic write, from the pathfinder's slice: never from inside a
    // live tick (see Store::remember).
    void saveIfDue();
    void forgetLevel();
    bool isOpen() const { return m_open; }

    // Solutions near a state, best first: the ideas to try before anything
    // else. Indices into level().solutions.
    void nearby(const Trajectory::StartState& st, std::vector<size_t>& out) const;
    void remember(const Trajectory::StartState& st, const std::vector<TickInput>& inputs, int lasted, bool hard);
    void used(size_t index);
    void won(size_t index);

    int spotFails(float x) const;
    void spotFailed(float x, int fails);

    // Which kinds of idea usually win here (for ordering the ideas).
    std::string situation(const Trajectory::StartState& st, bool repairing) const;
    void win(const std::string& situation, const std::string& ideaName);
    uint32_t score(const std::string& situation, const std::string& ideaName) const;
    static std::string kindOf(const std::string& ideaName);

    const Level& level() const { return m_level; }
    size_t levelsLearned() const { return m_priors.levels.size(); }

    static std::vector<TickInput> expand(const std::vector<uint32_t>& events, int ticks, bool startHeld);
    static std::vector<uint32_t> compress(const std::vector<TickInput>& inputs, bool startHeld);

   private:
    Store() = default;
    std::filesystem::path folder() const;
    std::filesystem::path levelPath() const;
    void loadPriors();

    Level m_level;
    Priors m_priors;
    bool m_open = false;
    bool m_priorsLoaded = false;
    bool m_dirty = false;
    bool m_priorsDirty = false;
    double m_lastSave = 0.0;
};

}  // namespace absense::memory

// Absense serialises these with glaze; GucciBot does it with Geode's matjson
// (memory.cpp, toJson/fromJson), keeping Absense's key names.

#endif  // PATHFINDER_MEMORY_HPP
