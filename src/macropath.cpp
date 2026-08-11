#include "GucciBot.hpp"
#include "action_types.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {

// Marker squares are drawn with an inverted destination-colour blend
// (result = 1 - background), so they stay visible against any terrain
// colour without needing to pick a fixed colour per level.
std::vector<CCPoint> squareVertices(CCPoint center, float half) {
    return {
        ccp(center.x - half, center.y - half), ccp(center.x + half, center.y - half),
        ccp(center.x + half, center.y + half), ccp(center.x - half, center.y + half)
    };
}

}

class MacroPathOverlay {
public:
    static MacroPathOverlay* get() {
        static MacroPathOverlay inst;
        return &inst;
    }

    void attach(PlayLayer* pl) {
        if (m_lineNode || !pl || !pl->m_objectLayer) return;

        auto* lineNode = CCDrawNode::create();
        lineNode->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        lineNode->m_bUseArea = false;
        pl->m_objectLayer->addChild(lineNode, 1600);
        m_lineNode = lineNode;

        auto* markerNode = CCDrawNode::create();
        markerNode->setBlendFunc({ GL_ONE_MINUS_DST_COLOR, GL_ZERO });
        markerNode->m_bUseArea = false;
        pl->m_objectLayer->addChild(markerNode, 1601);
        m_markerNode = markerNode;

        m_builtForCount = SIZE_MAX;
    }

    void detach() {
        if (m_lineNode) { m_lineNode->removeFromParent(); m_lineNode = nullptr; }
        if (m_markerNode) { m_markerNode->removeFromParent(); m_markerNode = nullptr; }
        m_builtForCount = SIZE_MAX;
        m_builtForName.clear();
    }

    void render(PlayLayer* pl) {
        auto* gb = GucciEngine::get();
        if (!pl) return;
        if (!m_lineNode) {
            attach(pl);
            if (!m_lineNode) return;
        }

        auto& samples = gb->replay.m_pathSamples;
        if (!gb->showMacroPath || samples.empty()) {
            if (m_builtForCount != 0) {
                m_lineNode->clear();
                m_markerNode->clear();
                m_builtForCount = 0;
            }
            return;
        }

        if (m_builtForCount == samples.size() && m_builtForName == gb->replayName) return;
        m_builtForCount = samples.size();
        m_builtForName  = gb->replayName;

        rebuild(samples);
    }

private:
    void rebuild(const std::vector<MacroPathSample>& samples) {
        auto* gb = GucciEngine::get();
        m_lineNode->clear();
        m_markerNode->clear();

        bool dual = false;
        for (auto const& s : samples) {
            if (s.hasP2) { dual = true; break; }
        }

        ccColor4F lineColor{ 1.f, 1.f, 1.f, gb->macroPathLineOpacity };
        for (size_t i = 1; i < samples.size(); ++i) {
            m_lineNode->drawSegment({ samples[i - 1].p1x, samples[i - 1].p1y },
                                     { samples[i].p1x, samples[i].p1y }, 1.2f, lineColor);
            if (dual) {
                m_lineNode->drawSegment({ samples[i - 1].p2x, samples[i - 1].p2y },
                                         { samples[i].p2x, samples[i].p2y }, 1.2f, lineColor);
            }
        }

        float half = gb->macroPathMarkerSize * 0.5f;
        auto drawMarker = [&](CCPoint at) {
            auto verts = squareVertices(at, half);
            m_markerNode->drawPolygon(verts.data(), verts.size(),
                ccc4f(1.f, 1.f, 1.f, 1.f), 0.f, ccc4f(0.f, 0.f, 0.f, 0.f));
        };

        for (auto const& a : gb->replay.m_actionAtom.m_actions) {
            if (a.m_type != gb::ActionType::Jump) continue;
            if (a.m_frame >= samples.size()) continue;
            auto const& s = samples[a.m_frame];

            bool isRelease = !a.m_holding;
            char gm = a.m_player2 ? s.gamemode2 : s.gamemode1;
            if (isRelease && !(gm == 'V' || gm == 'R' || gm == 'H')) continue;

            CCPoint at = a.m_player2 ? ccp(s.p2x, s.p2y) : ccp(s.p1x, s.p1y);
            drawMarker(at);
        }
    }

    CCDrawNode* m_lineNode = nullptr;
    CCDrawNode* m_markerNode = nullptr;
    size_t      m_builtForCount = SIZE_MAX;
    std::string m_builtForName;
};

class $modify(MacroPathPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        MacroPathOverlay::get()->attach(this);
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        MacroPathOverlay::get()->render(this);
    }

    void onQuit() {
        MacroPathOverlay::get()->detach();
        PlayLayer::onQuit();
    }
};
