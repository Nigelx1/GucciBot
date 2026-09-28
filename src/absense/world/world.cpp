#include "absense/world/world.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>

#include "absense/compat/bot.hpp"
#include "absense/physics/collisions.hpp"
#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"

namespace world {

namespace {
WorldState* g_current = nullptr;
Run* g_run = nullptr;
// What teleportSeed hands out when there is no trajectory to hold the old
// state (a group teleport can only come from a copy, so never in practice).
uint64_t g_spareSeed = 0;
}  // namespace

WorldState* current() { return g_current; }
Run* currentRun() { return g_run; }

// A state made current on its own (the drawn prediction without a run of its
// own) is not any run's: the run slot is cleared for its lifetime.
Scope::Scope(WorldState* ws) : was(g_current), wasRun(g_run) {
    g_current = ws;
    g_run = nullptr;
}
Scope::Scope(Run* run) : was(g_current), wasRun(g_run) {
    g_run = run;
    g_current = run ? run->ws : nullptr;
}
Scope::Scope(Run* run, WorldState* ws) : was(g_current), wasRun(g_run) {
    g_run = run;
    g_current = run ? run->ws : ws;
}
Scope::~Scope() {
    g_current = was;
    g_run = wasRun;
}

int copyIndex(PlayerObject* copy) {
    Trajectory* t = Bot::get()->trajectory().unsafeInner();
    return (t && copy == t->m_fakePlayer2) ? 1 : 0;
}

bool canActivate(GJBaseGameLayer* pl, PlayerObject* copy, EffectGameObject* object) {
    WorldState* ws = current();
    if (!ws) return phys::copyCanBeActivated(pl, copy, object);
    const int idx = copyIndex(copy);
    auto& contacts = ws->contact[idx];
    const int uid = object->m_uniqueID;
    const uint32_t stamp = pl->m_gameState.m_commandIndex;
    // 0x2178d4-0x2178ea: the object's own rule, asked with the player's
    // platformer flag (+0xb70).
    const bool multi = object->canMultiActivate(copy->m_isPlatformer);
    auto it = std::lower_bound(contacts.begin(), contacts.end(), uid,
                               [](const std::pair<int, uint32_t>& e, int key) { return e.first < key; });
    const bool touching = it != contacts.end() && it->first == uid;
    // Both ways through the game's function stamp the contact (0x217962 and
    // 0x217993), a refusal included.
    if (touching) it->second = stamp;
    else contacts.insert(it, {uid, stamp});
    if (multi && touching) return false;
    // hasBeenActivatedByPlayer 0x1a4af0 is false for a multi-activate object,
    // and otherwise the flag of the player. The game picks the flag by
    // m_uniqueID == 1; a copy's id never is, so the copy of player 1 would read
    // player 2's flag - the copy picks it by which copy it is.
    if (multi) return true;
    return !(idx == 1 ? object->m_activatedByPlayer2 : object->m_activatedByPlayer1);
}

void ageContacts(WorldState& ws, uint32_t commandIndex) {
    for (auto& contacts : ws.contact) {
        std::erase_if(contacts, [commandIndex](const std::pair<int, uint32_t>& e) { return e.second < commandIndex; });
    }
}

void restampContacts(WorldState& ws, uint32_t commandIndex) {
    for (auto& contacts : ws.contact) {
        for (auto& e : contacts) e.second = commandIndex;
    }
}

float nextRandom(uint64_t& seed) {
    // ReplaySystem's advanceState (hooks/GJBaseGameLayer.cpp), which the game
    // runs in place of rand() here: the product is taken in 64 bits and only
    // the 15-bit value is kept as the next state. The design had the plain
    // MSVC generator (the full 32-bit seed kept), which is what the game's
    // inlined rand() does to its own global at 0x6c2ef8 - but the midhook
    // replaces the value the pick uses, so that is not what decides the target.
    seed = static_cast<int>((214013 * seed + 2531011) >> 16) & 0x7FFF;
    // cvtdq2ps and divss by 32767.0f (0x20fee0-0x20fee3).
    return (float)static_cast<int>(seed) / 32767.0f;
}

uint64_t& teleportSeed() {
    if (WorldState* ws = current()) return ws->seed;
    if (Trajectory* t = Bot::get()->trajectory().unsafeInner()) return t->teleportRand();
    return g_spareSeed;
}

}  // namespace world
