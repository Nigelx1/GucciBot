#ifndef PATHFINDER_HUMAN_HPP
#define PATHFINDER_HUMAN_HPP

// Absense - a human's macro for the level, as an idea.
//
// The replays folder holds a great many human-made macros, one per level
// name. When the search runs on a level that has one, the human's inputs
// around the current tick (converted to the game's tick rate) go in as an
// idea at every decision, a tick or two either side as well. The game's
// own physics judges it like any other idea, so where the human's route
// still holds the decision is one idea, and where it does not (a different
// tick rate, a different approach) the search carries on as usual.

#include <cstdint>
#include <string>
#include <vector>

#include "absense/compat/bot.hpp"  // slc::ActionAtom

#include "absense/trajectory/trajectory.hpp"

class PlayLayer;

namespace absense::human {

class Reference {
   public:
    static Reference& get();

    // Looks for the level's macro in the replays folder; failing that, uses
    // the actions loaded in the replay system when the search started (a
    // macro the player loaded by hand), at the tick rate they are in.
    void open(PlayLayer* pl, const slc::ActionAtom* loaded, double loadedTps);
    void close();

    bool available() const { return !m_events.empty(); }
    const std::string& source() const { return m_source; }
    double tps() const { return m_tps; }
    size_t presses() const { return m_presses; }

    // The human's button over [tick, tick + count) at the game's tick rate,
    // shifted by `shift` ticks, starting from the button as it is now.
    // False when the human's inputs end before `tick`.
    bool script(uint64_t tick, int count, double gameTps, int shift, bool startHeld, std::vector<TickInput>& out) const;

   private:
    Reference() = default;
    struct Ev {
        double second;  // where the event falls in the macro's own time
        bool holding;
    };
    std::vector<Ev> m_events;  // player 1's jump button, in time order
    double m_tps = 240.0;      // the rate it starts at (what the card shows)
    std::string m_source;
    double m_lastSecond = 0.0;
    size_t m_presses = 0;
};

}  // namespace absense::human

#endif  // PATHFINDER_HUMAN_HPP
