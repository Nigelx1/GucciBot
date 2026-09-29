// See judge.hpp.

#include "absense/judge.hpp"

#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <unordered_set>

using namespace geode::prelude;

namespace absense::judge {

    namespace {
        bool s_active = false;
        bool s_died = false;
        bool s_complete = false;
        GameObject* s_killer = nullptr;
        std::unordered_set<int> s_distrusted;

        bool jumpHeld(PlayerObject* p) {
            if (!p)
                return false;
            auto const it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            return it != p->m_holdingButtons.end() && it->second;
        }
    }  // namespace

    bool active() {
        return s_active;
    }

    void noteDeath(GameObject* object) {
        if (s_died)
            return;  // the first hit of a tick is the one
        s_died = true;
        s_killer = object;
    }

    void noteComplete() {
        s_complete = true;
    }

    void distrust(int uid) {
        if (uid != 0)
            s_distrusted.insert(uid);
    }

    void clearDistrust() {
        s_distrusted.clear();
    }

    bool distrusted(int uid) {
        return uid != 0 && s_distrusted.count(uid) != 0;
    }

    std::vector<int> distrustedList() {
        std::vector<int> out(s_distrusted.begin(), s_distrusted.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    Result run(std::span<const TickInput> inputs, int ticks, std::vector<TraceSample>* trace) {
        Result r;
        auto* pl = PlayLayer::get();
        auto* gb = gucci::GucciEngine::get();
        if (!pl || !pl->m_player1) {
            r.error = "not in a level";
            return r;
        }
        if (s_active) {
            r.error = "a run is already going";
            return r;
        }
        if (pl->m_player1->m_isDead || pl->m_isPaused) {
            r.error = pl->m_isPaused ? "the pause menu is open" : "the player is dead";
            return r;
        }
        auto& upd = gb->updater;
        auto& pf = gb->practiceFix;

        // What to put back. The state is captured the way the pathfinder keeps
        // its anchors (keepAnchor), which restore exactly.
        auto const savedMode = gb->mode;
        bool const savedPaused = upd.m_paused;
        bool const savedBackstep = upd.m_backwardsStepping;
        uint32_t const startFrame = upd.getFrame();
        r.startFrame = startFrame;
        r.xBefore = pl->m_player1->getPositionX();
        CheckpointObject* cp = pl->createCheckpoint();
        if (!cp) {
            r.error = "the game made no checkpoint";
            return r;
        }
        cp->retain();
        gucci::SavedCheckpointState const state = pf.createCheckpoint(cp, startFrame);

        // Nothing the run does is recorded or played (idle), and nothing is
        // stored for stepping back through (the store holds the real path).
        if (savedMode != gucci::GucciEngine::Mode::Idle)
            gb->setMode(gucci::GucciEngine::Mode::Idle);
        upd.m_backwardsStepping = false;
        upd.setPaused(true);

        s_active = true;
        s_died = false;
        s_complete = false;
        s_killer = nullptr;
        r.ran = true;

        int const n = std::max(0, ticks);
        for (int i = 0; i < n && !inputs.empty(); i++) {
            TickInput const& in = inputs[std::min((size_t)i, inputs.size() - 1)];
            // A tick's input as AbsensePathfinder::applyInput queues it.
            pl->m_queuedButtons.clear();
            bool down = jumpHeld(pl->m_player1);
            for (uint8_t k = 0; k < in.presses; k++) {
                if (down)
                    pl->queueButton(1, false, false, 0.0);
                pl->queueButton(1, true, false, 0.0);
                down = true;
            }
            if (down != in.held)
                pl->queueButton(1, in.held, false, 0.0);

            // One tick, as the pathfinder's stepGame runs one.
            upd.stepOnce();
            CCScheduler::get()->update(static_cast<float>(upd.getPhysicsDt()));

            if (trace) {
                auto* p = pl->m_player1;
                TraceSample s;
                s.tick = i + 1;
                s.x = p->getPositionX();
                s.y = p->getPositionY();
                s.yVel = static_cast<float>(p->m_yVelocity);
                s.xVel = static_cast<float>(p->m_platformerXVelocity);
                s.rotation = p->getRotation();
                s.onGround = p->m_isOnGround;
                s.held = jumpHeld(p);
                s.dead = s_died;
                trace->push_back(s);
            }
            if (s_died || s_complete)
                break;
            r.survived = i + 1;
        }

        r.died = s_died;
        r.complete = s_complete;
        if (s_killer) {
            r.killerId = s_killer->m_objectID;
            r.killerUid = s_killer->m_uniqueID;
            r.killerType = static_cast<int>(s_killer->m_objectType);
            r.killerX = s_killer->getPositionX();
            r.killerY = s_killer->getPositionY();
        }

        // Back to where it started: the player, the level and the frame
        // counter (resetWithState sets it from the state's frame).
        pf.m_isBackstep = true;
        pf.resetWithState(state);
        pf.m_isBackstep = false;
        s_active = false;
        r.frameAfter = upd.getFrame();
        r.xAfter = pl->m_player1->getPositionX();
        r.stepArmedAfter = upd.m_stepOnce_;
        s_killer = nullptr;
        cp->release();

        upd.m_backwardsStepping = savedBackstep;
        if (savedMode != gucci::GucciEngine::Mode::Idle)
            gb->setMode(savedMode);
        upd.setPaused(savedPaused);
        // The look-ahead's copies are set up from the real player again.
        ::Bot::get()->trajectory().realStateChanged();
        return r;
    }

}  // namespace absense::judge
