#include "jupiterghost.hpp"
#include "GucciBot.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

// Mirrors PracticeRangeOverlay (practicerange.cpp) exactly on purpose: a
// CCDrawNode attached to m_objectLayer, cleared and redrawn every frame,
// attached on PlayLayer::init and detached on PlayLayer::onQuit. That
// pattern is already proven safe in this codebase for per-frame gameplay
// overlays -- reusing it here means this feature can't touch anything it
// isn't explicitly reading (m_pathSamples, a locally-tracked best-attempt
// path), unlike the checkpoint/reset system flagged elsewhere as fragile.
class JupiterGhostOverlay {
public:
    static JupiterGhostOverlay* get() { static JupiterGhostOverlay inst; return &inst; }

    void attach(PlayLayer* pl) {
        if (m_node || !pl) return;
        auto* anchor = pl->m_objectLayer;
        if (!anchor) return;
        auto* node = CCDrawNode::create();
        node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        node->m_bUseArea = false;
        anchor->addChild(node, 1403);
        m_node = node;
    }

    void detach() {
        if (m_node) { m_node->removeFromParent(); m_node = nullptr; }
        m_liveAttemptPath.clear();
    }

    void render(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!gb || !pl) return;
        if (!m_node) { attach(pl); if (!m_node) return; }
        m_node->clear();
        if (!pl->m_started) return;

        // Capture this attempt's live path for the "own best attempt" ghost.
        // Only during real manual play -- bot playback is just replaying the
        // same macro we're already comparing against, so capturing it would
        // be redundant at best and would corrupt "your own best" at worst.
        if (gb->jupiterBestGhostEnabled && !gb->isPlaying() && pl->m_player1) {
            m_liveAttemptPath.push_back({ pl->m_player1->m_position.x, pl->m_player1->m_position.y });
        }

        if (gb->jupiterGhostEnabled && !gb->replay.m_pathSamples.empty()) {
            uint32_t frame = resolveFrame(gb, gb->replay.m_pathSamples.size());
            if (frame < gb->replay.m_pathSamples.size()) {
                auto const& s = gb->replay.m_pathSamples[frame];
                drawGhost(s.p1x, s.p1y, ccc4f(0.30f, 0.85f, 1.f, 0.65f));
            }
        }

        if (gb->jupiterBestGhostEnabled && !m_bestAttemptPath.empty()) {
            uint32_t frame = resolveFrame(gb, m_bestAttemptPath.size());
            if (frame < m_bestAttemptPath.size()) {
                auto const& p = m_bestAttemptPath[frame];
                drawGhost(p.first, p.second, ccc4f(1.f, 0.75f, 0.20f, 0.65f));
            }
        }
    }

    void onAttemptEnded() {
        float reachX = m_liveAttemptPath.empty() ? 0.f : m_liveAttemptPath.back().first;
        if (reachX > m_bestReachX) {
            m_bestReachX = reachX;
            m_bestAttemptPath = m_liveAttemptPath;
        }
        m_liveAttemptPath.clear();
    }

private:
    static uint32_t resolveFrame(GucciEngine* gb, size_t pathLen) {
        if (gb->jupiterScrubActive)
            return (uint32_t)std::clamp(gb->jupiterScrubPercent / 100.f * (float)pathLen, 0.f, (float)(pathLen > 0 ? pathLen - 1 : 0));
        return gb->updater.getFrame();
    }

    void drawGhost(float x, float y, ccColor4F col) {
        m_node->drawDot(CCPoint(x, y), 9.f, col);
    }

    CCDrawNode* m_node = nullptr;
    std::vector<std::pair<float,float>> m_liveAttemptPath;
    std::vector<std::pair<float,float>> m_bestAttemptPath;
    float m_bestReachX = 0.f;
};

class $modify(JupiterGhostPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        JupiterGhostOverlay::get()->attach(this);
        return true;
    }
    void onQuit() {
        JupiterGhostOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};

namespace gbju {
    void renderJupiterGhost(PlayLayer* pl) {
        JupiterGhostOverlay::get()->render(pl);
    }
    void notifyJupiterAttemptEnded() {
        JupiterGhostOverlay::get()->onAttemptEnded();
    }
}
