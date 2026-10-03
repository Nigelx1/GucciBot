#pragma once

// Autoclicker. 2026-10-01: the previous implementation shared code with
// ToastyReplay and was removed when its author withdrew permission. This is a
// fresh, minimal one: per player, hold for holdTicks, release for
// releaseTicks, repeat, while enabled (and, with onlyWhileHolding, only while
// the player holds the button).

#include <vector>

namespace gucci {

    struct Autoclicker {
        static Autoclicker* get() {
            static Autoclicker s_instance;
            return &s_instance;
        }

        struct PlayerSettings {
            bool enabled = true;
            int holdTicks = 1;
            int releaseTicks = 1;
        };

        bool enabled = false;
        bool onlyWhileHolding = false;
        PlayerSettings p1{};
        PlayerSettings p2{};

        struct TickResult {
            std::vector<bool> p1;
            std::vector<bool> p2;
        };

        TickResult processTick() {
            TickResult out;
            if (!enabled)
                return out;
            step(p1, m_state[0], m_userHolding[0], out.p1);
            step(p2, m_state[1], m_userHolding[1], out.p2);
            return out;
        }

        void reset() {
            m_state[0] = m_state[1] = Cycle{};
        }

        void trackUserInput(bool pressed, bool isPlayer2) {
            m_userHolding[isPlayer2 ? 1 : 0] = pressed;
        }

        void syncP2FromP1() { p2 = p1; }

    private:
        struct Cycle {
            int ticksLeft = 0;
            bool down = false;
        };
        Cycle m_state[2];
        bool m_userHolding[2] = {false, false};

        void step(PlayerSettings const& s, Cycle& c, bool userHolding, std::vector<bool>& events) {
            bool const active = s.enabled && (!onlyWhileHolding || userHolding);
            if (!active) {
                if (c.down)
                    events.push_back(false);
                c = Cycle{};
                return;
            }
            if (c.ticksLeft > 0) {
                --c.ticksLeft;
                return;
            }
            c.down = !c.down;
            events.push_back(c.down);
            c.ticksLeft = (c.down ? s.holdTicks : s.releaseTicks) - 1;
            if (c.ticksLeft < 0)
                c.ticksLeft = 0;
        }
    };

} // namespace gucci
