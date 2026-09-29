#include "core/GucciBot.hpp"
#include "analysis/ac/cbf.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/trajectory.hpp"
#include "hooks/util_midhook.hpp"
#include "absense/compat/bot.hpp"
#include "absense/trajectory/trajectory.hpp"
#include "mcp/tickprobe.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
using namespace geode::prelude;

using namespace gucci;

// GD quantises y-velocity to a fixed 0.001 step. Above the macro's recorded
// tick rate that is coarser than the simulation, so the extra ticks buy
// nothing. Silicate scales the step down by the ratio ("High TPS Precision").
static constexpr double kVanillaQuantum = 0.001;

static double yVelocityQuantum() {
    auto* gb = GucciEngine::get();
    if (!gb->updater.m_highTpsPrecision || !gb->enabled)
        return kVanillaQuantum;
    double const tps = gb->updater.m_tps;
    double ref = gb->replay.m_initialTPS;
    if (!(ref > 0.0))
        ref = 240.0;
    if (!(tps > ref))
        return kVanillaQuantum;
    return kVanillaQuantum * (ref / tps);
}

static double quantise(double value, double quantum) {
    double const whole = std::trunc(value);
    double const frac = value - whole;
    if (frac == 0.0)
        return value;
    return whole + std::round(frac / quantum) * quantum;
}

// ENGINE_AUDIT §1.3. setYVelocity below only catches the writes that go
// through the setter; GD also rounds y-velocity inline, and Silicate catches
// that with this midhook -- quantise the value in xmm1 to the finer step and
// jump past GD's own 0.001 rounding. Until 2026-09-27 GucciBot had only the
// setter half, so High TPS Precision did half of what it says.
static void yVelocityRoundMidhook(SafetyHookContext& ctx) {
    double const quantum = yVelocityQuantum();
    if (quantum >= kVanillaQuantum)
        return;
    ctx.xmm1.f64[0] = quantise(ctx.xmm1.f64[0], quantum);
    ctx.rip = geode::base::get() + 0x38c34d;
}

$execute {
    util_midhook(geode::base::get() + 0x38c315, "yVelocityRound", yVelocityRoundMidhook);
}

// Congregation slope diagnostic, see engine_updater.cpp.
namespace gucci::slopediag {
    extern int g_postCalls;
    void log(std::string const& line);
}

class $modify(GB7PlayerObject, PlayerObject) {
    // Splits one physics tick into sub-steps so an input can land BETWEEN
    // them, which is what makes sub-tick (CBF) frame windows mean anything.
    // Ported from Silicate 2026-09-20. Without it the CBF engine still armed
    // and the analyzer still reported sub-tick numbers, but no tick was ever
    // actually split -- every "sub-tick" leg was really a whole-frame leg, so
    // the fractions it printed were measuring nothing.
    //
    // Only player 1 drives this; player 2 is stepped from inside the same
    // loop, and m_p2Handled tells the p2 update to stand down for that tick.
    // anticroom's CBF split, as of his 2026-09-26 source drop.
    //
    // The tick used to be carved into a queue of sub-steps that this function
    // popped one at a time. It is now split exactly once, at the fraction of
    // the tick the input actually landed on: advance by `lead`, settle
    // collisions, fire the input, advance by the `rest`. One split point per
    // tick is all an input ever needed, and it removes a loop whose exit
    // condition depended on two separate engine flags.
    bool cbfSplitUpdate(float stepDelta) {
        auto* eng = cbf::Engine::get();
        // GJBaseGameLayer rather than PlayLayer, so the split also runs in an
        // editor playtest. checkCollisions lives on GJBaseGameLayer anyway.
        auto* pl = GJBaseGameLayer::get();
        if (!pl || this != pl->m_player1)
            return false;
        if (eng->m_midStep)
            return false;

        eng->m_p2Handled = false;

        if (!eng->beginTick()) {
            eng->m_p1Split = false;
            eng->m_p2Split = false;
            return false;
        }

        // Player 2 only exists for this purpose in dual mode.
        PlayerObject* p2 = pl->m_gameState.m_isDualMode ? pl->m_player2 : nullptr;
        bool const tickGround = SLSettings::get()->frameWindow.cbfTickGround;

        eng->m_p1Split = cbf::canSplit(this, tickGround);
        eng->m_p2Split = cbf::canSplit(p2, tickGround);  // false for a null p2
        eng->m_p1Pos = this->getPosition();
        eng->m_p2Pos = p2 ? p2->getPosition() : cocos2d::CCPoint{};

        bool const p1OnGround = this->m_isOnGround;
        bool const p2OnGround = p2 && p2->m_isOnGround;

        // Upstream's note, kept: this stays as two multiplies. Deriving `rest`
        // as stepDelta - lead drifts by a float ulp and desyncs old macros.
        float const lead = stepDelta * static_cast<float>(eng->fraction());
        float const rest = stepDelta * static_cast<float>(1.0 - eng->fraction());

        eng->m_midStep = true;
        eng->m_shipRotAccum = 0.f;
        eng->m_shipRotAccumP2 = 0.f;
        eng->m_shipRotHeld = true;

        auto const settle = [&](PlayerObject* p, bool wasOnGround) {
            // GD drops ground contact after the first half of the tick.
            if (tickGround && ((p->m_yVelocity < 0) ^ p->m_isUpsideDown))
                p->m_isOnGround = wasOnGround;

            // On a slope the collision pass needs the full step, off one it
            // needs none -- same rule as before, now in one place.
            bool const slope = p->m_isOnSlope && !p->m_isDart;
            pl->checkCollisions(p, slope ? stepDelta : 0.f, true);
            p->updateRotation(lead);
            cbf::resetCollisionLog(p);
        };

        eng->m_rotationDelta = lead;
        if (eng->m_p1Split) {
            PlayerObject::update(lead);
            settle(this, p1OnGround);
        }
        if (eng->m_p2Split) {
            p2->update(lead);
            settle(p2, p2OnGround);
        }

        eng->fire();

        eng->m_rotationDelta = rest;
        PlayerObject::update(eng->m_p1Split ? rest : stepDelta);
        if (p2)
            p2->update(eng->m_p2Split ? rest : stepDelta);

        eng->m_shipRotHeld = false;
        if (eng->m_shipRotAccum != 0.f) {
            PlayerObject::updateShipRotation(eng->m_shipRotAccum);
            eng->m_shipRotAccum = 0.f;
        }
        if (p2 && eng->m_shipRotAccumP2 != 0.f) {
            p2->updateShipRotation(eng->m_shipRotAccumP2);
            eng->m_shipRotAccumP2 = 0.f;
        }

        eng->m_p2Handled = p2 != nullptr;
        eng->endTick();  // also clears m_midStep
        return true;
    }

    void update(float dt) {
        auto& upd = GucciEngine::get()->updater;
        auto& traj = TrajectoryPredictionService::get();
        bool const real = !traj.ownsPreviewPlayer(this);

        if (real)
            upd.m_lastPlayerX = upd.m_currentPlayerX;

        // Player 2 was already stepped inside player 1's split loop this tick.
        if (auto* eng = cbf::Engine::get(); eng->m_p2Handled && !eng->m_midStep) {
            if (auto* pl = PlayLayer::get(); pl && this == pl->m_player2) {
                eng->m_p2Handled = false;
                return;
            }
        }

        // Simulated players are never split: a fork exists to answer one
        // question about physics, and splitting its ticks would change the
        // physics it is being asked about.
        if (tickprobe::armed && real) {
            if (auto* ppl = PlayLayer::get(); ppl && this == ppl->m_player1)
                tickprobe::lastDt += dt;
        }
        if (real && this->cbfSplitUpdate(dt)) {
            upd.m_currentPlayerX = this->getPositionX();
            if (tickprobe::armed) {
                if (auto* ppl = PlayLayer::get(); ppl && this == ppl->m_player1)
                    tickprobe::lastSplit = true;
            }
            return;
        }

        PlayerObject::update(dt);

        if (real)
            upd.m_currentPlayerX = this->getPositionX();
        // Absense's look-ahead steps its copies with the delta the real game
        // used (a no-op when it has no copies).
        if (real && !::Bot::get()->trajectory().isFakePlayer(this))
            ::Bot::get()->trajectory().setDelta(dt);
    }

    // While a tick is split, rotation must advance by the SUB-step delta and
    // measure from the position the tick started at -- otherwise the player
    // visibly spins by a full tick's worth on every sub-step.
    void updateRotation(float t) {
        auto* eng = cbf::Engine::get();
        auto* pl = PlayLayer::get();

        if (pl && !eng->m_midStep && eng->m_p1Split && this == pl->m_player1) {
            PlayerObject::updateRotation(eng->m_rotationDelta);
            this->m_lastPosition = eng->m_p1Pos;
            return;
        }
        if (pl && !eng->m_midStep && eng->m_p2Split && this == pl->m_player2) {
            PlayerObject::updateRotation(eng->m_rotationDelta);
            this->m_lastPosition = eng->m_p2Pos;
            return;
        }

        PlayerObject::updateRotation(t);
    }

    // Ship rotation accumulates across the sub-steps and is applied once at
    // the end of the tick, so a split tick rotates the ship exactly as far as
    // an unsplit one would.
    void updateShipRotation(float t) {
        auto* eng = cbf::Engine::get();
        if (eng->m_shipRotHeld) {
            auto* pl = PlayLayer::get();
            if (pl && this == pl->m_player2)
                eng->m_shipRotAccumP2 += t;
            else
                eng->m_shipRotAccum += t;
            return;
        }

        PlayerObject::updateShipRotation(t);
    }

    // Diagnostic only. GD decides a slope launch here: when m_isOnSlope and
    // m_wasOnSlope differ and the player is now off the slope, it stamps
    // m_slopeEndTime and moves the slope velocity into y-velocity.
    void postCollision(float dt, bool betweenSteps) {
        auto* pl = PlayLayer::get();
        bool const p1 = pl && this == pl->m_player1;
        bool const edge = p1 && m_isOnSlope != m_wasOnSlope;
        if (p1)
            gucci::slopediag::g_postCalls++;
        if (edge) {
            auto* gb = GucciEngine::get();
            gucci::slopediag::log(fmt::format(
                "  {}-PC f={} in: is={} was={} dart={} between={} dt={:.4f} et={:.3f}",
                gb->analyzerOwnsRun() ? "CALC" : "PLAY", gb->updater.getFrame(),
                m_isOnSlope ? 1 : 0, m_wasOnSlope ? 1 : 0, m_isDart ? 1 : 0,
                betweenSteps ? 1 : 0, dt, m_slopeEndTime));
        }
        PlayerObject::postCollision(dt, betweenSteps);
        if (edge) {
            auto* gb = GucciEngine::get();
            gucci::slopediag::log(fmt::format(
                "  {}-PC f={} out: is={} was={} et={:.3f} ys={:.3f} sVel={:.3f} curS={}",
                gb->analyzerOwnsRun() ? "CALC" : "PLAY", gb->updater.getFrame(),
                m_isOnSlope ? 1 : 0, m_wasOnSlope ? 1 : 0, m_slopeEndTime,
                m_yVelocity, (double)m_slopeVelocity, m_currentSlope ? 1 : 0));
        }
    }

    void playDeathEffect() {
        auto& upd = GucciEngine::get()->updater;
        if (upd.m_preventDeath || upd.m_predicting)
            return;
        PlayerObject::playDeathEffect();
    }

    void playSpawnEffect() {
        if (GucciEngine::get()->practiceFix.m_loadCheckpoint)
            return;
        if (::Bot::get()->frameWindow().hideSpawnEffects())
            return;
        PlayerObject::playSpawnEffect();
    }

    void spawnCircle() {
        if (::Bot::get()->frameWindow().hideSpawnEffects())
            return;
        PlayerObject::spawnCircle();
    }

    void setYVelocity(double velocity, int type) {
        double const quantum = yVelocityQuantum();
        if (quantum >= kVanillaQuantum)
            return PlayerObject::setYVelocity(velocity, type);
        m_yVelocity = quantise(velocity, quantum);
    }

    // A fake player drawn for a trajectory preview must not count toward the
    // real attempt's jump tally.
    void incrementJumps() {
        if (TrajectoryPredictionService::get().ownsPreviewPlayer(this))
            return;
        // Nor a jump by a copy in Absense's look-ahead.
        if (auto& t = ::Bot::get()->trajectory(); t.drawing() || t.simulating())
            return;
        PlayerObject::incrementJumps();
    }

    // Level flipping is a gameplay effect; in the editor it fights the
    // editor's own camera handling.
    bool levelFlipping() {
        if (LevelEditorLayer::get())
            return false;
        return PlayerObject::levelFlipping();
    }

    // Pending checkpoints are GD's own deferred-placement path. The practice
    // fix places and restores checkpoints itself, so letting GD drop one from
    // under it desyncs the two. Silicate suppresses this outright.
    void removePendingCheckpoint() {
        return;
    }

    // Replaces GD's placement with a timeout, so holding the key does not
    // spray checkpoints. 0.2s in quick mode, 1s otherwise, measured on the
    // game state's own clock rather than real time.
    void tryPlaceCheckpoint() {
        if (!GameManager::get()->getGameVariable("0027"))
            return;
        double const timeout = this->m_quickCheckpointMode ? 0.2 : 1.0;
        if ((this->m_gameLayer->m_gameState.m_totalTime - this->m_lastCheckpointTime) > timeout) {
            this->m_gameLayer->m_uiLayer->onCheck(nullptr);
            this->m_shouldTryPlacingCheckpoint = false;
            this->m_lastCheckpointTime = this->m_totalTime;
        }
    }

    void releaseAllButtons() {
        auto* gb = GucciEngine::get();
        if (gb->updater.m_canDie || gb->isPlaying())
            PlayerObject::releaseAllButtons();
    }
};
