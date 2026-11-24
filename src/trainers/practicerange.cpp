#include <Geode/Geode.hpp>
#include "core/geode_compat.hpp" // GB_PLAYLAYER_INIT_PARAMS
#include <Geode/modify/PlayLayer.hpp>
#include "core/GucciBot.hpp"
#include "core/level_nodes.hpp"

using namespace geode::prelude;

using namespace gucci;

class PracticeRangeOverlay {
public:
    static PracticeRangeOverlay* get() {
        static PracticeRangeOverlay inst;
        return &inst;
    }

    // The node hangs in this level. It is held, not only pointed at, and used
    // only while it's in the current level: GD's Edit exits close a level
    // without PlayLayer::onQuit (core/level_nodes.hpp).
    bool attachedTo(PlayLayer* pl) const {
        return levelnodes::inLevel(m_node, pl);
    }

    // Made in every level, and again whenever the one held isn't in this
    // level (that one is dropped first, as the missed quit would have).
    void attach(PlayLayer* pl) {
        if (!pl || !pl->m_objectLayer || attachedTo(pl))
            return;
        if (m_node)
            levelnodes::noteStale("Practice Range", m_staleLogged);
        detach();
        auto* node = CCDrawNode::create();
        node->setID("practice-range"_spr);
        node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
        node->m_bUseArea = false;
        m_node = levelnodes::hold(node, pl->m_objectLayer, 1402);
    }

    void detach() {
        levelnodes::drop(m_node);
        m_clicks.clear();
        m_macroSig = 0;
        m_speedInit = false;
    }

    void render(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl)
            return;
        if (!attachedTo(pl)) {
            attach(pl);
            if (!attachedTo(pl))
                return;
        }

        if (!gb->practiceRangeEnabled || gb->fwAnalyzing || !pl->m_started) {
            if (m_node)
                m_node->clear();
            return;
        }

        auto& actions = gb->replay.m_actionAtom.m_actions;
        if (actions.empty()) {
            if (m_node)
                m_node->clear();
            return;
        }

        auto* p1 = pl->m_player1;
        if (!p1) {
            if (m_node)
                m_node->clear();
            return;
        }

        uint32_t curFrame = gb->updater.getFrame();
        float playerX = p1->m_position.x;

        size_t sig = actions.size();
        if (sig != m_macroSig) {
            buildClicks(actions);
            m_macroSig = sig;
            m_speedInit = false;
        }

        if (m_speedInit && curFrame + 2 < m_lastFrame) {
            for (auto& c : m_clicks)
                c.sampled = false;
            m_speedInit = false;
        }

        for (auto& c : m_clicks) {
            if (!c.sampled && curFrame >= c.pressFrame) {
                c.x = playerX;
                c.sampled = true;
            }
        }

        if (!m_speedInit) {
            m_lastFrame = curFrame;
            m_lastPlayerX = playerX;
            m_speed = 0.f;
            m_speedInit = true;
        } else if (curFrame != m_lastFrame) {
            float inst = (playerX - m_lastPlayerX) / std::max(1u, curFrame - m_lastFrame);
            m_speed = 0.5f * m_speed + 0.5f * inst;
            m_lastFrame = curFrame;
            m_lastPlayerX = playerX;
        }

        m_node->clear();
        constexpr float yLow = -1000.f, yHigh = 2000.f;
        const ccColor4F faint{0.20f, 0.85f, 0.25f, 0.28f};
        const ccColor4F bright{0.10f, 1.00f, 0.20f, 0.85f};

        for (auto const& c : m_clicks) {
            float x = c.sampled ? c.x
                                : playerX + static_cast<float>(static_cast<int64_t>(c.pressFrame) -
                                                               static_cast<int64_t>(curFrame)) *
                                                m_speed;
            bool held = (curFrame >= c.pressFrame && curFrame < c.releaseFrame);
            uint32_t holdFrames =
                (c.releaseFrame != UINT32_MAX) ? (c.releaseFrame - c.pressFrame) : 0u;
            float thickness = std::clamp(4.f + holdFrames * 0.35f, 4.f, 34.f);
            m_node->drawSegment(
                CCPoint(x, yLow), CCPoint(x, yHigh), thickness, held ? bright : faint);
        }

        const ccColor4F playhead{1.f, 1.f, 1.f, 0.85f};
        m_node->drawSegment(CCPoint(playerX, yLow), CCPoint(playerX, yHigh), 1.f, playhead);
    }

private:
    struct Click {
        uint32_t pressFrame = 0;
        uint32_t releaseFrame = UINT32_MAX;
        float x = 0.f;
        bool sampled = false;
    };

    void buildClicks(const std::vector<gb::Action>& actions) {
        m_clicks.clear();
        for (size_t i = 0; i < actions.size(); ++i) {
            auto const& a = actions[i];
            if (a.m_type != gb::ActionType::Jump || !a.m_holding)
                continue;
            uint32_t release = UINT32_MAX;
            for (size_t j = i + 1; j < actions.size(); ++j) {
                auto const& b = actions[j];
                if (b.m_type == gb::ActionType::Jump && !b.m_holding &&
                    b.m_player2 == a.m_player2) {
                    release = b.m_frame;
                    break;
                }
            }
            m_clicks.push_back({a.m_frame, release, 0.f, false});
        }
    }

    CCDrawNode* m_node = nullptr;  // held (retained) while set
    int m_staleLogged = 0;
    std::vector<Click> m_clicks;
    size_t m_macroSig = 0;
    uint32_t m_lastFrame = 0;
    float m_lastPlayerX = 0.f;
    float m_speed = 0.f;
    bool m_speedInit = false;
};

class $modify(PracticeRangePlayLayer, PlayLayer) {
    bool init(GB_PLAYLAYER_INIT_PARAMS) {
        if (!PlayLayer::init(GB_PLAYLAYER_INIT_ARGS))
            return false;
        PracticeRangeOverlay::get()->attach(this);
        return true;
    }
    void onQuit() {
        PracticeRangeOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};

namespace gucci::gbpr {
    void renderPracticeRange(PlayLayer* pl) {
        PracticeRangeOverlay::get()->render(pl);
    }
} // namespace gbpr
