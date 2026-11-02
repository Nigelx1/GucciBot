// See judge.hpp.

#include "absense/judge.hpp"

#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <chrono>
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

    Result run(std::span<const TickInput> inputs, int ticks, std::vector<TraceSample>* trace,
               Options const& options) {
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
        r.onlyRefreshAtStart = upd.m_onlyRefresh;
        // A step someone else armed is theirs: taken out of the way here and
        // put back at the end.
        bool const stepArmedBefore = upd.consumeStep();

        s_active = true;
        s_died = false;
        s_complete = false;
        s_killer = nullptr;
        r.ran = true;

        int const n = std::max(0, ticks);
        // The side that moves player 1 (see sideFor; Flip 2-Player Controls
        // hands side 1 to player 2 in a 2-player level outside platformer).
        bool const side = gb->replay.sideFor(pl, false);
        using Clock = std::chrono::steady_clock;
        auto const playStart = Clock::now();
        for (int i = 0; i < n && !inputs.empty(); i++) {
            TickInput const& in = inputs[std::min((size_t)i, inputs.size() - 1)];
            // A tick's input as AbsensePathfinder::applyInput queues it.
            pl->m_queuedButtons.clear();
            bool down = jumpHeld(pl->m_player1);
            for (uint8_t k = 0; k < in.presses; k++) {
                if (down)
                    pl->queueButton(1, false, side, 0.0);
                pl->queueButton(1, true, side, 0.0);
                down = true;
            }
            if (down != in.held)
                pl->queueButton(1, in.held, side, 0.0);

            // One tick, as the pathfinder's stepGame runs one.
            auto const tickStart = options.tickUs ? Clock::now() : Clock::time_point{};
            upd.stepOnce();
            CCScheduler::get()->update(static_cast<float>(upd.getPhysicsDt()));
            if (options.tickUs)
                options.tickUs->push_back(
                    std::chrono::duration<float, std::micro>(Clock::now() - tickStart).count());
            if (upd.m_stepOnce_)
                r.stepLeftArmed++;

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
                // GucciBot (2026-10-24, step 3): gravity (never filled in
                // here before), kind, size and speed, as the copies' traces
                // have them. What the game fired is not followed here (0).
                s.upsideDown = p->m_isUpsideDown;
                s.mode = (int8_t)Trajectory::modeOf(p);
                s.mini = p->m_vehicleSize != 1.0f;
                s.speed = p->m_playerSpeed;
                trace->push_back(s);
            }
            if (s_died && options.throughDeaths) {
                // GucciBot (2026-11-01, H0): noted, and the run goes on (the
                // hook kept the player alive). The next tick's first hit is
                // the next one noted.
                NotedDeath d;
                d.tick = i + 1;
                if (s_killer) {
                    d.uid = s_killer->m_uniqueID;
                    d.id = s_killer->m_objectID;
                    d.type = static_cast<int>(s_killer->m_objectType);
                    d.x = (float)s_killer->m_positionX;
                    d.y = (float)s_killer->m_positionY;
                }
                r.deaths.push_back(d);
                s_died = false;
                s_killer = nullptr;
            }
            if (s_died || s_complete)
                break;
            r.survived = i + 1;
            if (options.maxMs > 0.0 && i + 1 < n &&
                std::chrono::duration<double, std::milli>(Clock::now() - playStart).count() >= options.maxMs) {
                r.stoppedByClock = true;
                break;
            }
        }

        r.died = s_died;
        r.complete = s_complete;
        if (s_killer) {
            r.killerId = s_killer->m_objectID;
            r.killerUid = s_killer->m_uniqueID;
            r.killerType = static_cast<int>(s_killer->m_objectType);
            // GucciBot (2026-10-24): where it really is, as the copies' killer
            // is read now (the sprite slides with an enter effect).
            r.killerX = (float)s_killer->m_positionX;
            r.killerY = (float)s_killer->m_positionY;
        } else if (!r.deaths.empty()) {
            // Carried on through its deaths: the first one.
            NotedDeath const& d = r.deaths.front();
            r.died = true;
            r.killerId = d.id;
            r.killerUid = d.uid;
            r.killerType = d.type;
            r.killerX = d.x;
            r.killerY = d.y;
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
        // Nothing of the run's own is left to step once it is over (in -bl
        // every run left one armed, and the next frame played it: the game
        // came back a tick later than it started).
        upd.consumeStep();
        if (stepArmedBefore)
            upd.stepOnce();
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
