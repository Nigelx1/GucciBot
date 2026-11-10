#include <Geode/Geode.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>

#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "core/GucciBot.hpp"
#include "core/platform.hpp"

using namespace geode::prelude;
using namespace gucci;

// Deterministic Random triggers, ported from Silicate 2026-09-22.
//
// GD's Advanced Random trigger (object 2068) picks one of several groups by weighted
// chance, using the game's shared RNG. Nothing about that is reproducible, so
// a macro recorded through a level with Random triggers replays down a
// different branch every attempt, and no amount of checkpoint fidelity saves
// it -- the divergence is in which objects spawn at all.
//
// GucciBot already carried the seed this needs (the macro header's rngSeed ->
// m_startingSeed -> m_startingSeedThisAttempt) and simply had no Random-trigger
// hook to spend it in.
//
// Each trigger keeps its own LCG state, seeded from the attempt seed and the
// object's unique id, and registers a pointer to that state with the practice
// fix so checkpoints can capture and restore it. The selection below
// reimplements GD's weighted pick against that state instead of the shared RNG.
//
// The plain Random trigger (object 1912) is handled in
// EffectGameObject::triggerObject (0x4a5f30 on Windows), whose 1912 case draws
// once from GD's shared fast random (0x6c2e90 on Windows) and spawns one of
// two groups. RandTriggerGameObject::triggerObject (0x4b41e0) passes every id
// but 2068 to that function too, so the hook at the end of this file is on
// EffectGameObject and catches 1912 whichever class the object is.
// The game also draws from that shared state for spawn delays and particles,
// so the value it holds when a 1912 trigger fires differed between the
// recording and every replay (seen in-game 2026-11-10, build -at). While a
// macro records or plays (or Calculate replays one), the pick is made from a
// value built from the macro's seed, the trigger's unique id and the frame
// it fires on: the shared state is saved, that value written, GD's own code
// run, and the saved state put back, so nothing else in the game sees a
// change. The key holds no state of its own, so a practice checkpoint respawn
// gives the same pick for free. Plain play (Off) keeps GD's own random.
struct GB7RandTriggerGameObject
    : Modify<GB7RandTriggerGameObject, RandTriggerGameObject> {
    struct Fields {
        uint64_t m_randomState = std::numeric_limits<uint64_t>::max();
    };

    uint64_t takeNextRandomState() {
        uint64_t const state = m_fields->m_randomState;
        m_fields->m_randomState = 214013 * state + 2531011;
        return state;
    }

    int generateValueWithMax(int max) {
        return static_cast<int>(
            std::round(static_cast<float>((this->takeNextRandomState() >> 16) & 0x7FFF) /
                       32767.0f * static_cast<float>(max)));
    }

    void customObjectSetup(gd::vector<gd::string>& values,
                           gd::vector<void*>& exists) override {
        RandTriggerGameObject::customObjectSetup(values, exists);
        GucciEngine::get()->practiceFix.m_advancedRandom.push_back(
            {&this->m_fields->m_randomState, this->m_uniqueID});
    }

    void triggerObject(GJBaseGameLayer* layer,
                       int uniqueID,
                       gd::vector<int> const* remapKeys) override {
        auto* gb = GucciEngine::get();
        if (m_objectID != 2068 || !gb->enabled)
            return RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);

        if (m_fields->m_randomState == std::numeric_limits<uint64_t>::max())
            m_fields->m_randomState = gb->replay.m_startingSeedThisAttempt ^
                                      (static_cast<uint64_t>(m_uniqueID) * 2137);

        int sum = 0;
        for (auto const& co : m_chanceObjects)
            sum += co.m_chance;

        int const threshold = this->generateValueWithMax(sum);
        int runningSum = 0, i = 0, groupID = 0;

        while (runningSum < sum) {
            ChanceObject& co = m_chanceObjects[i++];
            runningSum += co.m_chance;
            groupID = co.m_groupID;
            if (runningSum >= threshold)
                break;
            if (static_cast<size_t>(i) == m_chanceObjects.size())
                break;
        }

        std::vector<int> keys;
        if (remapKeys)
            keys = *remapKeys;

        layer->spawnGroup(groupID, false, 0.0, keys, m_uniqueID, m_controlID);
    }
};

// Object 1912 (see the top of the file). The same trigger firing more than
// once on one frame of one attempt (a spawn loop) gets a different value each
// time.
struct GB7RandomEffectGameObject : Modify<GB7RandomEffectGameObject, EffectGameObject> {
    void triggerObject(GJBaseGameLayer* layer,
                       int uniqueID,
                       gd::vector<int> const* remapKeys) override {
        auto* gb = GucciEngine::get();
        if (m_objectID != 1912 || !gb->enabled || (gb->isIdle() && !gb->analyzerOwnsRun()))
            return EffectGameObject::triggerObject(layer, uniqueID, remapKeys);

        // The count starts again on a new frame or a new attempt: a practice
        // respawn plays the checkpoint's frames again, and a trigger that
        // fires on the same frame as before must pick the same way.
        static uint32_t s_frame = std::numeric_limits<uint32_t>::max();
        static int s_attempt = -1;
        static std::unordered_map<int, uint32_t> s_firedThisFrame;
        uint32_t const frame = gb->updater.getFrame();
        int const attempt = layer ? layer->m_attempts : 0;
        if (frame != s_frame || attempt != s_attempt) {
            s_frame = frame;
            s_attempt = attempt;
            s_firedThisFrame.clear();
        }
        uint32_t const repeat = s_firedThisFrame[m_uniqueID]++;

        // splitmix64 over the seed, the trigger and the frame.
        uint64_t key = gb->replay.m_startingSeed ^
                       (static_cast<uint64_t>(static_cast<uint32_t>(m_uniqueID)) * 0x9E3779B97F4A7C15ull) ^
                       (static_cast<uint64_t>(frame) * 0xC2B2AE3D27D4EB4Full) ^
                       (static_cast<uint64_t>(repeat) * 0x165667B19E3779F9ull);
        key += 0x9E3779B97F4A7C15ull;
        key = (key ^ (key >> 30)) * 0xBF58476D1CE4E5B9ull;
        key = (key ^ (key >> 27)) * 0x94D049BB133111EBull;
        key ^= key >> 31;

        uint64_t const saved = readGdRandom();
        writeGdRandom(key);
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
        writeGdRandom(saved);
        log::debug("Random trigger {} on frame {} (seed {}, repeat {}): value {}", m_uniqueID, frame,
                   gb->replay.m_startingSeed, repeat, key);
    }
};
