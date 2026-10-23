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

void Engine::arm(uint32_t frame, double fraction, int player) {
    if (!(fraction > 0.0 && fraction < 1.0)) return;

    // GucciBot (2026-10-23): one arm per frame and player (see Armed::player);
    // with every arm at -1, one per frame, as before.
    for (auto& a : m_armed) {
        if (a.frame == frame && a.player == player) {
            a.fraction = fraction;
            return;
        }
    }
    m_armed.push_back(Armed{frame, fraction, player});
}

Armed const* Engine::findArmed(uint32_t frame, int player) const {
    for (auto const& a : m_armed)
        if (a.frame == frame && (player < 0 || a.player < 0 || a.player == player))
            return &a;
    return nullptr;
}

bool Engine::hasPendingFor(bool player2) const {
    for (auto const& d : m_pending)
        if (d.player2 == player2) return true;
    return false;
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

    // GucciBot (2026-10-23): an arm for the other player doesn't take this
    // input (Armed::player); an arm for any player, the only kind without
    // Calculate's 2-player fixes, takes it as before.
    Armed const* a = this->findArmed(frame, player2 ? 1 : 0);
    if (!a) return false;

    m_tickFrame = frame;
    m_tickFraction = a->fraction;
    // One player's tick only if every input it took was armed for that player.
    m_tickPlayer = m_pending.empty() || m_tickPlayer == a->player ? a->player : -1;
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
    m_tickPlayer = -1;
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
