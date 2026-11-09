// See legit.hpp. GucciBot (2026-11-09): Nigel's "Legit only" switch.
//
// The set held off is safe mode's own list (hacks/safemode.cpp, cheatingNow
// and showsTheWay), one row here for each thing it reads; a new cheat there
// needs a row here too.

#include "hacks/legit.hpp"

#include "absense/glue.hpp"
#include "analysis/ac/framewindow.hpp"
#include "analysis/pathfinder.hpp"
#include "core/GucciBot.hpp"
#include "core/bot_switch.hpp"
#include "hacks/autoclicker.hpp"
#include "hacks/hitborders.hpp"
#include "trainers/click_indicators.hpp"
#include "trainers/trainer_core.hpp"
#include "trainers/videomode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace geode::prelude;

namespace gucci::legit {

    namespace {

        constexpr const char* kSaveKey = "legit_only";

        // One setting held off: `engaged` while it's set the cheating way,
        // `hold` keeps its value aside, `clear` sets it the legit way, `give`
        // puts the kept value back. `when`: only held off while that's true
        // (the trainers' own level); always when empty. `addr`: the field,
        // for kept().
        struct Row {
            std::function<bool()> engaged;
            std::function<void()> hold;
            std::function<void()> clear;
            std::function<void()> give;
            std::function<bool()> when;
            void const* addr = nullptr;
            std::function<double()> heldValue;
            bool holding = false;
        };

        template <class T>
        Row field(std::function<T&()> ref, T legit, std::function<bool()> when = {}) {
            auto held = std::make_shared<T>(legit);
            Row r;
            r.engaged = [ref, legit] { return ref() != legit; };
            r.hold = [ref, held] { *held = ref(); };
            r.clear = [ref, legit] { ref() = legit; };
            r.give = [ref, held] { ref() = *held; };
            r.when = std::move(when);
            r.addr = &ref();
            r.heldValue = [held] { return static_cast<double>(*held); };
            return r;
        }

        // A switch kept behind a getter and a setter (the AI pathfinder's
        // drawings).
        Row toggle(std::function<bool()> get, std::function<void(bool)> set) {
            auto held = std::make_shared<bool>(false);
            Row r;
            r.engaged = get;
            r.hold = [get, held] { *held = get(); };
            r.clear = [set] { set(false); };
            r.give = [set, held] { set(*held); };
            r.heldValue = [held] { return *held ? 1.0 : 0.0; };
            return r;
        }

        std::vector<Row>& rows() {
            static std::vector<Row> v = [] {
                std::vector<Row> v;
                auto* gb = GucciEngine::get();
                // The settings read the first time they're needed, read
                // now, so what's held aside is the saved value and not a
                // default about to be replaced. (The engine's, the
                // autoclicker's and the hitborders' are read at launch.)
                trainers::ensureLoaded();
                videomode::ensureLoaded();
                (void)clickind::settings();
                auto B = [](bool& b) { return std::function<bool&()>([&b]() -> bool& { return b; }); };
                using trainers::Kind;
                // The game: noclip, Prevent death, Force platformer (for the
                // levels opened after; see legit.hpp), the autoclicker.
                v.push_back(field<bool>(B(gb->noclipEnabled), false));
                v.push_back(field<bool>(B(gb->updater.m_preventDeath), false));
                v.push_back(field<bool>(B(gb->hackForcePlatformer), false));
                v.push_back(field<bool>(B(Autoclicker::get()->enabled), false));
                // Seeing what a player can't: hitboxes and hitborders while
                // you play (their Show on death switches stay), layout mode,
                // no mirror effect.
                v.push_back(field<bool>(B(gb->showHitboxes), false));
                v.push_back(field<bool>(B(HitborderOverlay::get()->settings.show), false));
                v.push_back(field<bool>(B(gb->layoutMode), false));
                v.push_back(field<bool>(B(gb->noMirrorEffect), false));
                // What shows the way through the level (safemode.cpp,
                // showsTheWay).
                v.push_back(field<bool>(B(clickind::settings().enabled), false));
                v.push_back(field<bool>(B(gb->pathPreview), false));
                v.push_back(field<bool>(B(gb->survivalIndicator), false));
                v.push_back(field<bool>(B(gb->showMacroPath), false));
                v.push_back(field<bool>(B(gb->practiceRangeEnabled), false));
                for (Kind k : {Kind::Jupiter, Kind::Any}) {
                    auto s = trainers::state(k);
                    v.push_back(field<bool>(B(s.ghost), false));
                    v.push_back(field<bool>(B(s.bestGhost), false));
                    // Only in the trainer's own level: away from it the bar
                    // and Video Mode are how the trainer is used.
                    auto own = [k] { return trainers::followsLevel(k); };
                    v.push_back(field<bool>(B(s.barEnabled), false, own));
                    v.push_back(field<bool>(
                        B(k == Kind::Jupiter ? gb->jupiterVideoModeEnabled : gb->trainerVideoModeEnabled), false,
                        own));
                }
                v.push_back(toggle([] { return absense::levelMapShown(); }, [](bool b) { absense::showLevelMap(b); }));
                v.push_back(toggle([] { return absense::routeShown(); }, [](bool b) { absense::showRoute(b); }));
                // The HUD's Touching orbs and Time warp.
                v.push_back(field<bool>(B(gb->hud.showOrbs), false));
                v.push_back(field<bool>(B(gb->hud.showTimeWarp), false));
                // Physics at 240 TPS, the game at its own speed.
                {
                    Row tps = field<double>(std::function<double&()>([gb]() -> double& { return gb->updater.m_tps; }),
                                            240.0);
                    tps.engaged = [gb] { return std::abs(gb->updater.m_tps - 240.0) > 1e-6; };
                    tps.clear = [gb] { gb->updater.setTps(240.0); };
                    v.push_back(std::move(tps));
                    Row speed = field<double>(
                        std::function<double&()>([gb]() -> double& { return gb->updater.m_speedhack; }), 1.0);
                    speed.engaged = [gb] { return std::abs(gb->updater.m_speedhack - 1.0) > 1e-6; };
                    v.push_back(std::move(speed));
                }
                return v;
            }();
            return v;
        }

        struct State {
            bool loaded = false;
            bool on = false;
        };

        State& st() {
            static State s;
            if (!s.loaded) {
                s.loaded = true;
                s.on = Mod::get()->getSavedValue<bool>(kSaveKey, false);
            }
            return s;
        }

        bool live() {
            auto* gb = GucciEngine::get();
            return !gb->standingDown && !botswitch::takenOut();
        }

        // What isn't a setting: runs and modes. Stopped, never started again.
        void stopRuns() {
            auto* gb = GucciEngine::get();
            if (gb->isPlaying() || gb->isRecording())
                gb->setMode(GucciEngine::Mode::Idle);
            if (::Bot::get()->frameWindow().running())
                ::Bot::get()->frameWindow().cancel();
            if (absense::isRunning())
                absense::stopPathfinder();
            if (Pathfinder::get()->active)
                Pathfinder::get()->cancel();
            if (gb->updater.m_paused)
                gb->updater.setPaused(false);
            // The AI pathfinder's way to the next gate, drawn until replaced.
            if (auto* pl = PlayLayer::get(); pl && pl->m_objectLayer)
                if (auto* leg = pl->m_objectLayer->getChildByID("route-leg"_spr))
                    leg->removeFromParent();
        }

    } // namespace

    bool on() {
        return st().on;
    }

    void setOn(bool want) {
        auto& s = st();
        if (s.on == want)
            return;
        s.on = want;
        Mod::get()->setSavedValue(kSaveKey, want);
        if (!want) {
            // Everything back as it was left (or as last asked for).
            for (auto& r : rows())
                if (r.holding) {
                    r.give();
                    r.holding = false;
                }
            return;
        }
        enforce();
    }

    void enforce() {
        if (!st().on || !live())
            return;
        for (auto& r : rows()) {
            bool const applies = !r.when || r.when();
            if (!applies) {
                // Out of the trainer's own level: the bar or video back.
                if (r.holding) {
                    r.give();
                    r.holding = false;
                }
                continue;
            }
            if (!r.holding) {
                r.hold();
                r.holding = true;
                if (r.engaged())
                    r.clear();
            } else if (r.engaged()) {
                // Turned on some way the gates don't cover (a settings preset
                // loaded, a setting read back late): that's what's kept now.
                r.hold();
                r.clear();
            }
        }
        stopRuns();
    }

    bool kept(bool const& f) {
        if (st().on)
            for (auto const& r : rows())
                if (r.holding && r.addr == &f)
                    return r.heldValue() != 0.0;
        return f;
    }

    double kept(double const& f) {
        if (st().on)
            for (auto const& r : rows())
                if (r.holding && r.addr == &f)
                    return r.heldValue();
        return f;
    }

    bool blockKey() {
        if (!on())
            return false;
        Notification::create("Legit only is on", NotificationIcon::Info, 1.5f)->show();
        return true;
    }

} // namespace gucci::legit

// Every frame, before the scheduler's update steps the game (drawScene runs
// it first). Not from a CCScheduler::update hook: while frame advance holds
// the game, the engine's own hook there (core/engine_updater.cpp) doesn't
// step the rest of the chain, so a pause would never be undone.
class $modify(GBLegitDirector, CCDirector) {
    void drawScene() {
        gucci::legit::enforce();
        CCDirector::drawScene();
    }
};
