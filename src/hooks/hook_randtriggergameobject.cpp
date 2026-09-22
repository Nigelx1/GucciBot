#include <Geode/Geode.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>

#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include "core/GucciBot.hpp"

using namespace geode::prelude;
using namespace gucci;

// Deterministic Random triggers, ported from Silicate 2026-09-22.
//
// GD's Random trigger (object 2068) picks one of several groups by weighted
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
