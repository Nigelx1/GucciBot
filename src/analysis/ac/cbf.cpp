#include "cbf.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace cbf {

Engine* Engine::get() {
    static Engine instance;
    return &instance;
}

void Engine::arm(uint32_t frame, double fraction) {
    if (!(fraction > 0.0 && fraction < 1.0)) return;

    for (auto& a : m_armed) {
        if (a.frame == frame) {
            a.fraction = fraction;
            return;
        }
    }
    m_armed.push_back(Armed{frame, fraction});
}

double const* Engine::findArmed(uint32_t frame) const {
    for (auto const& a : m_armed)
        if (a.frame == frame) return &a.fraction;
    return nullptr;
}

void Engine::reset() {
    m_armed.clear();
    m_tickFrame = 0;
    m_pending.clear();
    this->endTick();
    m_p1Split = false;
    m_p2Split = false;
    m_p2Handled = false;
    m_rotationDelta = 0.f;
    m_shipRotAccum = 0.f;
    m_shipRotAccumP2 = 0.f;
    m_shipRotHeld = false;
}

bool Engine::capture(uint32_t frame, int button, bool holding, bool player2) {
    if (button < 1 || button > 3) return false;
    if (!m_pending.empty() && frame != m_tickFrame) return false;

    double const* f = this->findArmed(frame);
    if (!f) return false;

    m_tickFrame = frame;
    m_tickFraction = *f;
    m_pending.push_back({button, holding, player2});
    return true;
}

bool Engine::beginTick() {
    if (m_pending.empty()) return false;
    m_fired = false;
    return true;
}

void Engine::fire() {
    if (m_fired) return;
    m_fired = true;

    std::erase_if(m_armed,
                  [this](Armed const& a) { return a.frame == m_tickFrame; });

    auto* gjbgl = GJBaseGameLayer::get();
    if (!gjbgl) return;

    for (auto const& d : m_pending)
        gjbgl->handleButton(d.holding, d.button, !d.player2);
}

void Engine::endTick() {
    m_tickFraction = 0.0;
    m_pending.clear();
    m_fired = false;
    m_midStep = false;
}

bool Engine::flushOrphaned() {
    if (m_pending.empty()) return false;

    log::warn("[cbf] {} input(s) never got split on tick {}, firing on edge",
              m_pending.size(), m_tickFrame);
    this->fire();
    this->endTick();
    return true;
}

bool canSplit(PlayerObject* p, bool tickGround) {
    if (!p) return false;
    if (!tickGround) return true;
    if (p->m_isOnGround) return true;
    if (p->m_touchingRings && p->m_touchingRings->count()) return true;
    if (p->m_isDashing) return true;
    return p->m_isDart || p->m_isBird || p->m_isShip || p->m_isSwing;
}

void resetCollisionLog(PlayerObject* p) {
    if (!p) return;

    if (p->m_collisionLogTop) p->m_collisionLogTop->removeAllObjects();
    if (p->m_collisionLogBottom) p->m_collisionLogBottom->removeAllObjects();
    if (p->m_collisionLogLeft) p->m_collisionLogLeft->removeAllObjects();
    if (p->m_collisionLogRight) p->m_collisionLogRight->removeAllObjects();

    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
}

}  // namespace cbf
