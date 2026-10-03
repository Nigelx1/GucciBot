// Autoclicker (see autoclicker.hpp).
//
// The rhythm is Silicate's (src/assist/autoclicker.cpp, by peony, GPL-3.0,
// the licence GucciBot is under too): press and stay down for holdTicks, let
// go and stay up for releaseTicks; clicksPerHold adds a release and a press
// more on the press tick, each; swift clicks let go on the press tick itself.
// What changed in the port: the clicker hands back what it would press
// (engine_updater.cpp queues it) instead of queueing buttons itself, it acts
// outside recording as well, it stands aside while something else owns the
// buttons, and a fresh start takes the game's own button state as its
// starting point, so the first thing it sends always changes something.
//
// The black orb UFO loop is the exception: see kBlackOrbLoop below.

#include "hacks/autoclicker.hpp"

#include "absense/glue.hpp"
#include "analysis/pathfinder.hpp"
#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>

using namespace geode::prelude;

namespace gucci {

    namespace {

        using Settings = Autoclicker::PlayerSettings;

        // One tick of a fixed pattern, in the terms Absense's scripts use
        // (absense/glue.hpp, ScriptTick): how many presses the tick holds,
        // each after a release when the button is already down, and whether
        // the button is down once the tick is over.
        struct LoopTick {
            int presses;
            bool down;
        };

        // Absent's black orb UFO loop: five ticks, then round again.
        //
        // Absense's own autoclicker was not among the sources this was written
        // from, so these five ticks are a reconstruction, not a port. They are
        // the black orb spam unit of the Absense pathfinder that is ported in
        // this repo (absense/trajectory/trajectory.cpp, orbSpamTick: a press,
        // a release and a press inside one tick, let go on the next, then
        // quiet until the cadence comes round again) at a cadence of five
        // ticks. Check them against Absense's autoclicker; if the real loop
        // differs, this table is all that needs to change.
        constexpr LoopTick kBlackOrbLoop[] = {{2, true}, {0, false}, {0, false}, {0, false}, {0, false}};
        constexpr uint64_t kBlackOrbLoopTicks = std::size(kBlackOrbLoop);
        static_assert(kBlackOrbLoopTicks == 5, "the black orb UFO loop is five ticks long");

        // Which level each player's held jump key went down in (0: not held).
        // keybinds.cpp only reports keys while a level is open, so a key let go
        // of on the way out of one is never reported; a hold only counts in
        // the level it began in.
        uintptr_t g_heldIn[2] = {0, 0};

        uintptr_t levelId(PlayLayer* pl) {
            return reinterpret_cast<uintptr_t>(pl);
        }

        bool twoPlayerLevel(PlayLayer* pl) {
            return pl && pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
        }

        // Whether the game has that player's jump button down right now.
        bool jumpDown(PlayLayer* pl, bool player2) {
            PlayerObject* p = pl ? (player2 ? pl->m_player2 : pl->m_player1) : nullptr;
            if (!p)
                return false;
            auto const it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            return it != p->m_holdingButtons.end() && it->second;
        }

        // What has the buttons instead of the clicker right now, in words, or
        // nullptr. Calculate's legs and the pathfinders' runs only mean
        // something if nothing else touches the input, and a macro that plays
        // presses its own buttons.
        const char* buttonsOwner() {
            auto* gb = GucciEngine::get();
            if (gb->isPlaying())
                return "a macro is playing";
            if (gb->analyzerOwnsRun())
                return "Calculate is running";
            if (absense::isRunning() || absense::startPending() || Pathfinder::get()->active || gb->fwAnalyzing)
                return "the Pathfinder is running";
            return nullptr;
        }

        // The saved values, per player: "autoclicker_p1_hold_ticks" and so on.
        struct SavedSwitch {
            const char* name;
            bool Settings::*field;
        };
        struct SavedCount {
            const char* name;
            int Settings::*field;
            int most;
        };
        constexpr SavedSwitch kPlayerSwitches[] = {
            {"enabled", &Settings::enabled},
            {"swift_clicks", &Settings::swiftClicks},
            {"black_orb_ufo", &Settings::blackOrbUfo},
        };
        constexpr SavedCount kPlayerCounts[] = {
            {"hold_ticks", &Settings::holdTicks, Autoclicker::kMaxTicks},
            {"release_ticks", &Settings::releaseTicks, Autoclicker::kMaxTicks},
            {"clicks_per_hold", &Settings::clicksPerHold, Autoclicker::kMaxClicksPerHold},
        };

        std::string savedKey(int player, const char* name) {
            return fmt::format("autoclicker_p{}_{}", player, name);
        }

    } // namespace

    Autoclicker::TickResult Autoclicker::processTick() {
        TickResult out;
        auto* pl = PlayLayer::get();
        if (!pl) {
            // Outside a level the clicker holds nothing (as in Silicate).
            reset();
            return out;
        }
        uint64_t const tick = GucciEngine::get()->updater.getFrame();
        bool const busy = buttonsOwner() != nullptr;
        uintptr_t const here = levelId(pl);
        runLane(0, p1, true, m_userHolding[0] && g_heldIn[0] == here, busy, tick, out.p1);
        runLane(1, p2, twoPlayerLevel(pl), m_userHolding[1] && g_heldIn[1] == here, busy, tick, out.p2);
        return out;
    }

    // `applies`: this player's input reaches this player (always for Player
    // 1; for Player 2 only in a two-player level, see the header).
    void Autoclicker::runLane(int index, PlayerSettings const& cfg, bool applies, bool userHolds, bool busy,
                              uint64_t tick, std::vector<bool>& events) {
        Lane& lane = m_lanes[index];

        // Hands off while something else owns the buttons; whatever the
        // clicker had down is theirs now. It starts over when they are done.
        if (busy) {
            lane = Lane{};
            return;
        }

        bool const on = enabled && cfg.enabled && applies && (!onlyWhileHolding || userHolds);
        if (!on) {
            // Switched off, or the player let go of jump: let go too, as
            // Silicate does on switching off, unless the game already has the
            // button up (the player's own release got there first).
            if (lane.down && applies && jumpDown(PlayLayer::get(), index == 1))
                events.push_back(false);
            lane = Lane{};
            return;
        }

        // The frame counter went back without a reset: start over.
        if (lane.lastTick != kNever && tick < lane.lastTick)
            lane = Lane{};
        // A fresh start begins from the button as the game has it: a press
        // when it is up, a release when it is down (held through a respawn,
        // or by the player's own press under "only while holding").
        if (lane.lastTick == kNever)
            lane.down = jumpDown(PlayLayer::get(), index == 1);

        if (cfg.blackOrbUfo)
            blackOrbTick(lane, tick, events);
        else
            rhythmTick(lane, cfg, tick, events);
    }

    void Autoclicker::rhythmTick(Lane& lane, PlayerSettings const& cfg, uint64_t tick, std::vector<bool>& events) {
        // A later switch to the black orb loop starts the loop from its top.
        lane.loopStart = kNever;

        // processTick also runs on ticks where the frame does not move (the
        // player is dead): one decision per tick.
        if (tick == lane.lastTick)
            return;
        if (lane.lastTick != kNever) {
            int const wait = std::max(1, lane.down ? cfg.holdTicks : cfg.releaseTicks);
            if (tick < lane.lastTick + static_cast<uint64_t>(wait))
                return;
        }
        lane.lastTick = tick;

        if (lane.down) {
            events.push_back(false);
            lane.down = false;
            return;
        }

        events.push_back(true);
        int const clicks = std::clamp(cfg.clicksPerHold, 1, kMaxClicksPerHold);
        for (int i = 1; i < clicks; ++i) {
            events.push_back(false);
            events.push_back(true);
        }
        lane.down = true;

        if (cfg.swiftClicks) {
            events.push_back(false);
            lane.down = false;
        }
    }

    void Autoclicker::blackOrbTick(Lane& lane, uint64_t tick, std::vector<bool>& events) {
        if (tick == lane.lastTick)
            return;
        if (lane.loopStart == kNever || tick < lane.loopStart)
            lane.loopStart = tick;
        lane.lastTick = tick;

        LoopTick const& step = kBlackOrbLoop[(tick - lane.loopStart) % kBlackOrbLoopTicks];
        for (int i = 0; i < step.presses; ++i) {
            if (lane.down)
                events.push_back(false);
            events.push_back(true);
            lane.down = true;
        }
        if (lane.down != step.down) {
            events.push_back(step.down);
            lane.down = step.down;
        }
    }

    void Autoclicker::reset() {
        m_lanes[0] = Lane{};
        m_lanes[1] = Lane{};
    }

    void Autoclicker::trackUserInput(bool pressed, bool isPlayer2) {
        int const i = isPlayer2 ? 1 : 0;
        m_userHolding[i] = pressed;
        g_heldIn[i] = pressed ? levelId(PlayLayer::get()) : 0;
    }

    void Autoclicker::syncP2FromP1() {
        // Everything, the on/off included, as Silicate's copy does.
        p2 = p1;
    }

    bool Autoclicker::isClicking(bool player2) const {
        return m_lanes[player2 ? 1 : 0].down;
    }

    const char* Autoclicker::waitingFor() const {
        if (!enabled)
            return nullptr;
        auto* pl = PlayLayer::get();
        if (!pl)
            return "no level is open";
        if (const char* owner = buttonsOwner())
            return owner;
        if (!GucciEngine::get()->enabled)
            return "GucciBot is standing down";

        bool const twoPlayer = twoPlayerLevel(pl);
        bool const p2Clicks = twoPlayer && p2.enabled;
        if (!p1.enabled && !p2Clicks)
            return twoPlayer ? "both players are switched off" : "Player 1 is switched off";

        if (onlyWhileHolding) {
            uintptr_t const here = levelId(pl);
            bool const p1Held = p1.enabled && m_userHolding[0] && g_heldIn[0] == here;
            bool const p2Held = p2Clicks && m_userHolding[1] && g_heldIn[1] == here;
            if (!p1Held && !p2Held)
                return "jump is not held";
        }
        return nullptr;
    }

    void Autoclicker::loadSettings() {
        auto* mod = Mod::get();
        enabled = mod->getSavedValue<bool>("autoclicker_enabled", enabled);
        onlyWhileHolding = mod->getSavedValue<bool>("autoclicker_only_while_holding", onlyWhileHolding);
        PlayerSettings* players[] = {&p1, &p2};
        for (int i = 0; i < 2; ++i) {
            PlayerSettings& s = *players[i];
            for (auto const& sw : kPlayerSwitches)
                s.*sw.field = mod->getSavedValue<bool>(savedKey(i + 1, sw.name), s.*sw.field);
            for (auto const& count : kPlayerCounts) {
                int64_t const v = mod->getSavedValue<int64_t>(savedKey(i + 1, count.name), s.*count.field);
                s.*count.field = static_cast<int>(std::clamp<int64_t>(v, 1, count.most));
            }
        }
    }

    void Autoclicker::saveSettings() const {
        auto* mod = Mod::get();
        mod->setSavedValue<bool>("autoclicker_enabled", enabled);
        mod->setSavedValue<bool>("autoclicker_only_while_holding", onlyWhileHolding);
        PlayerSettings const* players[] = {&p1, &p2};
        for (int i = 0; i < 2; ++i) {
            for (auto const& sw : kPlayerSwitches)
                mod->setSavedValue<bool>(savedKey(i + 1, sw.name), players[i]->*sw.field);
            for (auto const& count : kPlayerCounts)
                mod->setSavedValue<int64_t>(savedKey(i + 1, count.name), players[i]->*count.field);
        }
    }

} // namespace gucci

$on_mod(Loaded) {
    gucci::Autoclicker::get()->loadSettings();
}
