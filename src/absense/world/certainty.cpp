// How sure the World is about an object (design step 13), and the count of
// the times it has been caught being wrong.
//
// A run that dies asks certaintyOf about what killed it: the pathfinder keeps
// its plans and its dead ends by that answer, so it has to be the careful one.
// Static is a promise about the level, Modelled a promise about the run, and
// everything else is Uncertain - a kind that is not ported yet, a group
// something the World does not follow has touched, or a run that carries its
// objects the old way (MovingObjects) all end there.

#include "absense/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_set>

#include <Geode/binding/PlayLayer.hpp>

#include "absense/compat/devlog.hpp"
#include "absense/world/def.hpp"
#include "absense/world/ledger.hpp"

namespace world {

namespace {

// Since the process started: a level reload keeps counting, and the pathfinder
// takes the difference from where its own run began.
uint64_t g_drifts = 0;

bool inUncertain(const std::vector<uint16_t>& list, int group) {
    const uint16_t g = (uint16_t)WorldDef::clampGroup(group);
    return std::binary_search(list.begin(), list.end(), g);
}

// PlayLayer::m_dynamicSaveObjects: the objects the game keeps the state of,
// which is what it does for one a dynamic move or an area effect has touched.
// Something moves those without naming a group of them, so they are never
// static. Only asked outside a run (a run has WorldState::uncertainAny for
// this), which is once per real death.
bool dynamicSaved(const GameObject* object) {
    PlayLayer* pll = PlayLayer::get();
    if (!pll) return false;
    const gd::vector<GameObject*>& dyn = pll->m_dynamicSaveObjects;
    if (dyn.empty()) return false;
    // Walking that list for every run that dies is most of a screening pass on
    // a level with thousands of objects, so it is kept as a set and read again
    // when the list grows or the level changes. The pointers are only ever
    // compared, never followed, and a stale one only ever says "uncertain",
    // which is the careful answer.
    static std::unordered_set<const GameObject*> saved;
    static const PlayLayer* savedFor = nullptr;
    static std::size_t savedSize = (std::size_t)-1;
    if (savedFor != pll || savedSize != dyn.size()) {
        saved.clear();
        saved.insert(dyn.begin(), dyn.end());
        savedFor = pll;
        savedSize = dyn.size();
    }
    return saved.count(object) != 0;
}

}  // namespace

uint64_t drifts() { return g_drifts; }

void noteDrift(const char* what) {
    g_drifts++;
    geode::log::warn("World: {}", what ? what : "drift");
    devlog::logf(devlog::Cat::System, "world: drift - %s", what ? what : "");
}

const char* certaintyName(Certainty certainty) {
    switch (certainty) {
        case Certainty::Static: return "static";
        case Certainty::Modelled: return "modelled";
        default: return "uncertain";
    }
}

Certainty certaintyOf(GJBaseGameLayer* pl, GameObject* object) {
    if (!object) return Certainty::Uncertain;
    // Where the level put it against where it stands: whatever names it, an
    // object that has already moved is not one nothing has touched.
    const cocos2d::CCPoint start = object->m_startPosition;
    const bool moved = std::abs(object->getPositionX() - start.x) > 0.01f ||
                       std::abs(object->getPositionY() - start.y) > 0.01f;

    Run* run = currentRun();
    const WorldDef* def = run ? run->def : nullptr;
    std::shared_ptr<const WorldDef> held;
    if (!def && !World::disabled) {
        // Asked outside a run (about the object the real game killed the
        // player with): the level's own read, which is cached after the first.
        // Never while the World is off - the offsets a WorldDef is read
        // through are exactly what turning it off says do not hold.
        held = WorldDef::get(pl);
        def = held.get();
    }
    // Something not tied to groups can move it: an area effect running in the
    // run, or - with no run to ask - the game having saved this object's state.
    const WorldState* ws = run ? run->ws : nullptr;
    const bool loose = ws ? ws->uncertainAny : dynamicSaved(object);

    const int slot = def ? def->slotOf(object) : -1;
    if (!def || slot < 0) {
        // No read of the level to ask, or an object of another one: only an
        // object nothing can name at all is certain. This is the fallback path
        // (the World off, the level still loading), and it is the careful one:
        // an object in a group no trigger names used to count as static, and
        // here it does not.
        return !moved && !loose && object->m_groupCount <= 0 ? Certainty::Static : Certainty::Uncertain;
    }

    const std::span<const uint16_t> groups = def->groupsOf(slot);
    // Static: no effect object in the level names a group of it, and it stands
    // where the level put it. Nothing can move it, in this run or any other.
    bool named = false;
    for (const uint16_t g : groups) {
        if (!def->targetingGroup((int)g).empty()) {
            named = true;
            break;
        }
    }
    if (!named && !moved && !loose) return Certainty::Static;

    // Modelled is a promise about this run: it only holds for a run that
    // follows where its objects stand (its state carries the running actions
    // and its start is exact, so run.objects is set). A run that carries them
    // at the speed of their last real tick knows no better than before, and an
    // area effect moves what is inside it whatever its groups.
    if (!run || !ws || !run->objects || loose) return Certainty::Uncertain;
    for (const uint16_t g : groups) {
        // Something the World does not follow has already moved or toggled it
        // this run, or a kind it does not run can reach it at all.
        if (inUncertain(ws->uncertainGroups, (int)g)) return Certainty::Uncertain;
        if (!def->groupModelled((int)g)) return Certainty::Uncertain;
        // ... or the ledger's shadow check caught the step having this group
        // somewhere else than the real game did (design step 10). The design
        // kept an untrusted set of kinds; a group is what the check can name
        // from a pose that came out wrong, and it is the narrower answer.
        if (ledgerUntrustedGroup((int)g)) return Certainty::Uncertain;
    }
    return Certainty::Modelled;
}

}  // namespace world
