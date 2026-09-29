#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>

namespace gucci {

    namespace gb {

        enum class ActionType : uint8_t {
            Jump = 1,
            Left = 2,
            Right = 3,
            Death = 10,
            Restart = 11,
            RestartFull = 12,
            TPS = 20,
        };

        struct Action {
            uint32_t m_frame = 0;
            ActionType m_type = ActionType::Jump;
            bool m_holding = false;
            bool m_player2 = false;
            double m_tps = 0.0;
            // Where inside its tick the input landed, 0 <= m_subtick < 1 --
            // anticroom's SCBF (sub-tick CBF). 0 is the tick edge, which is
            // what every input was before, and what every macro without the
            // GBR6 sub-tick section loads as. Playback arms the CBF engine
            // from it (processReplayAction), so the input fires this far into
            // the physics step instead of at its start. Last so every brace
            // initialiser in the codebase still means what it meant.
            double m_subtick = 0.0;

            bool operator<(const Action& o) const {
                return m_frame < o.m_frame;
            }
            bool operator>(const Action& o) const {
                return m_frame > o.m_frame;
            }
            bool isInput() const {
                return (uint8_t)m_type <= 3;
            }
        };

        struct ActionAtom {
            std::vector<Action> m_actions;

            size_t length() const {
                return m_actions.size();
            }
            bool empty() const {
                return m_actions.empty();
            }
            void clear() {
                m_actions.clear();
            }

            bool addAction(uint32_t frame, ActionType type, bool holding, bool player2) {
                // A button event that repeats that button's previous event on
                // the same frame says nothing new, and is dropped. Only the
                // previous one: this used to drop it if the same event was
                // anywhere on the frame, and release, press, release -- a tap
                // from a held button, which the Absense pathfinder plays all
                // the time (and its orb spam is press, release, press) -- lost
                // its last event. The game ended the tick with the button up,
                // the macro with it down, and the replay died (Zafari 2,
                // 2026-09-28: tick 1658, a replay death at 2154). Silicate
                // records every event.
                if ((uint8_t)type <= 3) {
                    for (auto it = m_actions.rbegin(); it != m_actions.rend(); ++it) {
                        if (it->m_frame != frame)
                            break;
                        if (it->m_type == type && it->m_player2 == player2) {
                            if (it->m_holding == holding)
                                return false;
                            break;
                        }
                    }
                }
                m_actions.push_back({frame, type, holding, player2, 0.0});
                return true;
            }
            void addTpsChange(uint32_t frame, double tps) {
                m_actions.push_back({frame, ActionType::TPS, false, false, tps});
            }
            void clipFrom(uint32_t frame) {
                m_actions.erase(std::remove_if(m_actions.begin(),
                                               m_actions.end(),
                                               [frame](const Action& a) {
                                                   return a.m_frame >= frame;
                                               }),
                                m_actions.end());
            }
        };

    } // namespace gb

} // namespace gucci
