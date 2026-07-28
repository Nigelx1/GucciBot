#pragma once
// action_types.hpp — GucciBot 10.0
#include <cstdint>
#include <vector>
#include <algorithm>

namespace gb {

enum class ActionType : uint8_t {
    Jump = 1, Left = 2, Right = 3,
    Death = 10, Restart = 11, RestartFull = 12,
    TPS = 20,
};

struct Action {
    uint32_t   m_frame   = 0;
    ActionType m_type    = ActionType::Jump;
    bool       m_holding = false;
    bool       m_player2 = false;
    double     m_tps     = 0.0;

    bool operator<(const Action& o) const { return m_frame < o.m_frame; }
    bool operator>(const Action& o) const { return m_frame > o.m_frame; }
    bool isInput() const { return (uint8_t)m_type <= 3; }
};

struct ActionAtom {
    std::vector<Action> m_actions;

    size_t length() const { return m_actions.size(); }
    bool   empty()  const { return m_actions.empty(); }
    void   clear()        { m_actions.clear(); }

    // Returns true if the action was added, false if it was a deduped duplicate.
    bool addAction(uint32_t frame, ActionType type, bool holding, bool player2) {
        // v10.1: drop exact-duplicate inputs. The recording path records each
        // press both in handleButton and in the queued-button flush, producing
        // two identical actions. Same (frame,type,holding,player2) is never a
        // real repeat. Scan back through all actions AT THIS FRAME (not just the
        // immediately-previous one) since another action can land between the two
        // records of the same press. Non-input actions (TPS etc.) are unaffected.
        if ((uint8_t)type <= 3) {
            for (auto it = m_actions.rbegin(); it != m_actions.rend(); ++it) {
                if (it->m_frame != frame) break;  // past this frame; stop scanning
                if (it->m_type == type && it->m_holding == holding &&
                    it->m_player2 == player2) {
                    return false;  // duplicate input at same frame — skip
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
        m_actions.erase(
            std::remove_if(m_actions.begin(), m_actions.end(),
                [frame](const Action& a){ return a.m_frame >= frame; }),
            m_actions.end());
    }
};

} // namespace gb
