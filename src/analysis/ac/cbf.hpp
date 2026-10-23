#ifndef ANALYSIS_CBF_HPP
#define ANALYSIS_CBF_HPP

#include <cocos2d.h>

#include <cstdint>
#include <vector>

class PlayerObject;
class GJBaseGameLayer;

namespace cbf {

struct Deferred {
    int button = 1;
    bool holding = false;
    bool player2 = false;
};

struct Armed {
    uint32_t frame = 0;
    double fraction = 0.0;
    // GucciBot (2026-10-23): whose input the split is for: 0 player 1, 1
    // player 2, -1 any input on the tick (every arm except the ones
    // Calculate's 2-player fixes make in a 2-player level).
    int player = -1;
};

class Engine {
   public:
    static Engine* get();

    // GucciBot (2026-10-23): `player` (Armed::player) is -1 for every caller
    // but Calculate's 2-player fixes, which split a tick in a 2-player level
    // for the measured player alone, so the other player's click on the same
    // tick stays on the tick edge instead of moving with it (anticroom's
    // antics-counter beta.4 changelog; no source). At -1 it is the engine as
    // it was: one arm per frame, any input on it captured.
    void arm(uint32_t frame, double fraction, int player = -1);
    bool isArmed(uint32_t frame, int player = -1) const {
        return this->findArmed(frame, player) != nullptr;
    }

    bool capture(uint32_t frame, int button, bool holding, bool player2);
    bool hasPending() const { return !m_pending.empty(); }
    // GucciBot (2026-10-23): an input of that player waits for this tick's
    // split, and whether this tick was armed for one player only.
    bool hasPendingFor(bool player2) const;
    bool splitsOnePlayer() const { return m_tickPlayer >= 0; }

    bool beginTick();
    double fraction() const { return m_tickFraction; }
    void fire();
    void endTick();

    bool flushOrphaned();

    void reset();

    float m_rotationDelta = 0.f;
    bool m_midStep = false;
    bool m_p1Split = false;
    bool m_p2Split = false;
    cocos2d::CCPoint m_p1Pos{};
    cocos2d::CCPoint m_p2Pos{};

    bool m_p2Handled = false;

    float m_shipRotAccum = 0.f;
    float m_shipRotAccumP2 = 0.f;
    bool m_shipRotHeld = false;

   private:
    // The arm on `frame` that takes `player`'s input (-1: any arm on it).
    Armed const* findArmed(uint32_t frame, int player) const;

    std::vector<Armed> m_armed;
    uint32_t m_tickFrame = 0;
    double m_tickFraction = 0.0;
    int m_tickPlayer = -1;

    std::vector<Deferred> m_pending;
    bool m_fired = false;
};

void resetCollisionLog(PlayerObject* p);
bool canSplit(PlayerObject* p, bool tickGround);

}  // namespace cbf

#endif  // ANALYSIS_CBF_HPP
