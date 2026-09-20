#include "cbf.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include <algorithm>
#include <limits>

using namespace geode::prelude;

namespace cbf {

static constexpr double SMALLEST_FLOAT =
    static_cast<double>(std::numeric_limits<float>::min());

Engine* Engine::get() {
    static Engine instance;
    return &instance;
}

void Engine::arm(uint32_t frame, double fraction) {
    if (fraction <= 0.0) return;

    double const f = std::clamp(fraction, SMALLEST_FLOAT, 1.0 - SMALLEST_FLOAT);

    for (auto& a : m_armed) {
        if (a.frame == frame) {
            a.fraction = f;
            return;
        }
    }
    m_armed.push_back(Armed{frame, f});
}

double const* Engine::findArmed(uint32_t frame) const {
    for (auto const& a : m_armed)
        if (a.frame == frame) return &a.fraction;
    return nullptr;
}

void Engine::disarm() { this->reset(); }

void Engine::reset() {
    m_armed.clear();
    m_tickFrame = 0;
    m_tickFraction = 0.0;
    m_pending.clear();
    m_queue.clear();
    m_cursor = 0;
    m_fired = false;
    m_midStep = false;
    m_p1Split = false;
    m_p2Split = false;
    m_p2Handled = false;
    m_rotationDelta = 0.f;
    m_shipRotAccum = 0.f;
    m_shipRotAccumP2 = 0.f;
    m_shipRotHeld = false;
}

bool Engine::capture(uint32_t frame, int button, bool holding, bool player2) {
    if (m_armed.empty()) return false;
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
    if (m_tickFraction <= 0.0) return false;
    if (!m_queue.empty()) return false;  // already inside this tick

    m_queue.push_back(
        Step{std::clamp(m_tickFraction, SMALLEST_FLOAT, 1.0), false});
    m_queue.push_back(
        Step{std::max(SMALLEST_FLOAT, 1.0 - m_tickFraction), true});
    m_cursor = 0;
    m_fired = false;
    return true;
}

Step Engine::pop() {
    if (m_cursor >= m_queue.size()) return Step{1.0, true};
    return m_queue[m_cursor++];
}

void Engine::fire() {
    if (m_fired) return;
    m_fired = true;

    for (size_t i = 0; i < m_armed.size(); i++) {
        if (m_armed[i].frame == m_tickFrame) {
            m_armed.erase(m_armed.begin() + i);
            break;
        }
    }

    auto* gjbgl = GJBaseGameLayer::get();
    if (!gjbgl) return;

    for (auto const& d : m_pending)
        gjbgl->handleButton(d.holding, d.button, !d.player2);
}

void Engine::endTick() {
    m_tickFraction = 0.0;
    m_pending.clear();
    m_queue.clear();
    m_cursor = 0;
    m_fired = false;
    m_midStep = false;
}

bool Engine::flushOrphaned() {
    if (m_pending.empty() || m_fired) {
        if (m_fired) this->endTick();
        return false;
    }

    log::warn(
        "[cbf] tick {} never reached the split loop -- firing {} deferred "
        "input(s) at the tick boundary instead",
        m_tickFrame, m_pending.size());
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
