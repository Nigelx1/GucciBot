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
};

class Engine {
   public:
    static Engine* get();

    void arm(uint32_t frame, double fraction);
    bool isArmed(uint32_t frame) const { return this->findArmed(frame); }

    bool capture(uint32_t frame, int button, bool holding, bool player2);
    bool hasPending() const { return !m_pending.empty(); }

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
    double const* findArmed(uint32_t frame) const;

    std::vector<Armed> m_armed;
    uint32_t m_tickFrame = 0;
    double m_tickFraction = 0.0;

    std::vector<Deferred> m_pending;
    bool m_fired = false;
};

void resetCollisionLog(PlayerObject* p);
bool canSplit(PlayerObject* p, bool tickGround);

}  // namespace cbf

#endif  // ANALYSIS_CBF_HPP
